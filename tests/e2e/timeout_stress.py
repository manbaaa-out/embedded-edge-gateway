#!/usr/bin/env python3
"""并发连接超时压力客户端。

目标服务必须在 TCP 8888 端口提供原样 echo，并在五秒空闲后主动关闭连接。脚本让
多条空闲连接在同一扫描窗口到期，同时维持活跃连接并延迟建立新连接，用于观察批量
清理是否误伤活跃 fd、监听 fd 或触发延迟销毁问题。本文件不负责启动目标服务。
"""

import socket
import threading
import time

HOST = "127.0.0.1"       # 目标 echo 服务地址。
PORT = 8888              # 目标 echo 服务监听端口。
SERVER_TIMEOUT = 5       # 目标服务配置的空闲超时秒数，必须与服务端一致。
IDLE_COUNT = 8           # 同时到期的静默连接数，用于触发单轮批量删除。
WAIT_AFTER_TIMEOUT = 4   # 超时阈值后的观察窗口，容纳定时扫描粒度。

results = {}             # 客户端名称到可读结果的跨线程汇总表。
lock = threading.Lock()  # 保护 results；同时使记录和打印保持同一临界区顺序。


def record(name, msg):
    """原子记录一个客户端结果；name 是唯一客户端名，msg 是最终诊断文本。"""
    with lock:
        results[name] = msg
    print(f"  [{name}] {msg}", flush=True)


def idle_client(idx):
    """运行第 idx 条静默连接，并验证服务在预期超时后主动关闭它。"""
    name = f"idle-{idx}"  # idx 让并发连接在汇总表中保持可区分。
    try:
        s = socket.create_connection((HOST, PORT), timeout=10)  # s 代表本条空闲 TCP 连接。
    except Exception as e:
        record(name, f"连接失败: {e}")
        return

    s.settimeout(SERVER_TIMEOUT + WAIT_AFTER_TIMEOUT + 5)
    t0 = time.time()  # 从连接建立后开始测量服务端空闲关闭耗时。
    try:
        # 客户端不发送任何字节；recv 返回空 bytes 表示对端完成有序关闭。
        data = s.recv(1024)  # 非空数据在该静默场景中属于异常服务行为。
        elapsed = time.time() - t0  # 用于区分空闲超时和过早断开。
        if data == b"":
            record(name, f"被踢出(连接关闭),耗时 {elapsed:.1f}s "
                         f"{'✓ 符合预期' if elapsed >= SERVER_TIMEOUT - 1 else '✗ 太快了,可能不是超时踢的'}")
        else:
            record(name, f"收到意外数据 {data!r} —— 不该有数据")
    except socket.timeout:
        record(name, "✗ 一直没被踢(recv 超时)—— 超时逻辑可能没生效")
    except Exception as e:
        record(name, f"异常: {e}")
    finally:
        s.close()


def active_client(idx):
    """运行第 idx 条活跃连接，以两秒间隔验证 echo 和空闲计时刷新。"""
    name = f"active-{idx}"
    try:
        s = socket.create_connection((HOST, PORT), timeout=10)  # s 在整个观察窗口复用。
    except Exception as e:
        record(name, f"连接失败: {e}")
        return

    s.settimeout(5)
    ok_rounds = 0  # 已完成且字节完全匹配的 echo 轮数。
    try:
        # 覆盖空闲连接应被批量清理的整个窗口，确认活跃 fd 不在删除集合中。
        deadline = time.time() + SERVER_TIMEOUT + WAIT_AFTER_TIMEOUT
        n = 0  # 写入负载的单调编号，避免不同轮次内容相同。
        while time.time() < deadline:
            payload = f"ping-{idx}-{n}".encode()  # 同时编码客户端和轮次身份。
            s.sendall(payload)
            echo = s.recv(1024)  # 服务应原样返回本轮负载。
            if echo == payload:
                ok_rounds += 1
            else:
                record(name, f"✗ echo 不匹配: 发 {payload!r} 收 {echo!r}")
                return
            n += 1
            time.sleep(2)  # 间隔严格小于 SERVER_TIMEOUT，用于刷新 last_active。
        record(name, f"✓ 全程存活,{ok_rounds} 轮 echo 正常,未被误杀")
    except socket.timeout:
        record(name, "✗ 活跃连接却被踢/无响应(recv 超时)—— 误杀活跃连接")
    except Exception as e:
        record(name, f"异常: {e}")
    finally:
        s.close()


def late_client():
    """在批量清理后新建连接，验证监听 fd 仍可接受并处理请求。"""
    name = "late-accept"
    # 延迟越过空闲阈值，但无需等待完整观察窗口。
    time.sleep(SERVER_TIMEOUT + 2)
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)  # 清理完成后建立的新连接。
    except Exception as e:
        record(name, f"✗ 踢人后无法建立新连接 —— listen_fd 可能被误关: {e}")
        return
    s.settimeout(5)
    try:
        s.sendall(b"hello-after-purge")
        echo = s.recv(1024)  # 同时验证 accept 后的新连接收发路径。
        if echo == b"hello-after-purge":
            record(name, "✓ 踢人后仍能 accept 新连接且 echo 正常,listen_fd 健在")
        else:
            record(name, f"✗ echo 异常: {echo!r}")
    except Exception as e:
        record(name, f"✗ 新连接收发异常: {e}")
    finally:
        s.close()


def main():
    """创建三类客户端线程，等待全部结束并按三项契约汇总结果。"""
    print(f"目标 {HOST}:{PORT}  server TIMEOUT={SERVER_TIMEOUT}s")
    print(f"启动 {IDLE_COUNT} 个空闲连接 + 2 个活跃连接 + 1 个延迟连接\n")

    threads = []  # 所有工作线程的所有权集合，用于统一启动和 join。

    # 连续启动静默连接，使它们的 last_active 接近并在同轮扫描进入删除集合。
    for i in range(IDLE_COUNT):
        t = threading.Thread(target=idle_client, args=(i,))  # i 传入客户端显示名称。
        threads.append(t)

    for i in range(2):
        t = threading.Thread(target=active_client, args=(i,))
        threads.append(t)

    threads.append(threading.Thread(target=late_client))

    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # 分别计算满足三类契约的客户端数量；诊断文字也承担机器判定标志。
    print("\n===== 结果汇总 =====")
    idle_kicked = sum(1 for k, v in results.items()  # 在预期路径观察到对端关闭的空闲连接数。
                      if k.startswith("idle-") and "被踢出" in v)
    active_alive = sum(1 for k, v in results.items()  # 完整存活观察窗口的活跃连接数。
                       if k.startswith("active-") and v.startswith("✓"))
    late = results.get("late-accept", "(未执行)")  # 清理后监听器检查的文本结果。

    print(f"空闲连接被踢: {idle_kicked}/{IDLE_COUNT}")
    print(f"活跃连接存活: {active_alive}/2")
    print(f"延迟连接(listen 健在): {late}")

    print("\n判定:")
    ok = (idle_kicked == IDLE_COUNT and active_alive == 2  # 三项必须全部满足才算整体通过。
          and late.startswith("✓"))
    if ok:
        print("  ✓✓ 全部通过:批量踢出正常、活跃不误杀、listen 健在、server 未崩")
    else:
        print("  ✗ 有未通过项,检查上面明细。若 server 此刻已崩溃,"
              "多半是延迟销毁在'一轮删多个'时失效。")


if __name__ == "__main__":
    main()
