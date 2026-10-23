#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Socket-level tests for the epoll HTTP server; uses no HTTP client library."""

import json
import os
import socket
import subprocess
import sys
import tempfile
import time


ROOT = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(ROOT, "epoll_echo")
TEST_RESPONSE_SIZE = 2 * 1024 * 1024
failures = []


class HttpClient:
    def __init__(self, host, port, receive_buffer=None):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(5)
        if receive_buffer is not None:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF,
                                 receive_buffer)
        self.buffer = b""

    def send(self, data):
        self.sock.sendall(data)

    def response(self):
        while b"\r\n\r\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError("connection closed before response headers")
            self.buffer += chunk

        header_end = self.buffer.index(b"\r\n\r\n")
        raw_headers = self.buffer[:header_end].decode("iso-8859-1")
        self.buffer = self.buffer[header_end + 4:]
        lines = raw_headers.split("\r\n")
        version, status, _ = lines[0].split(" ", 2)
        headers = {}
        for line in lines[1:]:
            name, value = line.split(":", 1)
            headers[name.lower()] = value.strip()

        if version != "HTTP/1.1" or "content-length" not in headers:
            raise AssertionError("invalid response status or missing Content-Length")
        content_length = int(headers["content-length"])
        while len(self.buffer) < content_length:
            chunk = self.sock.recv(min(65536, content_length - len(self.buffer)))
            if not chunk:
                raise AssertionError("connection closed before complete response body")
            self.buffer += chunk
        body = self.buffer[:content_length]
        self.buffer = self.buffer[content_length:]
        return int(status), headers, body

    def assert_closed(self):
        if self.buffer:
            raise AssertionError("unexpected bytes after response: %r" % self.buffer[:32])
        self.sock.settimeout(3)
        if self.sock.recv(1) != b"":
            raise AssertionError("server did not close the connection")

    def close(self):
        self.sock.close()


def request(path="/", headers=b"Host: test\r\n", method="GET"):
    return (method.encode("ascii") + b" " + path.encode("ascii") +
            b" HTTP/1.1\r\n" + headers + b"\r\n")


def response_word(response):
    return json.loads(response[2].decode("utf-8"))["word"]


