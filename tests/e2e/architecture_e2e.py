#!/usr/bin/env python3
"""Real PTY, MQTT broker and HTTP sockets exercise the gateway's async boundaries.

These are bounded functional concurrency checks, not throughput/latency benchmarks.
Only processes, temporary files and ports created by this test are used.
"""

import concurrent.futures
import contextlib
import errno
import http.client
import json
import os
from pathlib import Path
import pty
import platform
import resource
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import termios
import threading
import time
import tty

from serial_output import Peer


def eventually(predicate, description, seconds=6, health=lambda: None):
    deadline = time.monotonic() + seconds
    while True:
        health()
        value = predicate()
        if value:
            return value
        assert time.monotonic() < deadline, description
        time.sleep(0.02)


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=6)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def reserve_port():
    reservation = socket.socket()
    reservation.bind(("127.0.0.1", 0))
    return reservation, reservation.getsockname()[1]


class Node:
    """One reader owns serial parsing; writes are locked across test producers."""

    def __init__(self, peer):
        self.peer = peer
        self.stopping = threading.Event()
        self.hold_results = threading.Event()
        self.write_lock = threading.Lock()
        self.lock = threading.Lock()
        self.commands = []
        self.errors = []
        self.thread = threading.Thread(target=self.run, name="test-node")
        self.thread.start()

    def send(self, kind, payload):
        with self.write_lock:
            self.peer.write(kind, payload)

    def run(self):
        delayed_results = []
        try:
            while not self.stopping.is_set() and self.peer.process.poll() is None:
                for kind, payload, _ in self.peer.receive(0.02):
                    if kind == 0x23:
                        self.send(0x09, payload + payload + b"\x00")
                    elif kind == 0x24:
                        with self.lock:
                            self.commands.append(payload)
                        self.send(0x07, payload[:9])
                        result = b"\x00"
                        if payload[9] == 0x20:
                            result += b"\x00\x2a"
                        elif payload[9] == 0x21:
                            result += b"\x00\xe6\x02\x26"
                        delayed_results.append(payload[:10] + result)
                    else:
                        assert kind == 0x25, f"Unexpected gateway frame: {kind:#x}"
                if not self.hold_results.is_set():
                    for result in delayed_results:
                        self.send(0x08, result)
                    delayed_results.clear()
        except BaseException as error:
            if not self.stopping.is_set() and self.peer.process.poll() is None:
                self.errors.append(error)

    def snapshot(self):
        with self.lock:
            return list(self.commands)

    def close(self):
        self.stopping.set()
        self.thread.join(timeout=4)
        assert not self.thread.is_alive(), "Test node did not stop"


class Gateway:
    def __init__(self, root, process, peer, http_port, config, values):
        self.root = root
        self.process = process
        self.peer = peer
        self.http_port = http_port
        self.config = config
        self.values = values
        self.node = Node(peer)

    def health(self):
        assert self.process.poll() is None, "Gateway exited unexpectedly"
        if self.node.errors:
            raise self.node.errors[0]

    def messages(self):
        return (self.root / "messages.log").read_text().splitlines()

    def wait_message(self, expected):
        eventually(lambda: expected in self.messages(),
                   f"Missing MQTT message: {expected}", health=self.health)

    def request(self, path, method="GET"):
        connection = http.client.HTTPConnection("127.0.0.1", self.http_port, timeout=4)
        try:
            connection.request(method, path)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def rows(self, device, n=500):
        status, _, body = self.request(f"/api/data?dev={device}&n={n}")
        assert status == 200, f"HTTP query failed: {status} {body!r}"
        return json.loads(body)

    def rewrite_config(self, **changes):
        self.values.update(changes)
        candidate = self.config.with_suffix(".new")
        candidate.write_text("".join(f"{key}={value}\n" for key, value in self.values.items()))
        candidate.replace(self.config)
        self.process.send_signal(signal.SIGHUP)

    def shutdown(self):
        self.process.terminate()
        self.process.wait(timeout=6)
        assert self.process.returncode == 0, "Gateway did not shut down cleanly"


