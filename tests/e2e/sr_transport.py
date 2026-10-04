#!/usr/bin/env python3
"""真实 gateway + node-sim + MQTT + 双 PTY；中继确定性丢帧、延迟确认。"""
import contextlib
import os
from pathlib import Path
import pty
import subprocess
import sys
import tempfile
import time
import tty
from serial_output import Peer, running_gateway


@contextlib.contextmanager
def running_node(binary, *options):
    with tempfile.TemporaryDirectory(prefix="node-sr-") as directory:
        logfile = Path(directory) / "node.log"
        master, slave = pty.openpty()
        tty.setraw(slave)
        os.set_blocking(master, False)
        with logfile.open("w") as log:
            process = subprocess.Popen(
                [str(binary), os.ttyname(slave), "--period", "300", *options],
                stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                yield Peer(master, slave, 0, process), logfile
            except BaseException:
                print(logfile.read_text(), file=sys.stderr)
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                os.close(master)
                os.close(slave)


@contextlib.contextmanager
def results(peer):
    with tempfile.TemporaryFile(mode="w+") as output:
        sub = subprocess.Popen(
            ["mosquitto_sub", "-h", "127.0.0.1", "-p", str(peer.port), "-q", "1",
             "-t", "gateway/ack/#", "-t", "gateway/resp/#", "-v"],
            stdout=output, stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 3
            while "gateway/ack/#" not in peer.broker_log.read_text():
                assert sub.poll() is None
                assert time.monotonic() < deadline, "结果订阅超时"
                time.sleep(0.01)

            def messages():
                output.seek(0)
                return output.read().splitlines()

            yield messages
        finally:
            sub.terminate()
            sub.communicate(timeout=3)


class Relay:
    def __init__(self, gateway, node, filter_frame):
        self.gateway = gateway
        self.node = node
        self.filter_frame = filter_frame
        self.down = []
        self.up = []

    def pump(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for source, dest, direction, history in (
                (self.gateway, self.node, "down", self.down),
                (self.node, self.gateway, "up", self.up),
            ):
                for kind, p, at in source.receive(0.01):
                    history.append((kind, p, at))
                    if self.filter_frame(direction, kind, p):
                        dest.write(kind, p)

    def until(self, predicate, seconds=5):
        deadline = time.monotonic() + seconds
        while not predicate():
            assert time.monotonic() < deadline, "SR 场景没有在时限内收敛"
            self.pump(0.03)


def check_reorder_and_lost_results(gateway_binary, node_binary):
    with running_gateway(gateway_binary) as gateway, running_node(node_binary) as (node, log), results(gateway) as messages:
        data_count = {}
        result_count = {}
        ack_count = {}
        buffered_without_execution = []

        def faults(direction, kind, p):
            if direction == "down" and kind == 0x24:
                seq = p[8]
                data_count[seq] = data_count.get(seq, 0) + 1
                if seq == 0 and data_count[seq] == 1: return False
            if direction == "up" and kind == 0x07 and p[8] == 1:
                # B 已被接收，但 A 尚未补齐，不允许执行 B 或报告成功。
                buffered_without_execution.append("[命令]" not in log.read_text())
                assert not any(line.endswith(" ok") for line in messages())
            if direction == "up" and kind == 0x08:
                seq = p[8]
                result_count[seq] = result_count.get(seq, 0) + 1
                if seq == 0 and result_count[seq] == 1: return False
            if direction == "down" and kind == 0x25:
                seq = p[8]
                ack_count[seq] = ack_count.get(seq, 0) + 1
                if seq == 1 and ack_count[seq] == 1: return False
            return True

        relay = Relay(gateway, node, faults)
        gateway.publish("set_period", [5, 10])
        relay.until(lambda: len(messages()) == 2 and ack_count.get(1, 0) >= 2 and ack_count.get(0, 0) >= 1)
        relay.pump(0.7)
        assert buffered_without_execution and all(buffered_without_execution)
        assert data_count == {0: 2, 1: 1}, f"已确认的 B 被多余重传: {data_count}"
        execution = [line for line in log.read_text().splitlines() if "[命令]" in line]
        assert len(execution) == 2
        assert "seq=0 设采样周期 = 5 秒" in execution[0]
        assert "seq=1 设采样周期 = 10 秒" in execution[1]
        assert sorted(messages()) == ["gateway/ack/0 ok", "gateway/ack/1 ok"]
    print("PASS 丢 A、缓存 B、只重传 A、按 5→10 执行；结果和结果 ACK 丢失也不重复执行或通知")


def check_unrecoverable_gap(gateway_binary, node_binary):
    with running_gateway(gateway_binary) as gateway, running_node(node_binary) as (node, log), results(gateway) as messages:
        old_session = None
        new_session = None
        old_data = []
        held_open_ack = []
        release_ack = False

        def faults(direction, kind, p):
            nonlocal old_session, new_session
            if direction == "down" and kind == 0x23:
                if old_session is None: old_session = p
                elif p != old_session: new_session = p
            if direction == "down" and kind == 0x24 and p[:8] == old_session:
                old_data.append((kind, p))
                return p[8] != 0  # 永远丢弃 A，B 只能等待。
            if direction == "up" and kind == 0x09 and p[:8] == new_session and not release_ack:
                held_open_ack.append(p)
                return False
            return True

        relay = Relay(gateway, node, faults)
        gateway.publish("set_period", [5, 10])
        relay.until(lambda: bool(held_open_ack) and len(messages()) == 2, 5)
        assert messages() == ["gateway/ack/0 timeout", "gateway/ack/1 session_aborted"]
        assert "[命令]" not in log.read_text()
        assert len([p for _, p in old_data if p[8] == 0]) == 4  # 首发 + 3 次选择重传。
        assert len([p for _, p in old_data if p[8] == 1]) == 1
        gateway.publish("set_period", [20])
        relay.pump(0.2)
        assert not any(k == 0x24 and p[:8] == new_session for k, p, _ in relay.down)
        release_ack = True
        gateway.write(0x09, held_open_ack[-1])
        relay.until(lambda: "gateway/ack/0 ok" in messages())
        for kind, p in old_data: node.write(kind, p)
        node.write(0x23, old_session)
        gateway.write(0x08, old_session + b"\x00\x22\x00")
        relay.pump(0.8)
        execution = [line for line in log.read_text().splitlines() if "[命令]" in line]
        assert len(execution) == 1 and "设采样周期 = 20 秒" in execution[0]
        assert messages().count("gateway/ack/0 ok") == 1
    print("PASS A 重试耗尽取消 A/B，新会话确认前禁止继续下发，迟到旧命令不能覆盖新设置")


def check_failed_handshake(gateway_binary, node_binary):
    with running_gateway(gateway_binary) as gateway, running_node(node_binary) as (node, log), results(gateway) as messages:
        relay = Relay(gateway, node, lambda direction, kind, p: kind != 0x09)
        gateway.publish("set_period", [5, 10])
        relay.until(lambda: len(messages()) == 2, 5)
        relay.pump(0.7)
        assert messages() == ["gateway/ack/rejected link_failed"] * 2
        assert len([p for k, p, _ in relay.down if k == 0x23]) == 4
        assert not any(k == 0x24 for k, _, _ in relay.down)
        assert "[命令]" not in log.read_text()
    print("PASS 握手始终无确认时有限重试后失败，不跳过会话屏障执行命令")


def main():
    gateway_binary = Path(sys.argv[1]).resolve()
    node_binary = Path(sys.argv[2]).resolve()
    for check in (check_reorder_and_lost_results, check_unrecoverable_gap, check_failed_handshake):
        check(gateway_binary, node_binary)


if __name__ == "__main__":
    main()