def h1(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/"))
        status, headers, body = client.response()
        assert status == 200, status
        assert int(headers["content-length"]) == len(body)
        assert json.loads(body.decode("utf-8"))["path"] == "/"
    finally:
        client.close()


def h2(host, port):
    client = HttpClient(host, port)
    try:
        for word in ("one", "two", "three"):
            client.send(request("/?word=" + word))
            status, _, response_body = client.response()
            assert status == 200, status
            assert json.loads(response_body.decode("utf-8"))["word"] == word
    finally:
        client.close()


def h3(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/?word=first") + request("/?word=second"))
        first = client.response()
        second = client.response()
        assert first[0] == second[0] == 200
        assert response_word(first) == "first"
        assert response_word(second) == "second"
    finally:
        client.close()


def h4(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/", b"Host: test\r\nConnection: close\r\n"))
        status, headers, _ = client.response()
        assert status == 200, status
        assert headers.get("connection") == "close", headers
        client.assert_closed()
    finally:
        client.close()


def h5(host, port):
    client = HttpClient(host, port)
    try:
        client.send(b"GARBAGE\r\n\r\n")
        status, headers, _ = client.response()
        assert status == 400, status
        assert headers.get("connection") == "close", headers
        client.assert_closed()
    finally:
        client.close()


def h6(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/", method="POST"))
        status, _, _ = client.response()
        assert status == 405, status
    finally:
        client.close()


def h7(host, port):
    client = HttpClient(host, port)
    try:
        oversized = (b"GET / HTTP/1.1\r\nHost: test\r\nX-Fill: " +
                     b"x" * 10240 + b"\r\n\r\n")
        client.send(oversized)
        status, headers, _ = client.response()
        assert status in (400, 431), status
        assert headers.get("connection") == "close", headers
        client.assert_closed()
    finally:
        client.close()


def h8(host, port):
    client = HttpClient(host, port)
    try:
        client.send(b"GET /?word=%E4%B8%AD%E6%96%87 HTTP/1.1\r\n"
                    b"Host: test\r\n\r\n")
        status, _, body = client.response()
        assert status == 200, status
        assert json.loads(body.decode("utf-8"))["word"] == "中文"
    finally:
        client.close()


def h9(host, port):
    client = HttpClient(host, port)
    try:
        parts = (b"GET /?word=", b"slow", b" HTTP/1.1\r\nHost: test\r\n\r\n")
        for part in parts:
            client.send(part)
            time.sleep(0.1)
        status, _, body = client.response()
        assert status == 200, status
        assert json.loads(body.decode("utf-8"))["word"] == "slow"
    finally:
        client.close()


def h10(host, port):
    # 32 KiB equals the loopback MSS; a smaller SO_RCVBUF drops the advertised
    # window below one segment and makes this case take minutes in zero-window
    # probing. The 2 MiB body is still far larger than the buffer, so the
    # server must handle partial writes and EPOLLOUT.
    client = HttpClient(host, port, receive_buffer=32768)
    try:
        client.send(request("/__test/large",
                            b"Host: test\r\nConnection: close\r\n"))
        time.sleep(0.2)
        status, headers, body = client.response()
        assert status == 200, status
        assert int(headers["content-length"]) == TEST_RESPONSE_SIZE
        assert body == b"x" * TEST_RESPONSE_SIZE
        client.assert_closed()
    finally:
        client.close()


def h_idle_timeout(host, port):
    client = socket.create_connection((host, port), timeout=5)
    client.settimeout(8)
    started = time.monotonic()
    try:
        data = client.recv(1)
        elapsed = time.monotonic() - started
        assert data == b"", "expected EOF, got %r" % data
        assert 4.5 <= elapsed <= 7.0, elapsed
    finally:
        client.close()


def p1(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/s?word=boost"))
        status, headers, body = client.response()
        assert status == 200, status
        assert headers.get("content-type") == "application/json; charset=utf-8", headers
        assert isinstance(json.loads(body.decode("utf-8")), list)
    finally:
        client.close()


def p2(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/s"))
        status, headers, body = client.response()
        assert status == 400, status
        assert headers.get("content-type") == "text/plain", headers
        assert body == b"missing word parameter", body
        client.assert_closed()
    finally:
        client.close()


def p3(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/s?word="))
        status, headers, body = client.response()
        assert status == 200, status
        assert headers.get("content-type") == "application/json; charset=utf-8", headers
        assert body == b"[]", body
    finally:
        client.close()


def p4(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/s?word=%E4%B8%AD%E6%96%87"))
        status, headers, body = client.response()
        assert status == 200, status
        assert headers.get("content-type") == "application/json; charset=utf-8", headers
        assert isinstance(json.loads(body.decode("utf-8")), list)
    finally:
        client.close()


def p5(host, port):
    client = HttpClient(host, port)
    try:
        client.send(request("/s?word=98765432109876543210"))
        status, headers, body = client.response()
        assert status == 200, status
        assert headers.get("content-type") == "application/json; charset=utf-8", headers
        assert body == b"[]", body
    finally:
        client.close()


def choose_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]
    finally:
        sock.close()


def start_server(binary, port):
    log = tempfile.TemporaryFile()
    process = subprocess.Popen([binary, str(port)],
                               cwd=ROOT,
                               stdout=subprocess.DEVNULL,
                               stderr=log)
    deadline = time.time() + 60
    while time.time() < deadline:
        if process.poll() is not None:
            log.seek(0)
            output = log.read().decode("utf-8", "replace")
            log.close()
            raise RuntimeError("server exited during startup:\n" + output)
        try:
            probe = socket.create_connection(("127.0.0.1", port), timeout=0.2)
            probe.close()
            return process, log
        except OSError:
            time.sleep(0.05)
    process.terminate()
    process.wait(timeout=2)
    log.seek(0)
    output = log.read().decode("utf-8", "replace")
    log.close()
    raise RuntimeError("server did not start:\n" + output)


def stop_server(process, log):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    log.close()


def run_tests(binary, tests):
    port = choose_port()
    process, log = start_server(binary, port)
    try:
        thread_count = len(os.listdir("/proc/%d/task" % process.pid))
        assert thread_count == 7, "expected 7 server threads, got %d" % thread_count
        print("PASS  main + 2 sub-reactors + 4 business workers (%d threads)" %
              thread_count)
        for name, test in tests:
            try:
                test("127.0.0.1", port)
                print("PASS  " + name)
            except Exception as error:
                failures.append(name)
                print("FAIL  %s | %r" % (name, error))
    finally:
        stop_server(process, log)


def main():
    subprocess.run(["make", "epoll_echo"], cwd=ROOT, check=True)
    print("=== HTTP/1.1 socket tests ===")
    run_tests(SERVER, [
        ("H1 response framing and Content-Length", h1),
        ("H2 keep-alive sequential requests", h2),
        ("H3 pipelined requests in one send", h3),
        ("H4 Connection: close", h4),
        ("H5 malformed request line returns 400", h5),
        ("H6 unsupported method returns 405", h6),
        ("H7 oversized request headers", h7),
        ("H8 percent-decoded UTF-8 query", h8),
        ("H9 incrementally received request", h9),
        ("P1 /s returns search JSON", p1),
        ("P2 missing word returns exact 400 body and closes", p2),
        ("P3 empty word returns []", p3),
        ("P4 percent-decoded UTF-8 search", p4),
        ("P5 no-match search returns []", p5),
        ("idle keep-alive connection closes after timeout", h_idle_timeout),
    ])

    with tempfile.TemporaryDirectory(prefix="epoll-http-test-") as temp_dir:
        test_binary = os.path.join(temp_dir, "epoll_echo_large_test")
        subprocess.run([
            "g++", "-std=c++11", "-O2", "-Wall",
            "-DCOMMAND_SERVER_TEST_LARGE_RESPONSE",
            *[os.path.join(ROOT, source) for source in
              ("epoll_echo.cc", "CommandServer.cc", "Reactor.cc",
               "BusinessPool.cc")],
            "-o", test_binary, "-Icppjieba/include", "-ljsoncpp", "-pthread",
        ], cwd=ROOT, check=True)
        run_tests(test_binary, [
            ("H10 complete 2 MiB response after socket backpressure", h10),
        ])

    print("===")
    if failures:
        print("Result: FAIL — " + ", ".join(failures))
        return 1
    print("Result: PASS — H1-H10, P1-P5, and idle timeout")
    return 0


if __name__ == "__main__":
    sys.exit(main())
