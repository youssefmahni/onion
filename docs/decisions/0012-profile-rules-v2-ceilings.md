# 0012 — Profile rules v2: ceilings from the real measurements

Status: **Proposed** (owner decision; implemented behind the existing
`rules_version` mechanism with the current defaults unchanged).

## Context

Profile rules v1 (`src/profile/profile.cpp`, `rules_v1`) set a desktop ceiling
of 2,000 (start 16) and a laptop ceiling of 32 (start 4). The owner's real
measurements (OPTIMIZATION_GOAL.md §4.2) show DeepSeek's per-account peak is
about 1,000 in flight, that stable operation is 700–800, and that past the
peak throughput falls while p99 nearly triples. A 2,000 ceiling is therefore
never reachable and invites operation in the degenerate region; a laptop
ceiling of 32 predates any measurement.

## Options

1. Keep rules v1 unchanged.
2. Set fixed v2 numbers from the measurements.
3. Fixed numbers for desktops, and a measured ceiling for laptops.

## Recommendation

**Option 3, as rules v2:**

| Setting | v2 desktop | v2 laptop |
| --- | --- | --- |
| Request ceiling | 1,000 | measured once, then fixed: `min(1,000, plateau)`, never below 32 |
| Starting request limit | 400 | 32 |
| Recommended band (documented, advisory) | 700–800 | the measured stable band |
| Everything else (weight, RAM floor, battery cap, CPU pool) | as v1 | as v1 |

- A desktop starts at 400 so the goodput limiter (decision 0011) converges to
  the provider's plateau within about two minutes (O1's three-minute target).
- A laptop's ceiling is set from one measured probe: a single 60-second run
  of tiny prompts with the in-flight count ramping 4 → min(200, 8 × current
  ceiling), recording the smallest in-flight count at which goodput stops
  rising or p95 doubles (the same detector the limiter uses). The measured
  plateau is stored in the profile's spec; it is re-measured when
  `rules_version` changes, on explicit request (`node scan --force-ceiling`),
  and re-derived every 48 h scan if the network scan input changed materially.
- Implementation is behind `rules_version`: `node.conf` key
  `profile.rules_version = 2` selects rules v2; the default remains 1 until
  the owner approves. The profile records the version and the measured inputs,
  so the choice is visible and reversible.

## Consequences

- The desktop ceiling (1,000) and start (400) come from measured data (F1),
  keeping the fleet out of the degenerate region (F2, F5).
- Laptop ceilings stop being a guess; the measurement uses at most
  `min(200, 8 × ceiling)` calls per laptop and runs locally.
- Nothing changes for existing deployments until the owner flips
  `profile.rules_version` to 2; this record is the approval request.