@contextlib.contextmanager
def running(binary, fd_limit=None):
    with tempfile.TemporaryDirectory(prefix="gateway-architecture-") as directory:
        root = Path(directory)
        broker = process = subscriber = node = None
        mqtt_socket, mqtt_port = reserve_port()
        http_socket, http_port = reserve_port()
        master, slave = pty.openpty()
        tty.setraw(slave)
        os.set_blocking(master, False)
        config = root / "gateway.conf"
        values = dict(serial_path=os.ttyname(slave), serial_baud=115200,
                      mqtt_host="127.0.0.1", mqtt_port=mqtt_port, mqtt_keepalive=60,
                      http_port=http_port, db_path=root / "first.db", report_n=100,
                      idle_timeout=10, log_level=1)
        config.write_text("".join(f"{key}={value}\n" for key, value in values.items()))
        broker_config = root / "mosquitto.conf"
        broker_config.write_text(f"listener {mqtt_port} 127.0.0.1\n"
                                 "allow_anonymous true\npersistence false\n")
        failed = False
        with contextlib.ExitStack() as resources:
            logs = {name: resources.enter_context((root / name).open("w"))
                    for name in ("broker.log", "gateway.log", "messages.log", "subscriber.log")}
            try:
                mqtt_socket.close()
                broker = subprocess.Popen(["mosquitto", "-c", str(broker_config), "-v"],
                                          stdout=logs["broker.log"], stderr=subprocess.STDOUT)

                def broker_ready():
                    assert broker.poll() is None, "Test broker failed to start"
                    try:
                        with socket.create_connection(("127.0.0.1", mqtt_port), timeout=0.1):
                            return True
                    except OSError:
                        return False

                eventually(broker_ready, "Test broker did not listen")
                # Waiting for this client's actual SUBACK prevents startup packet loss.
                subscriber_id = "architecture-observer"
                subscriber = subprocess.Popen(
                    ["mosquitto_sub", "-h", "127.0.0.1", "-p", str(mqtt_port),
                     "-i", subscriber_id, "-q", "1", "-t", "gateway/#", "-v"],
                    stdout=logs["messages.log"], stderr=logs["subscriber.log"])
                eventually(lambda: f"Sending SUBACK to {subscriber_id}" in
                           (root / "broker.log").read_text(), "Observer subscription failed")
                http_socket.close()
                def limit_child_files():
                    if fd_limit is not None:
                        resource.setrlimit(resource.RLIMIT_NOFILE, (fd_limit, fd_limit))

                process = subprocess.Popen([str(binary), str(config)],
                                           stdout=logs["gateway.log"], stderr=subprocess.STDOUT,
                                           preexec_fn=limit_child_files if fd_limit is not None else None)
                peer = Peer(master, slave, mqtt_port, process)
                app = Gateway(root, process, peer, http_port, config, values)
                node = app.node
                eventually(lambda: "gateway/cmd/#" in (root / "broker.log").read_text(),
                           "Gateway MQTT subscription failed", health=app.health)

                def http_ready():
                    try:
                        return app.request("/")[0] == 200
                    except OSError:
                        return False

                eventually(http_ready, "Gateway HTTP service did not start", health=app.health)
                yield app
            except BaseException:
                failed = True
                raise
            finally:
                peer = node.peer if node else None
                cleanup_error = None
                try:
                    if peer:
                        peer.resume_output()
                    if node:
                        node.close()
                except BaseException as error:
                    cleanup_error = error
                for child in (process, subscriber, broker):
                    try:
                        stop_process(child)
                    except BaseException as error:
                        cleanup_error = cleanup_error or error
                mqtt_socket.close()
                http_socket.close()
                os.close(master)
                os.close(slave)
                if failed or (process is not None and process.returncode != 0):
                    for name in ("gateway.log", "broker.log", "subscriber.log"):
                        logs[name].flush()
                        print(f"{name}:\n{(root / name).read_text()[-7000:]}", file=sys.stderr)
                if not failed and process is not None:
                    assert process.returncode == 0, "Gateway cleanup required forced termination"
                if not failed and cleanup_error:
                    raise cleanup_error


