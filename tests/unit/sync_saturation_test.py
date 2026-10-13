#!/usr/bin/env python3
"""Sync decoupling under LLM saturation (M8 pre-work).

The root cause of the sync.multi_process_convergence CI flake: the sync
round lived INSIDE the node loop, after engine.run(), so a device whose
engine was busy in a long LLM call stopped syncing for minutes. This test
saturates node 1 with 60 s-per-call fake-LLM latency and proves the
decoupled scheduler: node 1's sync rounds keep firing at ~15 s cadence
(at most every 20 s) while a call is in flight, and node 2 learns the
campaign within two rounds.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FAKE_SERVER = os.path.join(ROOT, "tools", "fake_deepseek", "fake_deepseek.py")
HEALTHY = os.path.join(ROOT, "tools", "fake_deepseek", "scenarios", "healthy.json")
HARD_DEADLINE_S = float(os.environ.get("ONION_SYNC_SAT_DEADLINE_S", "180"))


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


def seed_hex(base):
    return "".join("%02x" % ((base + i) % 256) for i in range(32))


def main():
    binary = sys.argv[1]
    started = time.time()
    deadline = started + HARD_DEADLINE_S
    root = tempfile.mkdtemp(prefix="onion_sat_")

    fake = subprocess.Popen(
        [sys.executable, FAKE_SERVER, "--scenario", HEALTHY,
         "--record", os.path.join(root, "requests.jsonl"),
         "--port", "0", "--funnel", "--funnel-max-depth", "6",
         "--delay-ms", "60000"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    fake_port = int(fake.stdout.readline().strip())
    assert fake_port > 0

    devices = {}
    nodes = []
    try:
        for index in (1, 2):
            data = os.path.join(root, "d%d" % index)
            os.makedirs(os.path.join(data, "keys"))
            corpus = os.path.join(root, "corpus%d" % index)
            os.makedirs(corpus)
            with open(os.path.join(corpus, "Onion - level_04.csv"), "w") as f:
                f.write("code,path,title\nP0%d,P0%d,Phase %d\n"
                        % (index, index, index))
            run(binary, "node", "key", "set", "device-key",
                seed_hex(index * 16), "--data-dir", data)
            run(binary, "genesis-import", "--root", corpus, "--data-dir", data)
            identity = run(binary, "node", "identity", "--data-dir", data).stdout
            fields = dict(line.split("=", 1)
                          for line in identity.strip().splitlines())
            devices[index] = {"data": data,
                              "device_id": fields["device_id"],
                              "public_key": fields["public_key"]}

        campaign = os.path.join(root, "campaign.json")
        with open(campaign, "w") as f:
            json.dump({"id": "mp_sat", "scope": ["P01"],
                       "loop_budget": 20, "mode": "breadth-first",
                       "chunk": 50, "model": "deepseek-flash"}, f)
        run(binary, "campaign", "start", campaign, "--data-dir",
            devices[1]["data"], "--yes")

        members = [
            {"device_id": devices[i]["device_id"],
             "public_key": devices[i]["public_key"], "weight": 1.0}
            for i in (1, 2)
        ]
        for index in (1, 2):
            with open(os.path.join(root, "d%d" % index, "members.json"),
                      "w") as f:
                json.dump(members, f)

        for index in (1, 2):
            data = devices[index]["data"]
            nodes.append(subprocess.Popen(
                [binary, "node", "run", "--data-dir", data,
                 "--base-url", "http://127.0.0.1:%d" % fake_port,
                 "--members", os.path.join(data, "members.json"),
                 "--host", "127.0.0.1", "--sync-port", "0",
                 "--api-port", "0", "--swim-port", "0",
                 "--port-file", os.path.join(root, "port%d.json" % index)],
                stdout=open(os.path.join(root, "node%d.log" % index), "w"),
                stderr=subprocess.STDOUT,
                env=dict(os.environ, ONION_SYNC_DEBUG="1")))
        ports = {}
        for index in (1, 2):
            ports[index] = wait_port_file(
                os.path.join(root, "port%d.json" % index), timeout=30)
        members[0]["url"] = "127.0.0.1:%d" % ports[1]["sync"]
        members[0]["swim"] = "127.0.0.1:%d" % ports[1]["swim"]
        members[1]["url"] = "127.0.0.1:%d" % ports[2]["sync"]
        members[1]["swim"] = "127.0.0.1:%d" % ports[2]["swim"]
        for index in (1, 2):
            with open(os.path.join(root, "d%d" % index, "members.json"),
                      "w") as f:
                json.dump(members, f)

        # Node 1 starts its first LLM call almost at once; the 60 s
        # latency pins it inside engine.run(). Wait until the fake has
        # seen that first request (the call is now in flight).
        requests_path = os.path.join(root, "requests.jsonl")
        end = time.time() + 60
        while not os.path.exists(requests_path) and time.time() < end:
            time.sleep(0.5)
        assert os.path.exists(requests_path), "no LLM request reached the fake"
        with open(requests_path) as f:
            first_request_lines = sum(1 for _ in f)
        assert first_request_lines >= 1

        # The measured window: node 1's rounds must keep firing while the
        # call is still in flight (the window closes well before the call
        # returns at ~60 s from its receipt).
        rounds_before = state(ports[1]["api"]).get("sync_rounds_completed", 0)
        learned = False
        learned_at = None
        rounds_ok = False
        end = time.time() + 40
        while time.time() < end and time.time() < deadline:
            s1 = state(ports[1]["api"])
            rounds = s1.get("sync_rounds_completed", 0)
            if rounds >= rounds_before + 2:
                rounds_ok = True
            if not learned:
                s2 = state(ports[2]["api"])
                if any(c.get("id") == "mp_sat"
                       for c in s2.get("campaigns", [])):
                    learned = True
                    learned_at = time.time() - started
            if rounds_ok and learned:
                break
            time.sleep(1.0)

        assert rounds_ok, (
            "node 1 sync rounds stalled under saturation: before=%d now=%d"
            % (rounds_before,
               state(ports[1]["api"]).get("sync_rounds_completed", 0)))
        assert learned, "node 2 never learned the campaign (2 rounds)"
        print("sync saturation decoupling: PASS "
              "rounds=%d->%d campaign-learned-at=%.1fs"
              % (rounds_before,
                 state(ports[1]["api"]).get("sync_rounds_completed", 0),
                 learned_at))
    except Exception:
        for index in (1, 2):
            log = os.path.join(root, "node%d.log" % index)
            if os.path.exists(log):
                print("--- node%d log tail ---" % index)
                print("\n".join(open(log).read().splitlines()[-15:]))
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
        try:
            fake.wait(timeout=10)
        except subprocess.TimeoutExpired:
            fake.kill()
        if os.environ.get("ONION_KEEP_ROOT"):
            print("KEPT ROOT:", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
