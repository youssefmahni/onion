#!/usr/bin/env python3
"""Connection-ramp wiring test (decision 0011): with Connection: close per
response every request needs a new connection, and the token bucket holds
the open rate at the configured cap.

NIGHTLY ONLY: the assertions measure wall-clock rates and durations, which
depend on the runner's speed, not on product behaviour (AUDIT.md).  In the
normal CI matrix this test skips (exit 77)."""
import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path


def completion_ok(output, count):
    return output.count(" kind=0 http_status=200") == count


def summary(output, key):
    line = [line for line in output.splitlines() if line.startswith("http_version=")][0]
    return int(line.split(key + "=")[1].split(" ")[0])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if os.environ.get("ONION_NIGHTLY") != "1":
        print("skipped: timing-sensitive wiring test; runs in opt-nightly.yml")
        sys.exit(77)

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(256)
    port = listener.getsockname()[1]
    accepts = []
    lock = threading.Lock()
    stop = threading.Event()

    def handle(connection):
        try:
            while True:
                chunk = connection.recv(4096)
                if not chunk or b"\r\n\r\n" in chunk:
                    break
            time.sleep(0.4)
            body = json.dumps({"id": "t", "object": "chat.completion",
                               "choices": [{"index": 0, "message": {"role": "assistant", "content": "ok"},
                                            "finish_reason": "stop"}],
                               "usage": {"prompt_cache_hit_tokens": 0,
                                         "prompt_cache_miss_tokens": 4,
                                         "completion_tokens": 2}}).encode()
            response = (b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        b"Connection: close\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
            connection.sendall(response)
        except OSError:
            pass
        finally:
            connection.close()

    def accept_loop():
        while not stop.is_set():
            try:
                listener.settimeout(0.1)
                connection, _ = listener.accept()
                with lock:
                    accepts.append(time.monotonic())
                threading.Thread(target=handle, args=(connection,), daemon=True).start()
            except socket.timeout:
                pass
            except OSError:
                break

    thread = threading.Thread(target=accept_loop, daemon=True)
    thread.start()
    try:
        completed = subprocess.run(
            [str(args.binary), "llm", "probe", "--invalid-key",
             "--url", f"http://127.0.0.1:{port}",
             "--new-connections-per-s", "10", "--requests", "120", "--timeout", "10"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        assert completed.returncode == 0, completed.stderr
        assert completion_ok(completed.stdout, 120), completed.stdout
        with lock:
            times = list(accepts)
        assert len(times) == 120, f"expected 120 accepts, got {len(times)}"
        # Any sliding one-second window may hold at most the cap plus one
        # window of burst (decision 0011: 10 + 10 here).
        maximum = 0
        for index, moment in enumerate(times):
            count = sum(1 for t in times if moment <= t < moment + 1.0)
            maximum = max(maximum, count)
        assert maximum <= 20, f"new-connection rate exceeded cap + burst: {maximum}"
        # Unthrottled the run takes ~6 s (120/8 x 0.4 s); at the 10/s cap it
        # takes ~11-12 s.  The bound sits between the two.
        elapsed = max(times) - min(times)
        assert elapsed >= 10.5, f"ramp did not throttle the run: {elapsed:.1f}s"
    finally:
        stop.set()
        listener.close()
    print("ramp-ok")


if __name__ == "__main__":
    main()
