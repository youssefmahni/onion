#!/usr/bin/env python3
"""M8 pre-work: the 0014 majority-round budget scenario.

A 5-node fleet splits 2/3 NEAR BUDGET EXHAUSTION. Decision 0014 amendment:
a sync round is complete only when a MAJORITY of members (counting self)
answered within its timeouts, so only the majority side (3 nodes) may
reserve new chunks; the minority side (2 nodes) freezes. The restart
loophole is closed too: the reservation gate starts CLOSED after every
restart, so a minority node that restarts five times during the partition
still reserves nothing.

Deterministic: fixed device seeds (the HRW lanes are identical on every
run), a fixed 200 ms fake-LLM latency pins the measured chunk at the
50-attempt floor, and the split happens at a polled reservation level.
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
HARD_DEADLINE_S = float(os.environ.get("ONION_MP_DEADLINE_S", "360"))

BUDGET = 400
CHUNK_FLOOR = 50
MAJORITY = (1, 2, 3)
MINORITY = (4, 5)


def http_json(url, timeout=10):
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read())


def run(binary, *args, check=True):
    try:
        return subprocess.run([binary, *args], check=check,
                              capture_output=True, text=True)
    except subprocess.CalledProcessError as exc:
        raise AssertionError(
            "command failed: %r\nstdout: %s\nstderr: %s" % (
                [binary, *args], exc.stdout, exc.stderr)) from exc


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
    if (os.environ.get("ASAN_OPTIONS") or os.environ.get("TSAN_OPTIONS") or
            os.environ.get("UBSAN_OPTIONS")):
        print("SKIP: sanitizer builds run the scenarios too slowly")
        sys.exit(77)
    probe = run(binary, "node", "chaos-hooks").stdout.strip()
    if probe != "1":
        print("SKIP: chaos hooks not compiled in (release build)")
        sys.exit(77)

    root = tempfile.mkdtemp(prefix="onion_partbudget_")
    fake = subprocess.Popen(
        [sys.executable, FAKE_SERVER, "--scenario", HEALTHY,
         "--record", os.path.join(root, "requests.jsonl"),
         "--port", "0", "--funnel", "--funnel-max-depth", "6",
         "--delay-ms", "200"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    fake_port = int(fake.stdout.readline().strip())
    assert fake_port > 0

    devices = {}
    procs = {}

    def stop_all():
        for node in list(procs.values()):
            node.terminate()
        for node in list(procs.values()):
            try:
                node.wait(timeout=10)
            except subprocess.TimeoutExpired:
                node.kill()
        procs.clear()

    try:
        for index in range(1, 6):
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

        members = [
            {"device_id": devices[i]["device_id"],
             "public_key": devices[i]["public_key"], "weight": 1.0}
            for i in range(1, 6)
        ]
        campaign = os.path.join(root, "campaign.json")
        with open(campaign, "w") as f:
            json.dump({"id": "mp_budget", "scope": ["P01", "P02", "P03",
                                                    "P04", "P05"],
                       "loop_budget": BUDGET, "mode": "breadth-first",
                       "chunk": CHUNK_FLOOR, "model": "deepseek-flash"}, f)
        run(binary, "campaign", "start", campaign, "--data-dir",
            devices[1]["data"], "--yes")

        def write_members(index, entries):
            with open(os.path.join(devices[index]["data"], "members.json"),
                      "w") as f:
                json.dump(entries, f)

        def start_node(index, extra_env=None):
            data = devices[index]["data"]
            env = dict(os.environ, ONION_SYNC_DEBUG="1")
            if extra_env:
                env.update(extra_env)
            members_copy = os.path.join(data, "members.json")
            port_file = os.path.join(root, "port%d.json" % index)
            # The previous run's port file is stale: drop it so the wait
            # never reads a dead port.
            if os.path.exists(port_file):
                os.remove(port_file)
            node = subprocess.Popen(
                [binary, "node", "run", "--data-dir", data,
                 "--base-url", "http://127.0.0.1:%d" % fake_port,
                 "--members", members_copy,
                 "--host", "127.0.0.1", "--sync-port", "0",
                 "--api-port", "0", "--swim-port", "0",
                 "--port-file", port_file],
                stdout=open(os.path.join(root, "node%d.log" % index), "a"),
                stderr=subprocess.STDOUT, env=env)
            procs[index] = node
            ports = wait_port_file(port_file, timeout=30)
            devices[index]["api"] = ports["api"]
            devices[index]["sync"] = ports["sync"]
            devices[index]["swim"] = ports["swim"]
            return node

        # Two-phase launch: start all five, then fill the urls (in place,
        # so every later restart also writes the complete list).
        for index in range(1, 6):
            write_members(index, members)
            start_node(index)
        for entry in members:
            for i in range(1, 6):
                if entry["device_id"] == devices[i]["device_id"]:
                    entry["url"] = "127.0.0.1:%d" % devices[i]["sync"]
                    entry["swim"] = "127.0.0.1:%d" % devices[i]["swim"]
        for index in range(1, 6):
            write_members(index, members)

        def uncapped(api):
            campaigns = state(api).get("campaigns", [])
            return sum(c.get("reserved_uncapped", 0) for c in campaigns)

        def reserved(api):
            campaigns = state(api).get("campaigns", [])
            return sum(c.get("reserved", 0) for c in campaigns)

        def own_reserved_view(api, device_id):
            campaigns = state(api).get("campaigns", [])
            for c in campaigns:
                by_origin = c.get("reserved_by_origin", {})
                if device_id in by_origin:
                    return by_origin[device_id]
            return 0

        # Warm-up: reservations accumulate one chunk per device per
        # completed round (the gate opens at the first majority round).
        # Device 1's OWN reservations grow in 50-attempt steps (never in
        # bursts), so wait for two of them: the fleet is then near
        # exhaustion (roughly half to three quarters spent, never
        # finished). Cross-device propagation lag only makes the split
        # LATER views diverge, which the snapshot assertions absorb.
        def own_reserved(api, device_id):
            campaigns = state(api).get("campaigns", [])
            for c in campaigns:
                by_origin = c.get("reserved_by_origin", {})
                if device_id in by_origin:
                    return by_origin[device_id]
            return 0

        end = time.time() + 240
        while time.time() < end and time.time() < deadline:
            try:
                if own_reserved(devices[1]["api"],
                                devices[1]["device_id"]) >= 100:
                    break
            except Exception:
                pass
            time.sleep(1.0)
        assert own_reserved(devices[1]["api"], devices[1]["device_id"]) \
            >= 100, "device 1 never reserved pre-split"

        # Snapshot every device's view, then kill the fleet. The own-origin
        # reserve totals are the race-free minority oracle: only the
        # device itself writes them, and its gate can never reopen inside
        # the split. (A device's whole VIEW can still absorb one last
        # pre-split push between the snapshot and the kill — ignored.)
        pre_split = {}
        for index in range(1, 6):
            pre_split[index] = {
                "uncapped": uncapped(devices[index]["api"]),
                "reserved": reserved(devices[index]["api"]),
                "own": own_reserved_view(devices[index]["api"],
                                         devices[index]["device_id"]),
            }
        stop_all()

        # The split: 1,2,3 refuse 4,5 and vice versa.
        env_123 = {"ONION_PARTITION_FROM": "%s,%s" % (
            devices[4]["device_id"], devices[5]["device_id"])}
        env_45 = {"ONION_PARTITION_FROM": "%s,%s,%s" % (
            devices[1]["device_id"], devices[2]["device_id"],
            devices[3]["device_id"])}
        for index in MAJORITY:
            start_node(index, env_123)
        for index in MINORITY:
            start_node(index, env_45)
        # The restarts bound NEW ephemeral ports: refill the shared member
        # list and rewrite every members.json before the first round.
        for entry in members:
            for i in range(1, 6):
                if entry["device_id"] == devices[i]["device_id"]:
                    entry["url"] = "127.0.0.1:%d" % devices[i]["sync"]
                    entry["swim"] = "127.0.0.1:%d" % devices[i]["swim"]
        for index in range(1, 6):
            write_members(index, members)

        # One round post-split so every device's own view settles (the
        # majority's gate opens at its first completed round; the
        # minority's never does).
        time.sleep(20)

        # The minority froze exactly: no NEW RESERVE from its own devices
        # (only a minority device writes its own origin's records, and
        # its gate never reopens inside the split).
        for index in MINORITY:
            now_own = own_reserved_view(devices[index]["api"],
                                        devices[index]["device_id"])
            assert now_own == pre_split[index]["own"], (
                "minority device %d reserved after the split: %d -> %d"
                % (index, pre_split[index]["own"], now_own))
            rounds = state(devices[index]["api"]).get(
                "sync_rounds_completed", 0)
            assert rounds == 0, (
                "minority device %d completed a round after the split "
                "(%d)" % (index, rounds))

        # The majority side keeps reserving toward the cap and its fold
        # reaches the budget with at most one chunk per majority device.
        # (On a fast machine the pre-split fill can already exhaust the
        # budget on one device's view — that device then has nothing
        # left to reserve, which is fine: the rule's guarantees are the
        # minority freeze and the bound, not the majority's growth.)
        for index in MAJORITY:
            now = uncapped(devices[index]["api"])
            assert now >= pre_split[index]["uncapped"], (
                "majority device %d lost reservations: %d -> %d"
                % (index, pre_split[index]["uncapped"], now))
            assert now <= BUDGET + 3 * CHUNK_FLOOR, (
                "majority device %d overspent: %d" % (index, now))
        end = time.time() + 240
        filled = False
        while time.time() < end and time.time() < deadline:
            if all(reserved(devices[i]["api"]) >= BUDGET
                   for i in MAJORITY):
                filled = True
                break
            time.sleep(2.0)
        assert filled, "majority side never filled the budget"

        # The restart loophole is closed: a minority node restarted five
        # times during the partition reserves nothing each time (the gate
        # starts closed; 2-of-5 answers is never a majority).
        frozen = pre_split[4]["own"]
        for restart in range(5):
            procs[4].terminate()
            try:
                procs[4].wait(timeout=10)
            except subprocess.TimeoutExpired:
                procs[4].kill()
            del procs[4]
            start_node(4, env_45)
            # One round of the restarted node (it can only reach node 5):
            # no majority, no completed rounds, no reservation.
            time.sleep(20)
            now_own = own_reserved_view(devices[4]["api"],
                                        devices[4]["device_id"])
            assert now_own == frozen, (
                "restart %d of minority node 4 reserved something: %d -> %d"
                % (restart + 1, frozen, now_own))
            rounds = state(devices[4]["api"]).get(
                "sync_rounds_completed", 0)
            assert rounds == 0, (
                "restart %d of minority node 4 completed a round (%d)"
                % (restart + 1, rounds))

        print("partition budget majority: PASS "
              "frozen-minority=%d" % frozen)
    except Exception:
        for index in range(1, 6):
            log = os.path.join(root, "node%d.log" % index)
            if os.path.exists(log):
                lines = open(log).read().splitlines()
                print("--- node%d log tail ---" % index)
                print("\n".join(lines[-8:]))
        raise
    finally:
        stop_all()
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
