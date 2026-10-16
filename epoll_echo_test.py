#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""epoll_echo_test.py —— 阶段 1 验收脚本（在服务器上跑）

用法:
    python3 epoll_echo_test.py [port]        # 默认 8082

约定:
    * 前 6 个用例是阶段 1 的必过项，任一 FAIL 则退出码为 1。
    * T7 是"阶段 1 已知缺陷（部分写）"的复现记录，允许失败，不计入退出码；
      阶段 2 修完写缓冲后，这一条应该变成 PASS。
"""

import socket
import subprocess
import sys
import time

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8082

failures = []


def report(name, ok, detail=""):
    print(("PASS  " if ok else "FAIL  ") + name + (("  | " + detail) if detail else ""))
    if not ok:
        failures.append(name)
    return ok


def connect(timeout=5.0):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(timeout)
    return s


def recv_exact(s, n, timeout=5.0):
    s.settimeout(timeout)
    buf = b""
    while len(buf) < n:
        chunk = s.recv(min(65536, n - len(buf)))
        if not chunk:
            break
        buf += chunk
    return buf


def recv_all(s, timeout=1.0):
    s.settimeout(timeout)
    buf = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
    return buf


def server_fd_count():
    """返回 echo 进程打开的 fd 数量；找不到进程返回 None。"""
    try:
        pid = subprocess.check_output(["pgrep", "-x", "epoll_echo"],
                                      stderr=subprocess.DEVNULL).decode().split()[0]
    except Exception:
        return None
    try:
        return len(subprocess.check_output(["ls", "/proc/%s/fd" % pid],
                                           stderr=subprocess.DEVNULL).decode().split())
    except Exception:
        return None


def t1_half_close_echo():
    """printf 'hello' | nc 式：发数据 + shutdown(SHUT_WR)，仍要拿到回显"""
    s = connect()
    s.sendall(b"hello half-close\n")
    s.shutdown(socket.SHUT_WR)
    got = recv_all(s)
    s.close()
    report("T1 半关闭回显 (echo hello | nc)", got == b"hello half-close\n", repr(got))


def t2_two_connections():
    """两个连接交错收发，各自只收到自己的数据"""
    a, b = connect(), connect()
    pa1, pb1, pa2, pb2 = b"A" * 16, b"B" * 16, b"a2" * 8, b"b2" * 8
    a.sendall(pa1); b.sendall(pb1)
    a.sendall(pa2); b.sendall(pb2)
    ga = recv_exact(a, len(pa1) + len(pa2))
    gb = recv_exact(b, len(pb1) + len(pb2))
    a.close(); b.close()
    ok = ga == pa1 + pa2 and gb == pb1 + pb2
    report("T2 双连接互不干扰", ok, "A=%r B=%r" % (ga[:12], gb[:12]))


def t3_pipelined():
    """一条连接上连发 200 个小包（ET 读循环必须一次读到 EAGAIN）"""
    s = connect()
    msgs = [b"%04d-%s" % (i, b"z" * 4) for i in range(200)]
    for m in msgs:
        s.sendall(m)
    expect = b"".join(msgs)
    got = recv_exact(s, len(expect))
    s.close()
    report("T3 连发 200 包粘包回显", got == expect, "expect=%d recv=%d" % (len(expect), len(got)))


def t4_many_connections():
    """64 个并发连接，每个只收到自己的 payload"""
    n = 64
    conns = [connect(10.0) for _ in range(n)]
    payloads = [("conn-%03d|" % i).encode() * 4 for i in range(n)]
    for c, p in zip(conns, payloads):
        c.sendall(p)
    ok = True
    bad = ""
    for i, (c, p) in enumerate(zip(conns, payloads)):
        got = recv_exact(c, len(p), timeout=10.0)
        if got != p:
            ok = False
            bad = "conn %d got %r" % (i, got[:32])
            break
    for c in conns:
        c.close()
    report("T4 64 并发连接隔离", ok, bad)


def t5_round_trips():
    """同一条连接上来回 2000 次请求-响应，验证连接可长期复用、不卡死"""
    s = connect(10.0)
    ok = True
    detail = ""
    for i in range(2000):
        msg = b"ping-%d" % i
        s.sendall(msg)
        got = recv_exact(s, len(msg), timeout=5.0)
        if got != msg:
            ok = False
            detail = "第 %d 次失配: %r" % (i, got)
            break
    s.close()
    report("T5 单连接 2000 次往返", ok, detail)


def t6_fd_leak():
    """连接全关掉后，进程 fd 数应该回落（无泄漏）"""
    before = server_fd_count()
    if before is None:
        print("SKIP  T6 fd 泄漏检查（没找到 epoll_echo 进程，用 --pid 或忽略）")
        return
    conns = [connect(10.0) for _ in range(50)]
    peak = server_fd_count()
    for c in conns:
        c.close()
    after = peak
    ok = False
    for _ in range(20):                 # 最多等 2 秒让 close 传播完
        time.sleep(0.1)
        after = server_fd_count()
        if after is not None and after - before <= 3:
            ok = True
            break
    report("T6 连接关闭后 fd 回落", ok,
           "before=%s peak=%s after=%s" % (before, peak, after))


def t7_partial_write_known_bug():
    """【阶段 1 已知缺陷】非阻塞 send 的部分写，现在没有写缓冲 / EPOLLOUT。

    复现要点（踩过的坑，别退回 "1 MiB 单发" 那种写法）：
      * 直接发 1 MiB 在 loopback 上**不会**触发 —— 内核 autotune 会把 sndbuf
        涨到 1 MiB 以上，一次 send 就全进了内核缓冲区（本脚本实测过：PASS）。
      * 可靠的复现是"客户端发完一大块后**不读**"，把接收窗口堵死：
        服务端 recv 循环把 8 MiB 全收进用户态，但 send 只能写满 sndbuf，
        剩下的留在内存里；因为没有注册 EPOLLOUT，对端之后把数据读走时
        内核不会再通知服务端，这段数据就永远发不出去。
      * 阶段 2 补上写缓冲 + EPOLLOUT 后，这条应该变成 PASS。
    """
    n = 8 << 20
    payload = b"x" * n
    s = connect(30.0)
    s.sendall(payload)               # 服务端一边收一边尝试回显
    time.sleep(1.5)                  # 客户端故意不读：服务端 sndbuf 被塞满 -> 部分写
    got = recv_all(s, timeout=3.0)   # 开始读，能读到的都读掉
    got += recv_all(s, timeout=3.0)  # 再等一轮：若已注册 EPOLLOUT，这里会收到续发的数据
    s.close()
    if got == payload:
        status = "PASS(已修复)"
    else:
        status = "EXPECTED-FAIL(阶段2修)"
    print("      T7 8MiB 阻塞式回显（部分写复现）: %s | sent=%d recv=%d (丢 %d 字节)"
          % (status, n, len(got), n - len(got)))


def main():
    print("=== epoll_echo 阶段1 验收 @ %s:%d ===" % (HOST, PORT))
    for fn in (t1_half_close_echo, t2_two_connections, t3_pipelined,
               t4_many_connections, t5_round_trips, t6_fd_leak):
        try:
            fn()
        except Exception as e:  # noqa: BLE001
            report(fn.__name__, False, "异常: %r" % (e,))
    try:
        t7_partial_write_known_bug()
    except Exception as e:  # noqa: BLE001
        print("      T7 复现时异常（也算记录到即可）: %r" % (e,))

    print("===")
    if failures:
        print("结果: FAIL —— 未通过: %s" % ", ".join(failures))
        return 1
    print("结果: PASS —— 阶段 1 的 6 个必过用例全通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
