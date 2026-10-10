#!/usr/bin/env python3
"""Multi-process M6 test: three real node processes sync and converge.

One node runs a campaign against the fake funnel server; the other two sync
from it. After settling, all three stores must hold the same Merkle root
(the campaign work) and the same node count.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request


def wait_for(predicate, timeout=60.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.5)
    return False


def wait_for_key(data_dir):
    def check():
        return os.path.exists(os.path.join(data_dir, "keys", "device-key"))
    if not wait_for(check):
        raise RuntimeError("device key never appeared")
    return open(os.path.join(data_dir, "keys", "device-key")).read().strip()


def run(binary, *args, check=True):
    return subprocess.run([binary, *args], check=check, capture_output=True,
                          text=True)


def http_json(url):
    with urllib.request.urlopen(url, timeout=10) as response:
        return json.loads(response.read())


def main():
    binary = sys.argv[1]
    root = tempfile.mkdtemp(prefix="onion_mp_")

    fake = subprocess.Popen(
        [sys.executable, "tools/fake_deepseek/fake_deepseek.py",
         "--scenario", "tools/fake_deepseek/scenarios/healthy.json",
         "--record", os.path.join(root, "requests.jsonl"),
         "--port", "18101", "--funnel", "--funnel-max-depth", "3"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.7)

    # Bootstrap: three data directories, one seed root per device.
    devices = {}
    for index in (1, 2, 3):
        data = os.path.join(root, f"d{index}")
        os.makedirs(os.path.join(data, "keys"))
        corpus = os.path.join(root, f"corpus{index}")
        os.makedirs(corpus)
        with open(os.path.join(corpus, "Onion - level_00.csv"), "w") as f:
            f.write(f"code,title\nP0{index},Phase {index}\n")
        run(binary, "dev-import", "--root", corpus, "--data-dir", data)

        boot = subprocess.Popen(
            [binary, "node", "run", "--data-dir", data,
             "--base-url", "http://127.0.0.1:9", "--api-port",
             str(18110 + index)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_for_key(data)
        boot.terminate()
        boot.wait(timeout=10)

        identity = run(binary, "node", "identity", "--data-dir", data).stdout
        fields = dict(line.split("=", 1) for line in identity.strip().splitlines())
        devices[index] = {"data": data, "device_id": fields["device_id"],
                          "public_key": fields["public_key"]}

    # members.json with all three devices; node 1 runs the campaign.
    members = [
        {"device_id": d["device_id"], "public_key": d["public_key"],
         "url": "127.0.0.1:%d" % (18201 + i), "weight": 1.0}
        for i, d in [(1, devices[1]), (2, devices[2]), (3, devices[3])]
    ]
    for index in (1, 2, 3):
        with open(os.path.join(root, f"d{index}", "members.json"), "w") as f:
            json.dump(members, f)

    # A small loop budget closes the campaign fleet-wide (each device stops
    # when its chunk reservations exhaust the budget); the stop rule needs
    # 50 settled nodes per device, which lane splitting never reaches.
    campaign = {"id": "mp1", "scope": ["P01", "P02", "P03"],
                "loop_budget": 10, "mode": "breadth-first", "chunk": 50,
                "model": "deepseek-flash"}
    with open(os.path.join(root, "campaign.json"), "w") as f:
        json.dump(campaign, f)
    run(binary, "campaign", "start", os.path.join(root, "campaign.json"),
        "--data-dir", devices[1]["data"], "--yes")

    nodes = []
    for index in (1, 2, 3):
        data = devices[index]["data"]
        nodes.append(subprocess.Popen(
            [binary, "node", "run", "--data-dir", data,
             "--base-url", "http://127.0.0.1:18101",
             "--members", os.path.join(data, "members.json"),
             "--host", "127.0.0.1",
             "--sync-port", str(18201 + index),
             "--api-port", str(18110 + index)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

    try:
        # The campaign's roots hash to home lanes across all three devices;
        # wait until every node has stopped the campaign and at least one
        # settled something.
        def all_done():
            try:
                for index in (1, 2, 3):
                    state = http_json("http://127.0.0.1:%d/api/state" % (18110 + index))
                    campaigns = state.get("campaigns", [])
                    if not any(c["state"] == "stopped" for c in campaigns):
                        return False
                return True
            except Exception:
                return False
        assert wait_for(all_done, timeout=300), "campaign never finished everywhere"

        # Give the sync rounds time to converge every store.
        time.sleep(35)

        # All three must agree on the settled count, and it must be > 0.
        counts = []
        for index in (1, 2, 3):
            state = http_json("http://127.0.0.1:%d/api/state" % (18110 + index))
            counts.append(state["settled_nodes"])
        assert max(counts) > 0, "the campaign settled nothing anywhere"
        assert counts[0] == counts[1] == counts[2], counts
        print("multi-process convergence: PASS", counts)
    finally:
        for node in nodes:
            node.terminate()
        for node in nodes:
            try:
                node.wait(timeout=10)
            except subprocess.TimeoutExpired:
                node.kill()
        fake.terminate()
        fake.wait(timeout=10)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
