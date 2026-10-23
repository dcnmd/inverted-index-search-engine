#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASELINE_PORT=8081
EPOLL_PORT=8082
TMP_DIR="$(mktemp -d)"
BASELINE_PID=""
EPOLL_PID=""

stop_server() {
    local pid="$1"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid"
        wait "$pid" 2>/dev/null || true
    fi
}

cleanup() {
    stop_server "$EPOLL_PID"
    stop_server "$BASELINE_PID"
    rm -f -- "$TMP_DIR/baseline.log" "$TMP_DIR/epoll.log" "$TMP_DIR/baseline.json"
    rmdir -- "$TMP_DIR"
}
trap cleanup EXIT

python3 - "$BASELINE_PORT" "$EPOLL_PORT" <<'PY'
import socket
import sys

for port in map(int, sys.argv[1:]):
    with socket.socket() as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("0.0.0.0", port))
        except OSError as error:
            raise SystemExit("port %d is unavailable: %s" % (port, error))
PY

make -C "$ROOT" http_server epoll_echo

wait_ready() {
    local pid="$1"
    local port="$2"
    local logfile="$3"
    if ! python3 - "$pid" "$port" <<'PY'
import socket
import sys
import time

pid, port = int(sys.argv[1]), int(sys.argv[2])
deadline = time.monotonic() + 60
while time.monotonic() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            raise SystemExit(0)
    except OSError:
        try:
            import os
            os.kill(pid, 0)
        except OSError:
            raise SystemExit(1)
        time.sleep(0.1)
raise SystemExit(1)
PY
    then
        cat "$logfile" >&2
        return 1
    fi
}

"$ROOT/http_server" > /dev/null 2>"$TMP_DIR/baseline.log" &
BASELINE_PID="$!"
wait_ready "$BASELINE_PID" "$BASELINE_PORT" "$TMP_DIR/baseline.log"

python3 - "$BASELINE_PORT" "$TMP_DIR/baseline.json" <<'PY'
import base64
import http.client
import json
import sys

baseline_port, output_path = int(sys.argv[1]), sys.argv[2]
cases = [
    "/s?word=boost",
    "/s?word=asio",
    "/s?word=filesystem",
    "/s?word=cpp",
    "/s?word=%E4%B8%AD%E6%96%87",
    "/s?word=boost%20filesystem",
    "/s?word=",
    "/s",
    "/s?word=98765432109876543210",
]


def fetch(port, path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    try:
        connection.request("GET", path, headers={"Host": "localhost"})
        response = connection.getresponse()
        body = response.read()
        headers = sorted(
            (name.lower(), value.strip())
            for name, value in response.getheaders()
            if name.lower() not in ("date", "server")
        )
        return response.status, headers, body
    finally:
        connection.close()


snapshots = {}
for path in cases:
    status, headers, body = fetch(baseline_port, path)
    snapshots[path] = (status, headers, base64.b64encode(body).decode("ascii"))
with open(output_path, "w", encoding="ascii") as output:
    json.dump(snapshots, output)
PY

stop_server "$BASELINE_PID"
BASELINE_PID=""

"$ROOT/epoll_echo" "$EPOLL_PORT" > /dev/null 2>"$TMP_DIR/epoll.log" &
EPOLL_PID="$!"
wait_ready "$EPOLL_PID" "$EPOLL_PORT" "$TMP_DIR/epoll.log"

python3 - "$EPOLL_PORT" "$TMP_DIR/baseline.json" <<'PY'
import base64
import http.client
import json
import sys

epoll_port, input_path = int(sys.argv[1]), sys.argv[2]
with open(input_path, "r", encoding="ascii") as source:
    snapshots = json.load(source)

for path, expected in snapshots.items():
    baseline = (expected[0], [tuple(header) for header in expected[1]],
                base64.b64decode(expected[2]))
    connection = http.client.HTTPConnection("127.0.0.1", epoll_port, timeout=30)
    try:
        connection.request("GET", path, headers={"Host": "localhost"})
        response = connection.getresponse()
        body = response.read()
        headers = sorted(
            (name.lower(), value.strip())
            for name, value in response.getheaders()
            if name.lower() not in ("date", "server")
        )
        epoll = response.status, headers, body
    finally:
        connection.close()
    if baseline != epoll:
        print("FAIL %s" % path, file=sys.stderr)
        if baseline[0] != epoll[0]:
            print("  status: baseline=%r epoll=%r" %
                  (baseline[0], epoll[0]), file=sys.stderr)
        if baseline[1] != epoll[1]:
            print("  headers: baseline=%r epoll=%r" %
                  (baseline[1], epoll[1]), file=sys.stderr)
        if baseline[2] != epoll[2]:
            print("  body lengths: baseline=%d epoll=%d" %
                  (len(baseline[2]), len(epoll[2])), file=sys.stderr)
            limit = min(len(baseline[2]), len(epoll[2]))
            mismatch = next((i for i in range(limit)
                             if baseline[2][i] != epoll[2][i]), limit)
            print("  first body mismatch offset: %d" % mismatch,
                  file=sys.stderr)
        raise SystemExit(1)
    print("PASS %s (%d bytes)" % (path, len(epoll[2])))

print("PASS all nine baseline comparisons")
PY