def database_rows(path):
    with sqlite3.connect(f"file:{path}?mode=ro", uri=True, timeout=2) as connection:
        return connection.execute("SELECT device_id,value,ts FROM device_data ORDER BY id").fetchall()


def task_names(pid):
    tasks = {}
    for task in (Path("/proc") / str(pid) / "task").iterdir():
        try:
            tasks[int(task.name)] = (task / "comm").read_text().strip()
        except FileNotFoundError:
            pass  # A library thread can disappear while a snapshot is being taken.
    return tasks


def check_management_thread_ownership(app):
    names = eventually(lambda: (snapshot if len(snapshot := task_names(app.process.pid)) == 9 else None),
                       "Expected nine gateway threads after startup", health=app.health)
    for name in ("gateway-admin", "gateway-manage", "gateway-core", "gateway-query", "gateway-http"):
        assert name in names.values(), f"Missing thread {name}: {names}"
    admin_tid = next(tid for tid, name in names.items() if name == "gateway-admin")
    assert admin_tid != app.process.pid, "Management Reactor runs in the serial/main thread"
    managed_mask = sum(1 << (number - 1) for number in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT))
    for tid, name in names.items():
        status = (Path("/proc") / str(app.process.pid) / "task" / str(tid) / "status").read_text()
        blocked = next(int(line.split()[1], 16) for line in status.splitlines()
                       if line.startswith("SigBlk:"))
        assert blocked & managed_mask == managed_mask, f"{name}/{tid} did not inherit managed signal mask"

    proc = Path("/proc") / str(app.process.pid)
    descriptors = {}
    for fd in (proc / "fd").iterdir():
        try:
            descriptors[int(fd.name)] = fd.readlink().as_posix()
        except FileNotFoundError:
            pass
    signal_fds = {fd for fd, target in descriptors.items() if target == "anon_inode:[signalfd]"}
    serial_fds = {fd for fd, target in descriptors.items() if target == app.values["serial_path"]}
    assert len(signal_fds) == 1 and serial_fds, f"Unexpected signal/serial fds: {descriptors}"
    epolls = {}
    for fd, target in descriptors.items():
        if target == "anon_inode:[eventpoll]":
            epolls[fd] = {int(line.split()[1]) for line in (proc / "fdinfo" / str(fd)).read_text().splitlines()
                          if line.startswith("tfd:")}
    signal_owners = [fd for fd, watched in epolls.items() if watched & signal_fds]
    serial_owners = [fd for fd, watched in epolls.items() if watched & serial_fds]
    assert len(signal_owners) == 1, f"signalfd must belong to exactly one epoll: {epolls}"
    assert len(serial_owners) == 1, f"Serial must belong to exactly one epoll: {epolls}"
    assert signal_owners[0] != serial_owners[0], "Main serial epoll still owns signalfd"

    # syscall exposure depends on kernel ptrace policy. fdinfo topology and masks above
    # remain mandatory; when permitted, identify the actual thread waiting on each epoll.
    epoll_calls = {"x86_64": {232, 281, 441}, "aarch64": {22, 441},
                   "armv7l": {252, 346, 441}}.get(platform.machine())
    if epoll_calls is None:
        return
    for tid, expected_fd in ((admin_tid, signal_owners[0]), (app.process.pid, serial_owners[0])):
        unavailable = []

        def waiting_on_expected_epoll():
            try:
                call = (proc / "task" / str(tid) / "syscall").read_text().split()
            except PermissionError:
                unavailable.append(True)
                return True
            return (len(call) >= 2 and call[0] != "running" and int(call[0]) in epoll_calls
                    and int(call[1], 0) == expected_fd)

        eventually(waiting_on_expected_epoll, f"Thread {tid} does not wait on its expected epoll",
                   health=app.health)
        if unavailable:
            print("NOTE /proc task syscall restricted; verified thread masks and separate fdinfo registrations")
            break


