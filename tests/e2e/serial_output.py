#!/usr/bin/env python3
# 串口异步输出的进程级回归：独立 MQTT Broker + 真实 PTY + 网关可执行文件。
# 使用 TCOOFF/TCOON 暂停和恢复 tty 输出，验证实际应用路径中的排队、重试与唤醒。
# 调用：python3 tests/e2e/serial_output.py build/dev/gateway

import contextlib
import os
from pathlib import Path
import pty
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import termios
import time
import tty


def crc16(data):
    """CRC-16/MODBUS；与共享协议相同，覆盖 LEN、TYPE 和 payload。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def encode_frame(kind, payload):
    body = bytes([len(payload) + 1, kind]) + payload
    return b"\xaa\x55" + body + crc16(body).to_bytes(2, "little")


def unused_port():
    """由内核选择本轮测试端口，不连接用户正在运行的 Broker 或 HTTP 服务。"""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Peer:
    """测试节点持有 PTY 主端，并通过真实 MQTT 下发业务命令。"""

    def __init__(self, master, slave, port, process):
        self.master = master
        self.slave = slave
        self.port = port
        self.process = process
        self.buffer = bytearray()  # 跨 read 保存半帧，禁止悄悄跳过错误字节。

    def pause_output(self):
        termios.tcflow(self.slave, termios.TCOOFF)

    def resume_output(self):
        termios.tcflow(self.slave, termios.TCOON)

    def publish(self, name, values):
        subprocess.run(
            ["mosquitto_pub", "-h", "127.0.0.1", "-p", str(self.port),
             "-q", "1", "-t", "gateway/cmd/" + name, "-l"],
            input="".join(str(value) + "\n" for value in values),
            text=True, check=True, timeout=5,
        )

    def receive(self, seconds, count=None):
        """返回 (TYPE, payload, 观察时刻)，以截止时间限制等待并验证全部帧的 CRC。"""
        deadline = time.monotonic() + seconds
        frames = []
        while time.monotonic() < deadline:
            assert self.process.poll() is None, "网关在测试期间异常退出"
            ready, _, _ = select.select([self.master], [], [],
                                         max(0, deadline - time.monotonic()))
            if not ready:
                break
            self.buffer.extend(os.read(self.master, 65536))
            while len(self.buffer) >= 3:
                assert self.buffer[:2] == b"\xaa\x55", "输出出现残留或交错的帧字节"
                length = self.buffer[2]
                assert 1 <= length <= 64, "输出 LEN 不符合共享协议"
                size = length + 5
                if len(self.buffer) < size:
                    break
                frame = bytes(self.buffer[:size])
                del self.buffer[:size]
                assert crc16(frame[2:-2]) == int.from_bytes(frame[-2:], "little"), "CRC 错误"
                frames.append((frame[3], frame[4:-2], time.monotonic()))
            if count is not None and len(frames) >= count:
                break
        return frames

    def take(self, count):
        frames = self.receive(3, count)
        assert len(frames) == count, f"应收到 {count} 帧，实际 {len(frames)} 帧"
        return frames

    def quiet(self, seconds):
        assert not self.receive(seconds), "等待期间出现提前重试或多余命令"
        assert not self.buffer, "等待期间出现未完成帧"

    def write(self, kind, payload):
        data = bytearray(encode_frame(kind, payload))
        deadline = time.monotonic() + 3
        while data:
            assert time.monotonic() < deadline, "写入测试帧超时"
            _, ready, _ = select.select([], [self.master], [], 0.05)
            if ready:
                try:
                    del data[:os.write(self.master, data)]
                except BlockingIOError:
                    pass

    def handshake(self):
        frames = self.take(1)
        kind, session, _ = frames[0]
        assert kind == 0x23 and len(session) == 8, "业务命令越过了握手"
        self.write(0x09, session + session + b"\x00")
        return session

    def commands(self, count, seconds=3):
        deadline = time.monotonic() + seconds
        commands = []
        while time.monotonic() < deadline and len(commands) < count:
            for kind, payload, at in self.receive(min(0.05, max(0, deadline-time.monotonic()))):
                if kind == 0x23:
                    self.write(0x09, payload + payload + b"\x00")
                elif kind == 0x24:
                    commands.append((kind, payload, at))
                else:
                    assert kind == 0x25, f"意外的下行 TYPE: {kind}"
        assert len(commands) == count, f"应收到 {count} 条命令，实际 {len(commands)}"
        return commands

    def reply(self, frames):
        for kind, payload, _ in frames:
            assert kind == 0x24
            data = b"\x00"
            if payload[9] == 0x20: data += b"\x00\x2a"
            if payload[9] == 0x21: data += b"\x00\xe6\x02\x26"
            self.write(0x08, payload[:10] + data)

    def no_commands(self, seconds):
        frames = self.receive(seconds)
        assert all(kind == 0x25 for kind, _, _ in frames), "出现多余命令或会话重启"
        assert not self.buffer


@contextlib.contextmanager
def running_gateway(binary):
    """每个场景使用独立端口、PTY 和数据库，退出时只回收本场景创建的进程。"""
    with tempfile.TemporaryDirectory(prefix="gateway-serial-output-") as directory:
        root = Path(directory)
        mqtt_port = unused_port()
        http_port = unused_port()
        while http_port == mqtt_port:
            http_port = unused_port()
        broker_config = root / "mosquitto.conf"
        broker_config.write_text(
            f"listener {mqtt_port} 127.0.0.1\nallow_anonymous true\npersistence false\n"
        )
        master, slave = pty.openpty()
        tty.setraw(slave)
        os.set_blocking(master, False)
        config = root / "gateway.conf"
        config.write_text(
            f"serial_path={os.ttyname(slave)}\nserial_baud=115200\n"
            f"mqtt_host=127.0.0.1\nmqtt_port={mqtt_port}\nhttp_port={http_port}\n"
            f"db_path={root / 'gateway.db'}\nlog_level=1\n"
        )
        broker_path = root / "broker.log"
        gateway_path = root / "gateway.log"
        with broker_path.open("w") as broker_log, gateway_path.open("w") as gateway_log:
            broker = subprocess.Popen(
                ["mosquitto", "-c", str(broker_config), "-v"],
                stdout=broker_log, stderr=subprocess.STDOUT,
            )
            process = None
            failed = False
            try:
                deadline = time.monotonic() + 5
                while True:
                    assert broker.poll() is None, "测试 Broker 启动失败"
                    try:
                        with socket.create_connection(("127.0.0.1", mqtt_port), timeout=0.1):
                            break
                    except OSError:
                        assert time.monotonic() < deadline, "测试 Broker 启动超时"
                        time.sleep(0.01)
                process = subprocess.Popen([str(binary), str(config)],
                                           stdout=gateway_log, stderr=subprocess.STDOUT)
                # 等到 Broker 已处理实际订阅，防止测试命令先于网关订阅到达。
                while "gateway/cmd/#" not in broker_path.read_text():
                    assert process.poll() is None, "测试网关启动失败"
                    assert time.monotonic() < deadline, "网关 MQTT 订阅超时"
                    time.sleep(0.01)
                peer = Peer(master, slave, mqtt_port, process)
                peer.broker_log = broker_path
                yield peer
            except BaseException:
                failed = True
                raise
            finally:
                termios.tcflow(slave, termios.TCOON)
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                broker.terminate()
                try:
                    broker.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    broker.kill()
                    broker.wait()
                os.close(master)
                os.close(slave)
                if failed or (process is not None and process.returncode != 0):
                    print(gateway_path.read_text()[-6000:], file=sys.stderr)
                    print(broker_path.read_text()[-2000:], file=sys.stderr)
                if not failed and process is not None:
                    assert process.returncode == 0, "网关未正常退出"


def check_batch_and_sequence_resume(binary):
    with running_gateway(binary) as peer:
        peer.pause_output()
        peer.publish("set_period", range(1, 301))
        peer.quiet(0.2)
        peer.resume_output()
        session = peer.handshake()
        received = 0
        first_session = session
        while received < 300:
            count = min(8, 300 - received)
            batch = peer.commands(count)
            assert [p[8] for _, p, _ in batch] == [i % 256 for i in range(received, received + count)]
            assert [int.from_bytes(p[10:], "big") for _, p, _ in batch] == list(range(received + 1, received + count + 1))
            for _, p, _ in batch:
                if received < 256: assert p[:8] == first_session
                else: assert int.from_bytes(p[:8], "big") > int.from_bytes(first_session, "big")
            if received == 0: peer.no_commands(0.1)
            peer.reply(batch)
            received += count
        peer.no_commands(0.7)
    print("PASS SR 发送窗口 8，300 条命令有序分配，排空后换会话再复用序号")


def check_retry_clock(binary):
    with running_gateway(binary) as peer:
        peer.publish("query_light", ["x"])
        peer.handshake()
        first = peer.commands(1)[0]
        peer.pause_output()
        peer.quiet(1.1)
        peer.resume_output()
        retry = peer.commands(1)[0]
        assert retry[:2] == first[:2]
        peer.quiet(0.35)
        next_retry = peer.commands(1)[0]
        assert next_retry[:2] == first[:2]
        assert next_retry[2] - retry[2] >= 0.48
        peer.reply([next_retry])
        peer.no_commands(0.7)
    print("PASS 整帧写完才计时，背压期间只保留一条待发重试")


def check_ack_during_queued_retry(binary):
    with running_gateway(binary) as peer:
        peer.publish("query_light", ["x"] * 8)
        peer.handshake()
        first = peer.commands(8)
        peer.pause_output()
        peer.quiet(0.8)
        peer.reply(first)
        peer.quiet(0.1)
        peer.resume_output()
        peer.no_commands(0.7)
        peer.publish("query_light", ["x"] * 8)
        second = peer.commands(8)
        assert [p[8] for _, p, _ in second] == list(range(8, 16))
        peer.reply(second)
        peer.no_commands(0.2)
    print("PASS 等待串口写出的旧重试被执行结果撤销，窗口正常推进")


def check_expired_output_is_fenced(binary):
    with running_gateway(binary) as peer:
        peer.publish("set_period", [5])
        old_session = peer.handshake()
        first = peer.commands(1)[0]
        peer.pause_output()
        peer.quiet(4.2)
        peer.resume_output()
        new_session = peer.handshake()
        assert new_session > old_session
        peer.no_commands(0.1)
        peer.reply([first])  # 旧会话执行结果不能影响新会话。
        peer.publish("set_period", [10])
        new = peer.commands(1)
        assert new[0][1][:8] == new_session and new[0][1][8] == 0
        peer.reply(new)
        peer.no_commands(0.2)
    print("PASS 超期排队帧不能写出或复活，重新握手后才能发送新命令")


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/dev/gateway").resolve()
    for check in (check_batch_and_sequence_resume, check_retry_clock,
                  check_ack_during_queued_retry, check_expired_output_is_fenced):
        check(binary)


if __name__ == "__main__":
    main()
