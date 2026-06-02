#!/usr/bin/env python3
"""
M9 超时踢出压测脚本

验证三件事:
1. 批量空闲连接被同一轮定时器扫描一起踢出(压 UAF / 延迟销毁)
2. 活跃连接(持续发数据)不被误杀
3. listen_fd 没被误关 —— 踢人期间仍能接受新连接

用法:
    1. 先启动 server:  ./timer_server
    2. 再跑本脚本:      python3 timeout_stress.py
       (默认连本机 8888,TIMEOUT=5。如改了 server 参数,改下面常量)
"""

import socket
import threading
import time

HOST = "127.0.0.1"
PORT = 8888
SERVER_TIMEOUT = 5          # 必须和 server 里的 TIMEOUT 一致
IDLE_COUNT = 8              # 同时挂着不发数据的空闲连接数(压批量踢出)
WAIT_AFTER_TIMEOUT = 4      # 超时后再多等几秒,观察是否真被踢

results = {}                # name -> 描述结果
lock = threading.Lock()


def record(name, msg):
    with lock:
        results[name] = msg
    print(f"  [{name}] {msg}", flush=True)


def idle_client(idx):
    """连上后完全不发数据,应在 ~SERVER_TIMEOUT 秒后被 server 踢掉。"""
    name = f"idle-{idx}"
    try:
        s = socket.create_connection((HOST, PORT), timeout=10)
    except Exception as e:
        record(name, f"连接失败: {e}")
        return

    s.settimeout(SERVER_TIMEOUT + WAIT_AFTER_TIMEOUT + 5)
    t0 = time.time()
    try:
        # 一个字节都不发,只读。被 server 踢掉时,recv 返回 b''(对端关闭)。
        data = s.recv(1024)
        elapsed = time.time() - t0
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
    """每 2 秒发一次数据(< TIMEOUT),应当全程存活、echo 正常,不被踢。"""
    name = f"active-{idx}"
    try:
        s = socket.create_connection((HOST, PORT), timeout=10)
    except Exception as e:
        record(name, f"连接失败: {e}")
        return

    s.settimeout(5)
    ok_rounds = 0
    try:
        # 在"空闲连接应被踢"的时间窗口内,持续保持活跃
        deadline = time.time() + SERVER_TIMEOUT + WAIT_AFTER_TIMEOUT
        n = 0
        while time.time() < deadline:
            payload = f"ping-{idx}-{n}".encode()
            s.sendall(payload)
            echo = s.recv(1024)
            if echo == payload:
                ok_rounds += 1
            else:
                record(name, f"✗ echo 不匹配: 发 {payload!r} 收 {echo!r}")
                return
            n += 1
            time.sleep(2)   # 间隔 < TIMEOUT,保持活跃
        record(name, f"✓ 全程存活,{ok_rounds} 轮 echo 正常,未被误杀")
    except socket.timeout:
        record(name, "✗ 活跃连接却被踢/无响应(recv 超时)—— 误杀活跃连接")
    except Exception as e:
        record(name, f"异常: {e}")
    finally:
        s.close()


def late_client():
    """在批量踢人发生之后才连进来,验证 listen_fd 没被误关(还能 accept)。"""
    name = "late-accept"
    # 等到空闲连接都被踢之后再连
    time.sleep(SERVER_TIMEOUT + 2)
    try:
        s = socket.create_connection((HOST, PORT), timeout=5)
    except Exception as e:
        record(name, f"✗ 踢人后无法建立新连接 —— listen_fd 可能被误关: {e}")
        return
    s.settimeout(5)
    try:
        s.sendall(b"hello-after-purge")
        echo = s.recv(1024)
        if echo == b"hello-after-purge":
            record(name, "✓ 踢人后仍能 accept 新连接且 echo 正常,listen_fd 健在")
        else:
            record(name, f"✗ echo 异常: {echo!r}")
    except Exception as e:
        record(name, f"✗ 新连接收发异常: {e}")
    finally:
        s.close()


def main():
    print(f"目标 {HOST}:{PORT}  server TIMEOUT={SERVER_TIMEOUT}s")
    print(f"启动 {IDLE_COUNT} 个空闲连接 + 2 个活跃连接 + 1 个延迟连接\n")

    threads = []

    # 关键:8 个空闲连接几乎同时建立 —— 它们的 last_active 落在同一秒附近,
    # 大概率在同一轮定时器扫描里一起进入 timeout_fds,一次 for 循环连删多个,
    # 这正是压 UAF / 延迟销毁的场景。
    for i in range(IDLE_COUNT):
        t = threading.Thread(target=idle_client, args=(i,))
        threads.append(t)

    for i in range(2):
        t = threading.Thread(target=active_client, args=(i,))
        threads.append(t)

    threads.append(threading.Thread(target=late_client))

    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # 汇总
    print("\n===== 结果汇总 =====")
    idle_kicked = sum(1 for k, v in results.items()
                      if k.startswith("idle-") and "被踢出" in v)
    active_alive = sum(1 for k, v in results.items()
                       if k.startswith("active-") and v.startswith("✓"))
    late = results.get("late-accept", "(未执行)")

    print(f"空闲连接被踢: {idle_kicked}/{IDLE_COUNT}")
    print(f"活跃连接存活: {active_alive}/2")
    print(f"延迟连接(listen 健在): {late}")

    print("\n判定:")
    ok = (idle_kicked == IDLE_COUNT and active_alive == 2
          and late.startswith("✓"))
    if ok:
        print("  ✓✓ 全部通过:批量踢出正常、活跃不误杀、listen 健在、server 未崩")
    else:
        print("  ✗ 有未通过项,检查上面明细。若 server 此刻已崩溃,"
              "多半是延迟销毁在'一轮删多个'时失效。")


if __name__ == "__main__":
    main()