@contextlib.contextmanager
def blocked_config_reload(app):
    """Hold the real config reader on a FIFO; always restore its input before cleanup."""
    contents = app.config.read_bytes()
    writer = None
    app.config.unlink()
    os.mkfifo(app.config)
    try:
        app.process.send_signal(signal.SIGHUP)

        def attach_writer():
            nonlocal writer
            try:
                writer = os.open(app.config, os.O_WRONLY | os.O_NONBLOCK)
                return True  # A nonblocking writer opens only when a real reader exists.
            except OSError as error:
                if error.errno != errno.ENXIO:
                    raise
                return False

        eventually(attach_writer, "Management worker never opened the injected config FIFO", health=app.health)
        # Keeping this writer open, without data, prevents getline from seeing bytes or EOF.
        yield
    finally:
        restored = app.config.with_suffix(".restored")
        restored.write_bytes(contents)
        restored.replace(app.config)
        if writer is not None:
            try:
                assert len(contents) <= 4096, "Test configuration must fit one atomic FIFO write"
                assert os.write(writer, contents) == len(contents)
            except BrokenPipeError:
                pass  # The process may have died during an earlier assertion.
            finally:
                os.close(writer)


def consume_repeated_hup(app):
    # Standard signals may coalesce in the kernel. Wait for each HUP to be consumed
    # before sending another so the test also exercises application reload coalescing.
    for _ in range(4):
        app.process.send_signal(signal.SIGHUP)

        def hup_consumed():
            status = (Path("/proc") / str(app.process.pid) / "status").read_text()
            pending = [int(line.split()[1], 16) for line in status.splitlines()
                       if line.startswith(("SigPnd:", "ShdPnd:"))]
            return not any(mask & (1 << (signal.SIGHUP - 1)) for mask in pending)

        eventually(hup_consumed, "Management Reactor stopped consuming SIGHUP", health=app.health)


def check_management_reactor(binary):
    for stop_signal in (signal.SIGTERM, signal.SIGINT):
        with running(binary) as app:
            check_management_thread_ownership(app)
            completed_reloads = 0
            if stop_signal == signal.SIGTERM:
                with blocked_config_reload(app):
                    consume_repeated_hup(app)
                    app.node.send(0x02, (2026).to_bytes(2, "big"))
                    eventually(lambda: any(row["value"] == 2026 for row in app.rows("illuminance")),
                               "Serial/HTTP stopped while resource worker was blocked", health=app.health)
                # This response traverses the resource-worker queue after all repeated HUPs;
                # extra reloads would therefore appear in the final, flushed log below.
                app.peer.publish("query_light", [""])
                app.wait_message("gateway/resp/0 ok,42")
                completed_reloads = 1

            with blocked_config_reload(app):
                consume_repeated_hup(app)
                app.process.send_signal(stop_signal)
                eventually(lambda: "main loop exited, shutting down" in
                           (app.root / "gateway.log").read_text(),
                           f"{stop_signal.name} did not stop serial Reactor while resource worker was blocked",
                           health=app.health)
                assert app.process.poll() is None, "Shutdown finished before the blocked worker was released"
            app.process.wait(timeout=6)
            assert app.process.returncode == 0, f"Unclean shutdown after {stop_signal.name}"
            log = (app.root / "gateway.log").read_text()
            assert log.count("SIGHUP received, reloading config...") == completed_reloads + 1, \
                "SIGHUP submissions were not coalesced while a reload was in flight"
    print("PASS independent management epoll/thread and signal ownership, reload coalescing, blocked-worker TERM/INT")


