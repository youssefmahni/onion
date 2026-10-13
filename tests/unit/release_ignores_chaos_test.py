#!/usr/bin/env python3
"""Release builds must ignore the chaos knobs entirely.

The chaos hooks (ONION_KILL_AT_COMMIT, ONION_CLOCK_JUMP_MS,
ONION_SIM_DISK_FULL, ONION_PARTITION_FROM) are compiled in only when
CMake's ONION_CHAOS_HOOKS is ON (test presets). This test is registered
only when the option is OFF, so it runs against release binaries: two
nodes run a campaign with all four knobs set aggressively and must behave
as if none of them existed — no kill at commit, appends succeed, the
clock jump changes nothing observable, and the partition never happens,
so the two nodes converge.

Exit 77 = skip (used by the scenario suite's symmetric probe; this test
itself fails hard instead of skipping — being registered implies hooks
are absent and the checks must hold).
"""
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FAKE_SERVER = str(ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py")
HEALTHY_SCENARIO = str(ROOT / "tools" / "fake_deepseek"
                       / "scenarios" / "healthy.json")

HARD_DEADLINE_S = float(os.environ.get("ONION_MP_DEADLINE_S", "180"))


def http_json(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read())


def run(binary, *args, check=True):
    return subprocess.run([binary, *args], check=check, capture_output=True,
                          text=True)


def state(port):
    return http_json("http://127.0.0.1:%d/api/state" % port)


def wait_port_file(path, timeout=30):
    end = time.time() + timeout
    while not os.path.exists(path) and time.time() < end:
        time.sleep(0.2)
    assert os.path.exists(path), "port file %s never appeared" % path
    with open(path) as f:
        return json.load(f)


def main():
    binary = sys.argv[1]
    probe = run(binary, "node", "chaos-hooks").stdout.strip()
    assert probe == "0", "this test runs only against hookless builds"
    root = tempfile.mkdtemp(prefix="onion_nohook_")

    fake = subprocess.Popen(
        [sys.executable, FAKE_SERVER, "--scenario", HEALTHY_SCENARIO,
         "--record", os.path.join(root, "requests.jsonl"),
         "--port", "0", "--funnel", "--funnel-max-depth", "6"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    fake_port = int(fake.stdout.readline().strip())
    assert fake_port > 0

    devices = {}
    nodes = []
    deadline = time.time() + HARD_DEADLINE_S
    try:
        for index in (1, 2):
            data = os.path.join(root, "d%d" % index)
            os.makedirs(os.path.join(data, "keys"))
            corpus = os.path.join(root, "corpus%d" % index)
            os.makedirs(corpus)
            with open(os.path.join(corpus, "Onion - level_04.csv"),
                      "w") as f:
                f.write("code,path,title\nP%02d,P%02d,Phase %d\n"
                        % (index, index, index))
            run(binary, "node", "key", "set", "device-key",
                "%064x" % (index * 0x22222222), "--data-dir", data)
            run(binary, "genesis-import", "--root", corpus,
                "--data-dir", data)
            identity = run(binary, "node", "identity",
                           "--data-dir", data).stdout
            fields = dict(line.split("=", 1)
                          for line in identity.strip().splitlines())
            devices[index] = {"data": data,
                              "device_id": fields["device_id"],
                              "public_key": fields["public_key"]}

        members_path = os.path.join(root, "members.json")
        with open(members_path, "w") as f:
            json.dump([{"device_id": devices[i]["device_id"],
                        "public_key": devices[i]["public_key"],
                        "weight": 1.0} for i in (1, 2)], f)
        campaign_file = os.path.join(root, "campaign.json")
        with open(campaign_file, "w") as f:
            json.dump({"id": "nohook", "scope": ["P01", "P02"],
                       "loop_budget": 120, "mode": "breadth-first",
                       "chunk": 50, "model": "deepseek-flash"}, f)
        run(binary, "campaign", "start", campaign_file, "--data-dir",
            devices[1]["data"], "--yes")

        # Every chaos knob at its most aggressive; a release build must
        # ignore all of them.
        for index in (1, 2):
            other = 3 - index
            data = devices[index]["data"]
            member_copy = os.path.join(data, "members.json")
            subprocess.run(["cp", members_path, member_copy], check=True)
            port_file = os.path.join(root, "port%d.json" % index)
            env = dict(os.environ,
                       ONION_KILL_AT_COMMIT="1",
                       ONION_CLOCK_JUMP_MS="600000",
                       ONION_SIM_DISK_FULL="1",
                       ONION_PARTITION_FROM=devices[other]["device_id"],
                       ONION_SYNC_DEBUG="1")
            node = subprocess.Popen(
                [binary, "node", "run", "--data-dir", data,
                 "--base-url", "http://127.0.0.1:%d" % fake_port,
                 "--members", member_copy,
                 "--host", "127.0.0.1", "--sync-port", "0",
                 "--api-port", "0", "--swim-port", "0",
                 "--port-file", port_file],
                stdout=open(os.path.join(root, "node%d.log" % index), "w"),
                stderr=subprocess.STDOUT, env=env)
            nodes.append(node)
            ports = wait_port_file(port_file)
            devices[index]["port"] = ports["sync"]
            devices[index]["api"] = ports["api"]
            devices[index]["swim"] = ports["swim"]

        # The urls arrive in the members files; write them and let the
        # nodes reload.
        for index in (1, 2):
            entry = [{"device_id": devices[i]["device_id"],
                      "public_key": devices[i]["public_key"],
                      "url": "127.0.0.1:%d" % devices[i]["port"],
                      "swim": "127.0.0.1:%d" % devices[i]["swim"],
                      "weight": 1.0} for i in (1, 2)]
            with open(os.path.join(devices[index]["data"],
                                   "members.json"), "w") as f:
                json.dump(entry, f)
        time.sleep(2)

        # Both processes must stay alive (no kill at commit) and make
        # progress (appends succeed despite the disk-full knob).
        assert nodes[0].poll() is None, "node 1 died (kill hook active)"
        assert nodes[1].poll() is None, "node 2 died (kill hook active)"
        settled_before = None
        while time.time() < deadline:
            try:
                counts = [state(devices[i]["api"])["settled_nodes"]
                          for i in (1, 2)]
                if counts[0] > 0:
                    settled_before = counts
                    break
            except Exception:
                pass
            time.sleep(1.0)
        assert settled_before is not None, "no progress (disk-full hook?)"

        # The partition knob must not split the fleet: the counts keep
        # converging to the same value.
        converged = False
        while time.time() < deadline:
            try:
                counts = [state(devices[i]["api"])["settled_nodes"]
                          for i in (1, 2)]
                if counts[0] > settled_before[0] and \
                        counts[0] == counts[1]:
                    converged = True
                    break
            except Exception:
                pass
            time.sleep(2.0)
        assert converged, "nodes never converged (partition hook active?)"
        assert nodes[0].poll() is None and nodes[1].poll() is None, (
            "a node died mid-run (kill hook active)")
        print("release ignores chaos hooks: PASS settled=%r"
              % [state(devices[i]["api"])["settled_nodes"]
                 for i in (1, 2)])
    finally:
        for node in nodes:
            node.terminate()
        for node in nodes:
            try:
                node.wait(timeout=10)
            except subprocess.TimeoutExpired:
                node.kill()
        fake.terminate()
        try:
            fake.wait(timeout=10)
        except subprocess.TimeoutExpired:
            fake.kill()
        if os.environ.get("ONION_KEEP_ROOT"):
            print("KEPT ROOT:", root)
        else:
            import shutil
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
