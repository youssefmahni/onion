#!/usr/bin/env python3
"""m0_clean_failure_rows.py — M0: remove ledger rows written by API failures.

Before the M0 fixes, a node whose API calls failed twice was settled in
funnel_state.tsv with kept=0, atomic=0, retries>=1, score=0, so later runs
skipped it forever. With the mkstemp bug, EVERY real call failed, so a real run
may have written many such rows. This finds them and, with --apply, removes
them (after saving a backup) so those nodes run again.

Usage:
  python3 m0_clean_failure_rows.py [project_dir]            # preview only
  python3 m0_clean_failure_rows.py [project_dir] --apply    # rewrite, keeps a .bak
"""
import os, shutil, sys, time

def is_failure_row(cols):
    # path, kept, atomic, retries, score, unix_ts
    try:
        return (len(cols) >= 5 and int(cols[1]) == 0 and int(cols[2]) == 0
                and int(cols[3]) >= 1 and float(cols[4]) == 0.0)
    except ValueError:
        return False

def main(argv):
    apply = "--apply" in argv
    args = [a for a in argv if not a.startswith("--")]
    root = args[0] if args else "."
    path = os.path.join(root, "funnel_state.tsv")
    if not os.path.exists(path):
        print(f"no ledger at {path}")
        return 1
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines(keepends=True)
    keep, drop = [], []
    for line in lines:
        (drop if is_failure_row(line.rstrip("\n").split("\t")) else keep).append(line)
    print(f"{len(lines)} rows; {len(drop)} failure rows (kept=0, atomic=0, retries>=1, score=0)")
    for line in drop[:10]:
        print("  " + line.rstrip("\n"))
    if len(drop) > 10:
        print(f"  … and {len(drop) - 10} more")
    if not apply:
        print("preview only; run again with --apply to remove them")
        return 0
    if not drop:
        return 0
    bak = f"{path}.{time.strftime('%Y%m%dT%H%M%S')}.bak"
    shutil.copy2(path, bak)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.writelines(keep)
    os.replace(tmp, path)
    print(f"removed {len(drop)} rows; backup at {bak}")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