def check_round_trip_and_http(binary):
    with running(binary) as app:
        start = int(time.time())
        app.node.send(0x01, b"\x00\xeb\x02\x2b")  # 23.5 C, 55.5 %.
        app.node.send(0x02, b"\x01\x41")            # 321 lux.
        app.node.send(0x04, b"\x03")                # Both sensors healthy.
        expected = {"temperature": 23.5, "humidity": 55.5, "illuminance": 321,
                    "status_dht11": 1, "status_bh1750": 1}
        for device, value in expected.items():
            app.wait_message(f"gateway/up/{device} {value:g}")
            rows = eventually(lambda: app.rows(device), f"HTTP omitted {device}", health=app.health)
            assert len(rows) == 1 and rows[0]["device_id"] == device
            assert rows[0]["value"] == value and start <= rows[0]["ts"] <= int(time.time())
        stored = database_rows(app.root / "first.db")
        assert len(stored) == len(expected)
        assert {device: value for device, value, _ in stored} == expected

        for name, value, expected_result in (
            ("query_light", "", "gateway/resp/0 ok,42"),
            ("query_th", "", "gateway/resp/1 ok,23,55"),
            ("set_period", 7, "gateway/ack/2 ok"),
        ):
            app.peer.publish(name, [value])
            app.wait_message(expected_result)
        app.peer.publish("set_period", ["invalid"])
        eventually(lambda: any(line.startswith("gateway/ack/rejected ") for line in app.messages()),
                   "Invalid command received no rejection", health=app.health)
        assert len(app.node.snapshot()) == 3, "Rejected command reached serial node"

        connection = http.client.HTTPConnection("127.0.0.1", app.http_port, timeout=4)
        try:
            original_socket = None
            for path, status, content_type in (
                ("/", 200, "text/html"),
                ("/uplot.js", 200, "javascript"),
                ("/uplot.css", 200, "text/css"),
                ("/api/data?dev=temperature&n=1", 200, "application/json"),
                ("/api/data", 400, "application/json"),
                ("/missing", 404, "application/json"),
            ):
                connection.request("GET", path)
                if original_socket is None:
                    original_socket = connection.sock
                assert connection.sock is original_socket, "HTTP keepalive opened another socket"
                response = connection.getresponse()
                body = response.read()
                assert response.status == status, (path, response.status, body)
                assert content_type in response.getheader("Content-Type", "")
                assert int(response.getheader("Content-Length")) == len(body)
            connection.request("GET", "/", headers={"Connection": "close"})
            response = connection.getresponse()
            response.read()
            assert response.will_close, "Connection: close was ignored"
        finally:
            connection.close()
        assert app.request("/", "POST")[0] == 405
        assert app.rows("unknown") == []
        with socket.create_connection(("127.0.0.1", app.http_port), timeout=4) as client:
            client.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\ninvalid-header\r\n\r\n")
            response = http.client.HTTPResponse(client)
            response.begin()
            assert response.status == 400, "Malformed HTTP request was not rejected"
            response.read()
    print("PASS telemetry MQTT/SQLite/HTTP, command results, HTTP assets/errors/keepalive")


def check_independent_progress(binary):
    with running(binary) as app, contextlib.ExitStack() as clients:
        # Incomplete headers remain pending while complete requests and node traffic progress.
        for _ in range(8):
            client = clients.enter_context(socket.create_connection(("127.0.0.1", app.http_port), timeout=4))
            client.sendall(b"GET /api/data?dev=temperature HTTP/1.1\r\nHost: local")
        app.peer.pause_output()
        app.peer.publish("set_period", [11])
        app.node.send(0x02, b"\x01\xf4")
        app.wait_message("gateway/up/illuminance 500")
        eventually(lambda: app.rows("illuminance"), "HTTP stalled behind paused serial output", health=app.health)
        assert not app.node.snapshot(), "Paused PTY unexpectedly delivered a command"
        app.peer.resume_output()
        app.wait_message("gateway/ack/0 ok")

        def query_many():
            for _ in range(12):
                assert app.request("/api/data?dev=illuminance&n=5")[0] == 200

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as requests:
            futures = [requests.submit(query_many) for _ in range(4)]
            for value in range(501, 521):
                app.node.send(0x02, value.to_bytes(2, "big"))
            app.peer.publish("set_period", range(12, 20))
            for sequence in range(1, 9):
                app.wait_message(f"gateway/ack/{sequence} ok")
            for future in futures:
                future.result(timeout=8)
        eventually(lambda: len(app.rows("illuminance")) == 21,
                   "Concurrent HTTP/serial activity lost telemetry", health=app.health)
        commands = app.node.snapshot()
        assert [int.from_bytes(payload[10:], "big") for payload in commands] == list(range(11, 20))
        assert len(database_rows(app.root / "first.db")) == 21
        app.shutdown()  # Incomplete HTTP connections must not prevent thread shutdown.
    print("PASS slow HTTP and paused serial isolation, concurrent requests/commands, active-client shutdown")


