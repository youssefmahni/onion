#!/usr/bin/env python3
"""Genesis import: fixture corpus with duplicates, proposal, resolution, verify."""
import json
import os
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.run(args, capture_output=True, text=True)


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as root:
        with tempfile.TemporaryDirectory() as data:
            corpus = os.path.join(root, "corpus")
            os.makedirs(corpus)
            with open(os.path.join(corpus, "Onion - level_00.csv"), "w") as f:
                f.write("code,title\nP01,Phase One\nP02,Phase Two\n")
            with open(os.path.join(corpus, "Onion - level_01.csv"), "w") as f:
                f.write("code,path,title\n"
                        "IN01,P01-IN01,In One\n"
                        "IN02,P01-IN02,In Two\n"
                        "IN01,P01-IN01,In One Renamed\n")
            with open(os.path.join(corpus, "Onion - level_02.csv"), "w") as f:
                f.write("code,path,title\n"
                        "T01,P01-IN01-T01,Task One\n"
                        "T02,P01-IN01-T02,Task Two\n")
            # A duplicate nested under the duplicated path.
            with open(os.path.join(corpus, "Onion - level_03.csv"), "w") as f:
                f.write("code,path,title\n"
                        "ST01,P01-IN01-T01-ST01,Sub One\n")

            # Generate the device key first: the node runs forever, so start
            # it detached, wait for the key, then kill it.
            import time
            node = subprocess.Popen(
                [binary, "node", "run", "--data-dir", data,
                 "--base-url", "http://127.0.0.1:9", "--api-port", "18099"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(100):
                if os.path.exists(os.path.join(data, "keys", "device-key")):
                    break
                time.sleep(0.1)
            node.terminate()
            node.wait(timeout=10)

            # Without a resolution the import refuses.
            refused = run(binary, "genesis-import", "--root", corpus,
                          "--data-dir", data)
            assert refused.returncode == 2, refused.stdout + refused.stderr
            assert "duplicate paths found" in refused.stderr

            proposal = os.path.join(corpus, "proposal.tsv")
            proposed = run(binary, "genesis-import", "--root", corpus,
                           "--data-dir", data, "--propose", proposal)
            assert proposed.returncode == 0, proposed.stderr
            content = open(proposal).read()
            assert "P01-IN01" in content

            imported = run(binary, "genesis-import", "--root", corpus,
                           "--data-dir", data, "--resolution", proposal)
            assert imported.returncode == 0, imported.stdout + imported.stderr
            assert "VERIFIED" in imported.stdout
            assert "orphans=0" in imported.stdout
            assert "L0=2" in imported.stdout
            assert "L3=1" in imported.stdout
            print("genesis import test: PASS")


if __name__ == "__main__":
    main()
