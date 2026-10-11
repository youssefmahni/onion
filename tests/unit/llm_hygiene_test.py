#!/usr/bin/env python3
"""Client-hygiene regression tests for decision 0011:
one DNS lookup per host per TTL, resolver-failure classification, and
connection reuse.  (The new-connection-rate wiring test lives in
llm_ramp_test.py, nightly only.)"""
import argparse
import json
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from fake_server_harness import fake_server


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
    with fake_server("healthy.json") as (url, _):
        port = int(url.rsplit(":", 1)[1])
        completed = run_probe(binary, f"http://localhost:{port}",
                              "--test-resolver", "--requests", "100")
        assert completed.returncode == 0, completed.stderr
        assert completion_ok(completed.stdout, 100), completed.stdout
        assert summary(completed.stdout, "dns_lookups") == 1, completed.stdout


def resolver_failures_are_transport(binary):
    """A failing resolution is a Transport outcome and does not hit the wire."""
    with fake_server("healthy.json") as (url, _):
        port = int(url.rsplit(":", 1)[1])
        completed = run_probe(binary, f"http://localhost:{port}",
                              "--test-resolver", "--resolver-fail-first", "3", "--requests", "50")
        assert completed.returncode == 0, completed.stderr
        assert completed.stdout.count(" kind=1 ") == 3, completed.stdout
        assert completion_ok(completed.stdout, 47), completed.stdout
        # Three failed lookups plus the one successful resolution.
        assert summary(completed.stdout, "dns_lookups") == 4, completed.stdout


def connection_reuse(binary):
    """Keep-alive responses reuse connections: 100 requests open a handful."""
    with fake_server("healthy.json") as (url, _):
        port = int(url.rsplit(":", 1)[1])
        completed = run_probe(binary, f"http://127.0.0.1:{port}", "--requests", "100")
        assert completed.returncode == 0, completed.stderr
        assert completion_ok(completed.stdout, 100), completed.stdout
        opened = summary(completed.stdout, "connections_opened")
        assert opened <= 10, f"expected reuse, opened {opened} connections"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    dns_one_lookup_per_host(args.binary)
    resolver_failures_are_transport(args.binary)
    connection_reuse(args.binary)
    print("hygiene-ok")


if __name__ == "__main__":
    main()
