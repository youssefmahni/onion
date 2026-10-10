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
    parser.add_argument("--funnel", action="store_true",
                        help="deterministic funnel replies (offline campaigns)")
    parser.add_argument("--funnel-max-depth", default=5, type=int)
    parser.add_argument("--funnel-score", default=0.9, type=float)
    arguments = parser.parse_args()
    scenario = json.loads(arguments.scenario.read_text(encoding="utf-8"))
    arguments.record.parent.mkdir(parents=True, exist_ok=True)
    fake = FakeDeepSeek(scenario, arguments.record)
    handler = (handler_for_funnel(fake, arguments.funnel_max_depth,
                                   arguments.funnel_score)
               if arguments.funnel else handler_for(fake))
    server = ConcurrentFakeServer((arguments.host, arguments.port), handler)
    print(server.server_address[1], flush=True)
    server.serve_forever()




# ---------------------------------------------------------------------------
# Deterministic funnel mode: replies keyed on the node depth parsed from the
# "Node: L<d> ..." line of every gate prompt, so whole offline campaigns run
# reproducibly. Titles come from a rotating palette, so no ancestor or
# sibling repeats while the tree grows; at funnel_max_depth the pre-check
# says no and the branch ends.
FUNNEL_PALETTE = [
    "Order Capture", "Shipment Status", "Payment Method", "Inventory Count",
    "Customer Record", "Audit Trail", "Approval Flow", "Report Export",
    "Compliance Check", "Refund Process", "Vendor Registry",
    "Warehouse Layout", "Invoice Matching", "Return Authorization",
    "Quality Inspection", "Forecast Model", "Pricing Strategy",
    "Contract Review", "Credit Assessment", "Lead Scoring",
    "Support Escalation",
]


def funnel_children(depth, max_depth):
    # Children of any node at this depth share one palette band, so titles
    # never repeat a sibling, an ancestor or the node itself while the tree
    # grows; the last band is atomic.
    base = 3 * (depth + 1)
    return [
        {"title": FUNNEL_PALETTE[(base + i) % len(FUNNEL_PALETTE)],
         "atomic": depth >= max_depth - 1}
        for i in range(3)
    ]


def funnel_depth(prompt):
    marker = "Node: L"
    start = prompt.find(marker)
    if start < 0:
        return None
    digits = ""
    for character in prompt[start + len(marker):]:
        if character.isdigit():
            digits += character
        else:
            break
    return int(digits) if digits else None


def funnel_reply(prompt, max_depth, score=0.9):
    depth = funnel_depth(prompt)
    if depth is None:
        depth = 0
    if "Can this node be split" in prompt:
        return json.dumps({"splits": depth < max_depth}, separators=(",", ":"))
    if "Plan the knowledge coverage" in prompt:
        return json.dumps({"items": ["Coverage A", "Coverage B", "Coverage C"]},
                          separators=(",", ":"))
    if "Aspect:" in prompt:
        return json.dumps(
            {"items": [child["title"] for child in
                       funnel_children(depth, max_depth)]},
            separators=(",", ":"))
    if "From these candidates keep only" in prompt:
        return json.dumps(
            {"items": [child["title"] for child in
                       funnel_children(depth, max_depth)]},
            separators=(",", ":"))
    if "Critique this candidate set" in prompt:
        return json.dumps({"score": score, "remove": [], "add": [], "issues": ""},
                          separators=(",", ":"))
    if "select the final children set" in prompt:
        return json.dumps({"items": funnel_children(depth, max_depth)},
                          separators=(",", ":"))
    return json.dumps({"items": ["One", "Two", "Three"]}, separators=(",", ":"))


def handler_for_funnel(fake, max_depth, score=0.9):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            return

        def send_json(self, code, body):
            encoded = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

        def do_GET(self):
            if self.path != "/models":
                self.send_error(404)
                return
            self.send_json(200, {"object": "list",
                                 "data": [{"id": "deepseek-flash"}]})

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
            started = time.monotonic()
            messages = request.get("messages", [])
            prompt = "\n".join(str(message.get("content", ""))
                               for message in messages
                               if message.get("role") == "user")
            content = funnel_reply(prompt, max_depth, score)
            usage = {"prompt_cache_hit_tokens": 0, "prompt_cache_miss_tokens": 12,
                     "completion_tokens": 8}
            self.send_json(200, {"id": "fake-chatcmpl", "object": "chat.completion",
                                 "choices": [{"index": 0,
                                              "message": {"role": "assistant",
                                                          "content": content},
                                              "finish_reason": "stop"}],
                                 "usage": usage})
            headers = {key: value for key, value in self.headers.items()
                       if key.lower() != "authorization"}
            fake.write_record({"method": "POST", "path": self.path,
                               "headers": headers, "body": request,
                               "elapsed_ms": round((time.monotonic() - started) * 1000, 3)})
    return Handler


if __name__ == "__main__":
    main()