def check_reload_boundaries(binary):
    with running(binary) as app:
        app.node.send(0x02, b"\x03\x09")  # 777 in the old database.
        eventually(lambda: bool(app.rows("illuminance")), "Old database did not receive telemetry", health=app.health)
        old_rows = database_rows(app.root / "first.db")
        connection = http.client.HTTPConnection("127.0.0.1", app.http_port, timeout=4)
        try:
            connection.request("GET", "/api/data?dev=illuminance")
            response = connection.getresponse()
            assert json.loads(response.read())[0]["value"] == 777
            original_socket = connection.sock
            new_db = app.root / "second.db"
            app.rewrite_config(db_path=new_db, report_n=1)

            def switched():
                connection.request("GET", "/api/data?dev=illuminance")
                assert connection.sock is original_socket, "Reload closed a live HTTP connection"
                response = connection.getresponse()
                assert response.status == 200
                return json.loads(response.read()) == []

            eventually(switched, "Existing HTTP connection did not switch database", health=app.health)
            app.node.send(0x02, b"\x03\x78")  # 888 in the new database only.
            app.wait_message("gateway/up/illuminance 888")
            eventually(lambda: bool(app.rows("illuminance")), "New database did not receive telemetry", health=app.health)
            assert database_rows(app.root / "first.db") == old_rows
            assert [(row[0], row[1]) for row in database_rows(new_db)] == [("illuminance", 888)]

            # A failing candidate database must leave the last working database usable.
            app.rewrite_config(db_path=app.root / "missing" / "failed.db")
            eventually(lambda: "ERROR" in (app.root / "gateway.log").read_text(),
                       "Failed database reload was not reported", health=app.health)
            app.node.send(0x02, b"\x03\x79")
            eventually(lambda: len(app.rows("illuminance")) == 2,
                       "Failed reload lost the working database", health=app.health)
            app.peer.publish("query_light", [""])
            app.wait_message("gateway/resp/0 ok,42")
            assert app.request("/")[0] == 200
        finally:
            connection.close()
    print("PASS atomic database reload, existing HTTP connection, failed reload fallback and later commands")


