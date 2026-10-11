#!/usr/bin/env python3
"""Client-hygiene regression tests for decision 0011:
one DNS lookup per host per TTL, a capped new-connection rate, and
connection reuse."""
import argparse
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py"
SCENARIO = ROOT / "tools" / "fake_deepseek" / "scenarios" / "healthy.json"


def run_probe(binary, url, *extra):
    return subprocess.run(
        [str(binary), "llm", "probe", "--invalid-key", "--url", url, *extra],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)


def completion_ok(output, count):
    return output.count(" kind=0 http_status=200") == count


def summary(output, key):
    line = [line for line in output.splitlines() if line.startswith("http_version=")][0]
    return int(line.split(key + "=")[1].split(" ")[0])


def dns_one_lookup_per_host(binary):
    """The shared DNS cache plus the test resolver memo: one lookup for 100
    requests to the same host."""
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "requests.jsonl"
        server = subprocess.Popen(
            [sys.executable, str(SERVER), "--scenario", str(SCENARIO), "--record", str(record), "--port", "0"],
            stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().strip())
            completed = run_probe(binary, f"http://localhost:{port}",
                                  "--test-resolver", "--requests", "100")
            assert completed.returncode == 0, completed.stderr
            assert completion_ok(completed.stdout, 100), completed.stdout
            assert summary(completed.stdout, "dns_lookups") == 1, completed.stdout
        finally:
            server.terminate()
            server.wait(timeout=10)


def resolver_failures_are_transport(binary):
    """A failing resolution is a Transport outcome and does not hit the wire."""
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "requests.jsonl"
        server = subprocess.Popen(
            [sys.executable, str(SERVER), "--scenario", str(SCENARIO), "--record", str(record), "--port", "0"],
            stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().strip())
            completed = run_probe(binary, f"http://localhost:{port}",
                                  "--test-resolver", "--resolver-fail-first", "3", "--requests", "50")
            assert completed.returncode == 0, completed.stderr
            assert completed.stdout.count(" kind=1 ") == 3, completed.stdout
            assert completion_ok(completed.stdout, 47), completed.stdout
            # Three failed lookups plus the one successful resolution.
            assert summary(completed.stdout, "dns_lookups") == 4, completed.stdout
        finally:
            server.terminate()
            server.wait(timeout=10)


def connection_ramp_cap(binary):
    """With Connection: close per response every request needs a new
    connection; the token bucket holds the open rate at the configured cap."""
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
        completed = run_probe(binary, f"http://127.0.0.1:{port}",
                              "--new-connections-per-s", "10", "--requests", "120", "--timeout", "10")
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
        # Without the ramp the client would finish 120 requests in about
        # 120/8 x 0.4 s = 6 s; at the 10/s cap it takes at least 11 s.
        elapsed = max(times) - min(times)
        assert elapsed >= 11.0, f"ramp did not throttle the run: {elapsed:.1f}s"
    finally:
        stop.set()
        listener.close()


def connection_reuse(binary):
    """Keep-alive responses reuse connections: 100 requests open a handful."""
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "requests.jsonl"
        server = subprocess.Popen(
            [sys.executable, str(SERVER), "--scenario", str(SCENARIO), "--record", str(record), "--port", "0"],
            stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().strip())
            completed = run_probe(binary, f"http://127.0.0.1:{port}", "--requests", "100")
            assert completed.returncode == 0, completed.stderr
            assert completion_ok(completed.stdout, 100), completed.stdout
            opened = summary(completed.stdout, "connections_opened")
            assert opened <= 10, f"expected reuse, opened {opened} connections"
        finally:
            server.terminate()
            server.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    dns_one_lookup_per_host(args.binary)
    resolver_failures_are_transport(args.binary)
    connection_ramp_cap(args.binary)
    connection_reuse(args.binary)
    print("hygiene-ok")


if __name__ == "__main__":
    main()
