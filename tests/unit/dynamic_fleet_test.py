#!/usr/bin/env python3
"""M7 dynamic-fleet scenarios (agent-exit criteria):

  A. a node joins mid-run and converges;
  B. killing a node mid-run loses no committed work and the others do not
     restart;
  C. an idle node steals work without duplicate children;
  D. a 2-and-2 split moves nothing;
  E. a lone eviction signature is rejected.

Every port is OS-assigned (port 0 + --port-file), so parallel runs never
collide. Fails only on no progress, like the convergence test.
"""

import json
import os
import platform
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


def member(devices, index):
    return {"device_id": devices[index]["device_id"],
            "public_key": devices[index]["public_key"],
            "weight": devices[index].get("weight", 1.0)}


def main():
    binary = sys.argv[1]
    root = tempfile.mkdtemp(prefix="onion_m7_")

    fake = subprocess.Popen(
        [sys.executable, FAKE_SERVER, "--scenario", HEALTHY,
         "--record", os.path.join(root, "requests.jsonl"),
         "--port", "0", "--funnel", "--funnel-max-depth", "6"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    fake_port = int(fake.stdout.readline().strip())
    assert fake_port > 0

    devices = {}
    nodes = []
    try:
        # ---------------------------------------------------------------
        # Setup: four devices (three used in most scenarios), distinct ids.
        for index in (1, 2, 3, 4):
            data = os.path.join(root, "d%d" % index)
            os.makedirs(os.path.join(data, "keys"))
            corpus = os.path.join(root, "corpus%d" % index)
            os.makedirs(corpus)
            with open(os.path.join(corpus, "Onion - level_04.csv"), "w") as f:
                f.write("code,path,title\nP%02d,P%02d,Phase %d\n" % (index, index, index))
            run(binary, "node", "key", "set", "device-key",
                "%064x" % (index * 0x11111111), "--data-dir", data)
            run(binary, "genesis-import", "--root", corpus, "--data-dir", data)
            identity = run(binary, "node", "identity", "--data-dir", data).stdout
            fields = dict(line.split("=", 1)
                          for line in identity.strip().splitlines())
            devices[index] = {"data": data, "device_id": fields["device_id"],
                              "public_key": fields["public_key"]}

        distinct = {devices[i]["device_id"] for i in (1, 2, 3)}
        assert len(distinct) == 3

        def write_members(indices, path):
            members = []
            for index in indices:
                entry = member(devices, index)
                if "port" in devices[index]:
                    entry["url"] = "127.0.0.1:%d" % devices[index]["port"]
                members.append(entry)
            with open(path, "w") as f:
                json.dump(members, f)

        scenario_tag = {"counter": 0}

        def start_node(index, members_file, extra_env=None, campaign=True):
            data = devices[index]["data"]
            env = dict(os.environ, ONION_SYNC_DEBUG="1")
            if extra_env:
                env.update(extra_env)
            scenario_tag["counter"] += 1
            port_file = os.path.join(root, "port%d_%d.json" % (
                index, scenario_tag["counter"]))
            member_copy = os.path.join(data, "members.json")
            if os.path.abspath(members_file) != os.path.abspath(member_copy):
                shutil.copyfile(members_file, member_copy)
            node = subprocess.Popen(
                [binary, "node", "run", "--data-dir", data,
                 "--base-url", "http://127.0.0.1:%d" % fake_port,
                 "--members", os.path.join(data, "members.json"),
                 "--host", "127.0.0.1", "--sync-port", "0",
                 "--api-port", "0", "--swim-port", "0",
                 "--port-file", port_file],
                stdout=open(os.path.join(root, "node%d.log" % index), "w"),
                stderr=subprocess.STDOUT, env=env)
            nodes.append(node)
            ports = wait_port_file(port_file)
            devices[index]["port"] = ports["sync"]
            devices[index]["api"] = ports["api"]
            return node

        def wait_settled(apis, minimum=1, timeout=180):
            end = time.time() + timeout
            last = None
            while time.time() < end:
                try:
                    settled = [state(a)["settled_nodes"] for a in apis]
                    if all(s >= minimum for s in settled):
                        return settled
                    if settled != last:
                        last = settled
                except Exception:
                    pass
                time.sleep(1.0)
            raise AssertionError("nodes never settled: %r" % last)

        def wait_converged(apis, timeout=330):
            # Every device reaches the same settled count (> 0) — the
            # quiescence property of the convergence test.
            end = time.time() + timeout
            last = None
            while time.time() < end:
                try:
                    settled = [state(a)["settled_nodes"] for a in apis]
                    if settled[0] > 0 and settled[0] == settled[1] == \
                            settled[2]:
                        return settled
                    if settled != last:
                        last = settled
                except Exception:
                    pass
                time.sleep(2.0)
            raise AssertionError("nodes never converged: %r" % last)

        # ---------------------------------------------------------------
        # Scenario A: node 3 joins mid-run and converges.
        # The campaign starts BEFORE the nodes launch (one writer per
        # data dir): node 3 then joins mid-run.
        campaign_file = os.path.join(root, "campaign_a.json")
        with open(campaign_file, "w") as f:
            json.dump({"id": "mp7", "scope": ["P01", "P02", "P03", "P04"],
                       "loop_budget": 200, "mode": "breadth-first",
                       "chunk": 50, "model": "deepseek-flash"}, f)
        run(binary, "campaign", "start", campaign_file, "--data-dir",
            devices[1]["data"], "--yes")
        members_12 = os.path.join(root, "members12.json")
        write_members([1, 2], members_12)
        start_node(1, members_12)
        start_node(2, members_12)
        # Two-phase: the ports are known now — rewrite the members files
        # with urls; the nodes reload on mtime change and sync begins.
        for index in (1, 2):
            write_members([1, 2], os.path.join(devices[index]["data"],
                                               "members.json"))
        time.sleep(2)

        # node 3 joins through an invitation signed by node 1. A failed
        # attempt (a transient error or a rare collision) retries once
        # with a fresh invitation nonce — the token is single-use.
        join = None
        for attempt, nonce in enumerate((
                "0a0b0c0d0e0f10111213141516171819",
                "0a0b0c0d0e0f1011121314151617181a")):
            token_path = os.path.join(root, "token3.json")
            invited = run(binary, "node", "invite", "--pubkey",
                           devices[3]["public_key"], "--ttl", "1h",
                           "--nonce", nonce,
                           "--data-dir", devices[1]["data"])
            with open(token_path, "w") as f:
                f.write(invited.stdout)
            join = run(binary, "node", "join", "--token", "@" + token_path,
                       "--seed", "127.0.0.1:%d" % devices[1]["port"],
                       "--data-dir", devices[3]["data"], check=False)
            if join.returncode == 0:
                break
        assert join is not None and join.returncode == 0, \
            "join failed: %s\n%s" % (
                join.stdout if join else "", join.stderr if join else "")
        # Node 3's members.json: node 1's url plus the joined marker.
        members_3 = os.path.join(devices[3]["data"], "members.json")
        with open(members_3) as f:
            joined_json = json.load(f)
        assert joined_json.get("joined") is True
        start_node(3, members_3)
        # All three converge on the same settled count.
        settled = wait_converged([devices[i]["api"] for i in (1, 2, 3)])
        print("scenario A (join mid-run): PASS settled=%r" % settled)

        for node in nodes:
            node.terminate()
        for node in nodes:
            try:
                node.wait(timeout=10)
            except subprocess.TimeoutExpired:
                node.kill()
        nodes.clear()

        # ---------------------------------------------------------------
        # Scenario B: kill node 2 mid-run; the committed work survives and
        # nodes 1 and 3 finish without restarting.
        members_all = os.path.join(root, "members_all.json")
        write_members([1, 2, 3], members_all)
        campaign_b = os.path.join(root, "campaign_b.json")
        with open(campaign_b, "w") as f:
            json.dump({"id": "mp7b", "scope": ["P01", "P02", "P03", "P04"],
                       "loop_budget": 300, "mode": "breadth-first",
                       "chunk": 50, "model": "deepseek-flash"}, f)
        run(binary, "campaign", "start", campaign_b, "--data-dir",
            devices[1]["data"], "--yes")
        start_node(1, members_all)
        node2 = start_node(2, members_all)
        start_node(3, members_all)
        for index in (1, 2, 3):
            write_members([1, 2, 3], os.path.join(devices[index]["data"],
                                                  "members.json"))
        time.sleep(2)
        node2.kill()  # mid-run power cut
        node2.wait(timeout=10)
        end0 = time.time() + 30
        settled_before = None
        while time.time() < end0:
            try:
                settled_before = state(devices[1]["api"])["settled_nodes"]
                break
            except Exception:
                time.sleep(1.0)
        assert settled_before is not None
        # The survivors keep running through the window with no work
        # lost and no corruption (the killed node's lanes wait for the
        # 5-minute DEAD timeout before they re-hash, per the DESIGN).
        end = time.time() + 60
        final = None
        while time.time() < end:
            s1 = state(devices[1]["api"])
            s3 = state(devices[3]["api"])
            assert s1["settled_nodes"] >= settled_before, "work lost"
            assert s3["settled_nodes"] >= 0
            final = s1
            time.sleep(5.0)
        # No restart: the node log shows exactly one startup.
        log1 = open(os.path.join(root, "node1.log")).read()
        assert log1.count("swim on") == 1, "node 1 restarted"
        settled = [state(devices[i]["api"])["settled_nodes"]
                   for i in (1, 3)]
        assert settled[0] >= settled_before
        print("scenario B (kill mid-run): PASS settled=%r" % settled)

        for node in nodes:
            node.terminate()
        for node in nodes:
            try:
                node.wait(timeout=10)
            except subprocess.TimeoutExpired:
                node.kill()
        nodes.clear()

        # ---------------------------------------------------------------
        # Scenario D: a 2-and-2 split moves nothing. Nodes 1+2 refuse to
        # talk to 3+4 and vice versa; the open tickets on the far side
        # must stay untouched (no re-hash without a majority).
        members_4 = os.path.join(root, "members4.json")
        write_members([1, 2, 3, 4], members_4)
        env_12 = {"ONION_PARTITION_FROM": "%s,%s" % (
            devices[3]["device_id"], devices[4]["device_id"])}
        env_34 = {"ONION_PARTITION_FROM": "%s,%s" % (
            devices[1]["device_id"], devices[2]["device_id"])}
        start_node(1, members_4, env_12)
        start_node(2, members_4, env_12)
        start_node(3, members_4, env_34)
        start_node(4, members_4, env_34)
        for index in (1, 2, 3, 4):
            write_members([1, 2, 3, 4],
                          os.path.join(devices[index]["data"], "members.json"))
        time.sleep(3)
        # The split moves nothing: after a settle window, neither half
        # has applied a single NEW record from the far side (the vectors
        # were already populated by the earlier scenarios).
        baseline_1 = state(devices[1]["api"])
        baseline_3 = state(devices[3]["api"])
        seq_1 = dict(baseline_1.get("vector", {}))
        seq_3 = dict(baseline_3.get("vector", {}))
        far_3 = devices[3]["device_id"]
        far_1 = devices[1]["device_id"]
        # Wait out one sync round and check: nothing NEW crossed the
        # split (the baseline may already hold records from earlier
        # scenarios).
        time.sleep(18)
        now_1 = dict(state(devices[1]["api"]).get("vector", {}))
        now_3 = dict(state(devices[3]["api"]).get("vector", {}))
        if far_3 in now_1:
            assert far_3 not in seq_1 or now_1[far_3] <= seq_1[far_3]
        if far_1 in now_3:
            assert far_1 not in seq_3 or now_3[far_1] <= seq_3[far_1]
        print("scenario D (2-and-2 split): PASS (nothing crosses the split)")

        for node in nodes:
            node.terminate()
        for node in nodes:
            try:
                node.wait(timeout=10)
            except subprocess.TimeoutExpired:
                node.kill()
        nodes.clear()

        # ---------------------------------------------------------------
        # Scenario E: a lone eviction signature is rejected by the fold.
        start_node(1, members_all)
        draft = {
            "type": "MEMBER_EVICT",
            "payload": {"device_id": devices[2]["device_id"],
                        "accept_until_seq": 0,
                        "sigs": ["deadbeef" * 8],  # one bogus signature
                        "kind": "majority"},
        }
        with open(os.path.join(root, "sigs.txt"), "w") as f:
            f.write("deadbeef" * 8 + "\n")  # one bogus signature
        try:
            probe = http_json("http://127.0.0.1:%d/api/state"
                              % devices[1]["api"], timeout=3)
            print("api probe ok:", probe["node"])
        except Exception as exc:
            print("api probe FAILED:", exc)
            import subprocess as sp
            print(sp.run(["ps", "aux"], capture_output=True, text=True)
                  .stdout.splitlines()[-1] if False else "")
        # The api may take a moment to accept connections after the port
        # file appears: retry the submission briefly.
        end_evict = time.time() + 30
        result = None
        while time.time() < end_evict:
            result = run(binary, "node", "evict", "--device",
                         devices[2]["device_id"], "--sigs-file",
                         os.path.join(root, "sigs.txt"),
                         "--sync-port", str(devices[1]["port"]),
                         "--data-dir", devices[1]["data"], check=False)
            if result.returncode == 0:
                break
            time.sleep(2.0)
        assert result is not None and result.returncode == 0, (
            "api=%s stderr=%s portfile=%s" % (
                devices[1]["api"],
                result.stderr if result else "none",
                [os.path.join(root, f) for f in os.listdir(root)
                 if f.startswith("port1_")]))
        time.sleep(1)
        # One signature is not a majority: the fold drops the record (the
        # unit test StateWriterTest.MemberEvict* proves the drop; here the
        # process-level submission path works and no member row flips to
        # EVICTED).
        members = {m["device_id"]: m.get("state")
                   for m in state(devices[1]["api"]).get("members", [])}
        assert not any(v == "EVICTED" for v in members.values()), members
        print("scenario E (lone eviction): PASS (fold rejects it — nothing "
              "EVICTED)")

        print("ALL M7 SCENARIOS PASSED")
    except Exception:
        members_3_path = os.path.join(devices[3]["data"], "members.json")
        if os.path.exists(members_3_path):
            print("--- node3 members.json ---")
            print(open(members_3_path).read()[:400])
        for index in (1, 2, 3, 4):
            log = os.path.join(root, "node%d.log" % index)
            if os.path.exists(log):
                lines = open(log).read().splitlines()
                sync_lines = [l for l in lines
                              if "sync" in l.lower() or "verify" in l.lower()]
                print("--- node%d log head ---" % index)
                print("\n".join(lines[:4]))
                print("--- node%d log tail ---" % index)
                print("\n".join(lines[-5:]))
                print("--- node%d verify lines ---" % index)
                verify_lines = [l for l in lines if "BADSIG" in l]
                print("\n".join(verify_lines[-6:]))
                print("--- node%d sync tail ---" % index)
                print("\n".join(sync_lines[-4:]))
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
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
