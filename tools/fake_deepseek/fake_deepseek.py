#!/usr/bin/env python3
"""Scriptable, standard-library-only fake of the DeepSeek chat API."""
import argparse
import json
import random
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class FakeDeepSeek:
    def __init__(self, scenario, record_path):
        self.scenario = scenario
        self.record_path = record_path
        self.lock = threading.Lock()
        self.in_flight = 0

    def write_record(self, item):
        with self.lock:
            with self.record_path.open("a", encoding="utf-8", newline="\n") as output:
                output.write(json.dumps(item, ensure_ascii=False, separators=(",", ":")) + "\n")

    def delay(self):
        latency = self.scenario.get("latency", {})
        fixed = latency.get("fixed_ms")
        if fixed is not None:
            milliseconds = fixed
        else:
            milliseconds = random.gauss(latency.get("mean_ms", 0), latency.get("stddev_ms", 0))
        threshold = self.scenario.get("slow_when_in_flight_above")
        if threshold is not None and self.in_flight > threshold:
            milliseconds += self.scenario.get("slow_extra_ms", 0)
        if milliseconds > 0:
            time.sleep(milliseconds / 1000.0)

    def route(self, prompt):
        for item in self.scenario.get("routes", []):
            if item.get("contains", "") in prompt:
                return item
        return self.scenario.get("default", {})


def handler_for(fake):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            return

        def send_json(self, code, body, extra=None):
            encoded = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            for key, value in (extra or {}).items():
                self.send_header(key, str(value))
            self.end_headers()
            self.wfile.write(encoded)

        def do_GET(self):
            if self.path != "/models":
                self.send_error(404)
                return
            self.send_json(200, {"object": "list", "data": fake.scenario.get("models", [{"id": "deepseek-flash"}])})

        def do_POST(self):
            if self.path != "/chat/completions":
                self.send_error(404)
                return
            size = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(size)
            try:
                request = json.loads(raw)
            except json.JSONDecodeError:
                self.send_json(400, {"error": {"message": "invalid JSON"}})
                return
            messages = request.get("messages", [])
            prompt = "\n".join(str(message.get("content", "")) for message in messages if message.get("role") == "user")
            started = time.monotonic()
            with fake.lock:
                fake.in_flight += 1
            try:
                fake.delay()
                if random.random() < fake.scenario.get("reset_connection_rate", 0.0):
                    # A FIN can leave a client with a partial HTTP response on
                    # some platforms.  Linger-zero produces the requested TCP
                    # reset deterministically instead.
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                               struct.pack("ii", 1, 0))
                    self.connection.close()
                    return
                route = fake.route(prompt)
                errors = fake.scenario.get("errors", {})
                status = errors.get("status", 0)
                if not status:
                    status = 429 if random.random() < errors.get("rate_429", 0.0) else 503 if random.random() < errors.get("rate_503", 0.0) else 200
                if status != 200:
                    self.send_json(status, {"error": {"message": "scripted error"}}, {"Retry-After": errors.get("retry_after_s", 1)})
                    return
                if random.random() < fake.scenario.get("malformed_body_rate", 0.0):
                    self.send_response(200); self.end_headers(); self.wfile.write(b"{")
                    return
                content = route.get("content", fake.scenario.get("content", '{"items":["one","two","three"]}'))
                truncated = random.random() < fake.scenario.get("truncation_rate", 0.0)
                if truncated:
                    content = content[:max(1, len(content) // 2)]
                usage = route.get("usage", fake.scenario.get("usage", {"prompt_cache_hit_tokens": 0, "prompt_cache_miss_tokens": 12, "completion_tokens": 8}))
                self.send_json(200, {"id": "fake-chatcmpl", "object": "chat.completion", "choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "length" if truncated else "stop"}], "usage": usage})
            finally:
                with fake.lock:
                    fake.in_flight -= 1
                headers = {key: value for key, value in self.headers.items() if key.lower() != "authorization"}
                fake.write_record({"method": "POST", "path": self.path, "headers": headers, "body": request, "elapsed_ms": round((time.monotonic() - started) * 1000, 3)})
    return Handler


class ConcurrentFakeServer(ThreadingHTTPServer):
    """A deliberately deep, daemon-threaded accept queue for load tests."""
    daemon_threads = True
    request_queue_size = 2048
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--record", required=True, type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=18080, type=int)
    arguments = parser.parse_args()
    scenario = json.loads(arguments.scenario.read_text(encoding="utf-8"))
    arguments.record.parent.mkdir(parents=True, exist_ok=True)
    server = ConcurrentFakeServer((arguments.host, arguments.port), handler_for(FakeDeepSeek(scenario, arguments.record)))
    print(server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
