#!/usr/bin/env python3
"""M8: a running node exposes every dashboard observable on /api/state."""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request


def wait_for_state(port_file, process):
    deadline = time.time() + 20
    while time.time() < deadline:
        if process.poll() is not None:
            raise AssertionError("node exited before publishing its ports")
        if os.path.exists(port_file):
            with open(port_file, encoding="utf-8") as source:
                ports = json.load(source)
            if ports.get("api", 0) > 0:
                with urllib.request.urlopen(
                        "http://127.0.0.1:%d/api/state" % ports["api"],
                        timeout=5) as response:
                    return json.load(response)
        time.sleep(0.1)
    raise AssertionError("node did not publish an API state within 20 seconds")


def main():
    binary = sys.argv[1]
    root = tempfile.mkdtemp(prefix="onion_m8_observability_")
    data = os.path.join(root, "data")
    port_file = os.path.join(root, "ports.json")
    process = subprocess.Popen(
        [binary, "node", "run", "--data-dir", data,
         "--base-url", "http://127.0.0.1:9", "--host", "127.0.0.1",
         "--api-port", "0", "--sync-port", "0", "--swim-port", "0",
         "--port-file", port_file],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        state = wait_for_state(port_file, process)
        required = {
            "lanes": list,
            "claims": list,
            "steals_active": int,
            "review_backlog": int,
            "spend_rate_usd_per_hour": str,
            "log_bytes_by_device": dict,
            "data_dir_bytes": int,
            "quality_guard": dict,
        }
        for key, expected_type in required.items():
            assert key in state, "missing M8 state field %s" % key
            assert isinstance(state[key], expected_type), \
                "%s is not a %s" % (key, expected_type.__name__)
        assert state["lanes"], "single-node state must expose its own lane"
        assert {"device_id", "open_tickets"} <= set(state["lanes"][0])
        assert {"samples", "rolling_score", "failure_rate", "flagged"} <= \
            set(state["quality_guard"])
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
        if process.stderr:
            process.stderr.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