def check_resource_reload_under_activity(binary):
    with running(binary) as app:
        app.peer.publish("set_period", [21])
        app.wait_message("gateway/ack/0 ok")
        app.node.hold_results.set()
        app.peer.publish("set_period", [22])
        eventually(lambda: len(app.node.snapshot()) == 2,
                   "Command to interrupt did not reach node", health=app.health)
        subscriptions = (app.root / "broker.log").read_text().count("gateway/cmd/#")
        stopping = threading.Event()
        shutting_down = threading.Event()
        active = threading.Event()

        def http_traffic():
            while not stopping.is_set():
                try:
                    status = app.request("/api/data?dev=illuminance")[0]
                    if shutting_down.is_set() and status == 503:
                        return  # 入口已停止，但 HTTP 循环尚在关闭连接的短窗口。
                    assert status == 200, f"Unexpected HTTP status during active reload: {status}"
                    active.set()
                except (OSError, http.client.HTTPException):
                    if not shutting_down.is_set():
                        raise
                    return
                stopping.wait(0.01)

        def serial_traffic():
            value = 1200
            while not stopping.is_set():
                app.node.send(0x02, value.to_bytes(2, "big"))
                value = 1200 + (value - 1199) % 100
                stopping.wait(0.02)

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as producers:
            futures = [producers.submit(http_traffic), producers.submit(serial_traffic)]
            try:
                assert active.wait(4), "Concurrent HTTP traffic did not start"
                # Same PTY, different baud: exercise actual serial reopen without a timing guess.
                app.rewrite_config(mqtt_keepalive=30, serial_baud=57600)
                eventually(lambda: termios.tcgetattr(app.peer.slave)[4] == termios.B57600,
                           "Serial configuration was not applied", health=app.health)
                eventually(lambda: (app.root / "broker.log").read_text().count("gateway/cmd/#") > subscriptions,
                           "Rebuilt MQTT client did not resubscribe", health=app.health)
                app.wait_message("gateway/ack/1 session_aborted")
                app.node.hold_results.clear()  # Late old-session results must remain harmless.
                app.node.send(0x02, (1777).to_bytes(2, "big"))
                app.wait_message("gateway/up/illuminance 1777")
                eventually(lambda: any(row["value"] == 1777 for row in app.rows("illuminance")),
                           "Post-reload telemetry did not reach HTTP", health=app.health)
                previous = len(app.node.snapshot())
                app.peer.publish("query_light", [""])
                commands = eventually(lambda: app.node.snapshot()[previous:],
                                      "Post-reload command did not reach node", health=app.health)
                app.wait_message(f"gateway/resp/{commands[0][8]} ok,42")
                shutting_down.set()
                app.shutdown()  # Producers are still active as all gateway threads stop.
            finally:
                stopping.set()
                for future in futures:
                    future.result(timeout=5)
    print("PASS active command abort, MQTT/serial reload during HTTP+telemetry activity, concurrent shutdown")


def check_http_descriptor_exhaustion(binary):
    with running(binary, fd_limit=64) as app, contextlib.ExitStack() as clients:
        existing = http.client.HTTPConnection("127.0.0.1", app.http_port, timeout=4)
        clients.callback(existing.close)
        existing.request("GET", "/api/data?dev=illuminance")
        response = existing.getresponse()
        assert response.status == 200
        response.read()
        connection_socket = existing.sock
        for _ in range(80):
            client = socket.socket()
            clients.callback(client.close)
            client.settimeout(0.1)
            try:
                client.connect(("127.0.0.1", app.http_port))
                client.sendall(b"GET / HTTP/1.1\r\nHost: local")
            except TimeoutError:
                break  # The listen backlog may fill after accept reaches EMFILE.
        eventually(lambda: "Too many open files" in (app.root / "gateway.log").read_text(),
                   "HTTP accept did not hit the injected descriptor limit", health=app.health)

        def cpu_seconds():
            # /proc stat comm may contain spaces; fields after ')' begin at field 3.
            fields = Path(f"/proc/{app.process.pid}/stat").read_text().rsplit(")", 1)[1].split()
            return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")

        before = cpu_seconds()
        time.sleep(0.7)
        assert cpu_seconds() - before < 0.35, "EMFILE accept retry consumed CPU in a busy loop"
        existing.request("GET", "/api/data?dev=illuminance")
        assert existing.sock is connection_socket
        response = existing.getresponse()
        assert response.status == 200
        response.read()
        app.peer.publish("query_light", [""])
        app.wait_message("gateway/resp/0 ok,42")
        app.shutdown()
    print("PASS injected HTTP EMFILE: retry backoff, existing connection/command progress, shutdown")


def main():
    for executable in ("mosquitto", "mosquitto_pub", "mosquitto_sub"):
        assert shutil.which(executable), f"Missing required executable: {executable}"
    binary = Path(sys.argv[1]).resolve()
    assert binary.is_file(), f"Gateway binary does not exist: {binary}"
    for check in (check_round_trip_and_http, check_independent_progress, check_reload_boundaries,
                  check_resource_reload_under_activity, check_http_descriptor_exhaustion,
                  check_management_reactor):
        check(binary)


if __name__ == "__main__":
    main()
