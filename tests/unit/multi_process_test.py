#!/usr/bin/env python3
"""Multi-process M6 test: three real node processes sync and converge.

Deterministic: every device key comes from a fixed seed, so the HRW lanes
are identical on every run. After the campaign's work finishes, the test
waits for quiescence (no new records for three sync rounds), then asserts
that every device sees the campaign finished (exhausted or stopped per
decision 0010), the same settled count and the same Merkle root.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FAKE_SERVER = str(ROOT / "tools" / "fake_deepseek" / "fake_deepseek.py")
HEALTHY_SCENARIO = str(ROOT / "tools" / "fake_deepseek" / "scenarios" / "healthy.json")

# The local verification loop runs at 120 s; the CI sanitizer and hosted
# macOS jobs are slower, so ctest overrides this through the environment.
HARD_DEADLINE_S = float(os.environ.get("ONION_MP_DEADLINE_S", "120"))
SYNC_ROUND_S = 15.0


class DeadlineExceeded(Exception):
    pass


def seed_hex(base):
    return "".join("%02x" % ((base + i) % 256) for i in range(32))


def wait_for(predicate, timeout, deadline, what):
    end = time.time() + timeout
    while time.time() < end:
        if time.time() > deadline:
            raise DeadlineExceeded(what)
        if predicate():
            return True
        time.sleep(0.5)
    return False


def http_json(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read())


def run(binary, *args, check=True):
    return subprocess.run([binary, *args], check=check, capture_output=True,
                          text=True)


def node_state(port):
    return http_json("http://127.0.0.1:%d/api/state" % port)


def dump_debug_state(binary, data, members_path):
    return run(binary, "node", "debug-state", "--data-dir", data,
               "--members", members_path).stdout


def main():
    binary = sys.argv[1]
    started = time.time()
    started = time.time()
    deadline = started + HARD_DEADLINE_S
    root = tempfile.mkdtemp(prefix="onion_mp_")

    fake = subprocess.Popen(
        [sys.executable, FAKE_SERVER, "--scenario", HEALTHY_SCENARIO,
         "--record", os.path.join(root, "requests.jsonl"),
         "--port", "0", "--funnel", "--funnel-max-depth", "3"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    fake_port = int(fake.stdout.readline().strip())
    assert fake_port > 0, "fake server did not report its port"

    devices = {}
    try:
        for index in (1, 2, 3):
            data = os.path.join(root, f"d{index}")
            os.makedirs(os.path.join(data, "keys"))
            corpus = os.path.join(root, f"corpus{index}")
            os.makedirs(corpus)
            with open(os.path.join(corpus, "Onion - level_00.csv"), "w") as f:
                f.write(f"code,title\nP0{index},Phase {index}\n")
            # Fixed seed: the device identity (and so the lanes) are the
            # same on every run.
            run(binary, "node", "key", "set", "device-key",
                seed_hex(index * 16), "--data-dir", data)
            run(binary, "genesis-import", "--root", corpus, "--data-dir", data)
            identity = run(binary, "node", "identity", "--data-dir", data).stdout
            fields = dict(line.split("=", 1)
                          for line in identity.strip().splitlines())
            devices[index] = {"data": data, "device_id": fields["device_id"],
                              "public_key": fields["public_key"]}

        distinct_ids = {devices[i]["device_id"] for i in (1, 2, 3)}
        assert len(distinct_ids) == 3, \
            "devices share identities: %r" % distinct_ids

        members = [
            {"device_id": devices[i]["device_id"],
             "public_key": devices[i]["public_key"],
             "weight": 1.0}
            for i in (1, 2, 3)
        ]
        for index in (1, 2, 3):
            with open(os.path.join(root, f"d{index}", "members.json"), "w") as f:
                json.dump(members, f)

        campaign = {"id": "mp1", "scope": ["P01", "P02", "P03"],
                    "loop_budget": 100, "mode": "breadth-first", "chunk": 50,
                    "model": "deepseek-flash"}
        with open(os.path.join(root, "campaign.json"), "w") as f:
            json.dump(campaign, f)
        run(binary, "campaign", "start", os.path.join(root, "campaign.json"),
            "--data-dir", devices[1]["data"], "--yes")

        nodes = []
        for index in (1, 2, 3):
            data = devices[index]["data"]
            nodes.append(subprocess.Popen(
                [binary, "node", "run",
                 "--data-dir", data,
                 "--base-url", "http://127.0.0.1:%d" % fake_port,
                 "--members", os.path.join(data, "members.json"),
                 "--host", "127.0.0.1", "--sync-port", "0",
                 "--api-port", "0", "--swim-port", "0",
                 "--port-file", os.path.join(root, f"port{index}.json")],
                stdout=open(os.path.join(root, f"node{index}.log"), "w"),
                stderr=subprocess.STDOUT,
                env=dict(os.environ, ONION_SYNC_DEBUG="1")))

        # Read each node's REAL bound ports (bind-0 + port files: no fixed
        # ports anywhere, so parallel runs never collide), then fill the
        # peer urls into members.json — the nodes reload it on mtime
        # change and the sync rounds begin.
        ports = {}
        for index in (1, 2, 3):
            port_file = os.path.join(root, f"port{index}.json")
            end = time.time() + 30
            while not os.path.exists(port_file) and time.time() < end:
                time.sleep(0.2)
            assert os.path.exists(port_file), \
                "node %d never wrote its port file" % index
            with open(port_file) as f:
                ports[index] = json.load(f)
            assert ports[index]["sync"] > 0 and ports[index]["api"] > 0
        for i, index in enumerate((1, 2, 3), start=1):
            members[i - 1]["url"] = "127.0.0.1:%d" % ports[index]["sync"]
        for index in (1, 2, 3):
            with open(os.path.join(devices[index]["data"], "members.json"),
                      "w") as f:
                json.dump(members, f)

        # 1. Wait for the campaign to finish on every device (decision
        #    0010: exhausted, i.e. its RESERVE fold reaches the budget, or
        #    stopped — never a raw timer).
        def finished_everywhere():
            try:
                for index in (1, 2, 3):
                    state = node_state(ports[index]["api"])
                    campaigns = state.get("campaigns", [])
                    campaign_state = campaigns[0] if campaigns else None
                    if campaign_state is None:
                        return False
                    if campaign_state["state"] == "stopped":
                        continue
                    budget = (campaign_state.get("spec") or {}).get(
                        "loop_budget", 0)
                    if campaign_state.get("reserved", 0) < budget:
                        return False
                return True
            except Exception:
                return False
        # Leave room for the three quiescence rounds after the work.
        work_wait = max(60.0, HARD_DEADLINE_S - 90.0)
        if not wait_for(finished_everywhere, work_wait, deadline,
                        "campaign never finished"):
            raise AssertionError("campaign never finished everywhere")

        # 2. Quiescence: no node may write a new record for three sync
        #    rounds, so the last batch has reached every replica. A
        #    transient HTTP error on a loaded machine must not kill the
        #    run: this round simply does not count.
        def vector_signature():
            signature = []
            for index in (1, 2, 3):
                state = node_state(ports[index]["api"])
                signature.append(tuple(sorted(state.get("vector", {}).items())))
            return signature

        previous = None
        quiescent_rounds = 0
        while quiescent_rounds < 3:
            if time.time() > deadline:
                raise DeadlineExceeded("quiescence")
            time.sleep(SYNC_ROUND_S)
            try:
                current = vector_signature()
            except (urllib.error.URLError, OSError,
                    json.JSONDecodeError):
                # A transient HTTP/parse error on a loaded machine: this
                # round simply does not count. A persistently dead node
                # keeps resetting until DeadlineExceeded fires.
                quiescent_rounds = 0
                previous = None
                continue
            if current == previous and previous is not None:
                quiescent_rounds += 1
            else:
                quiescent_rounds = 0
            previous = current

        # 3. Convergence assertions.
        states = [node_state(ports[index]["api"]) for index in (1, 2, 3)]
        for index, state in enumerate(states, start=1):
            campaigns = state.get("campaigns", [])
            campaign_state = campaigns[0] if campaigns else {}
            budget = (campaign_state.get("spec") or {}).get("loop_budget", 0)
            finished = (campaign_state.get("state") == "stopped" or
                        campaign_state.get("reserved", 0) >= budget)
            assert finished, f"device {index} not finished: {campaign_state}"
        settled = [state["settled_nodes"] for state in states]
        assert max(settled) > 0, "nothing settled anywhere"
        assert settled[0] == settled[1] == settled[2], settled
        merkle = [state["merkle_root"] for state in states]
        assert merkle[0] == merkle[1] == merkle[2], merkle
        print("multi-process convergence: PASS settled=%s merkle=%s"
              % (settled, merkle[0][:16]))
    except (AssertionError, DeadlineExceeded) as error:
        print("FAILED: %s" % error)
        for index in (1, 2, 3):
            data = devices.get(index, {}).get("data")
            if not data:
                continue
            print("--- device %d debug-state ---" % index)
            try:
                print(dump_debug_state(
                    binary, data, os.path.join(root, f"d{index}", "members.json")))
            except Exception as exc:
                print("unreachable: %s" % exc)
            log_path = os.path.join(root, f"node{index}.log")
            if os.path.exists(log_path):
                print("--- device %d log tail ---" % index)
                print(open(log_path).read()[-1500:])
                lines = open(log_path).read().splitlines()
                sync_lines = [l for l in lines if "sync" in l.lower()]
                print("--- device %d sync lines (%d) ---" % (index, len(sync_lines)))
                print("\n".join(sync_lines[-10:]))
        raise
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
