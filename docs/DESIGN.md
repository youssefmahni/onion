# Onion Engine — Decentralized Multi-Device Architecture

Oct 9, 2026 · @y.mahni

## Overview

Every device, whether a Mac, a Linux box or a Windows PC, runs one C++ binary. Each of its workers takes one ticket from its own lane, asks DeepSeek for that node's direct children, checks the result against a quality gate, and commits it to the device's signed, append-only log. Devices sync those logs with each other, with no coordinator.

**Goals**

- Any number of devices on macOS, Linux and Windows 10 or 11, all on the home LAN for now; three Mac Pros today, more later.
- No master: every device is both client and server, and any member can admit a new one.
- Work spreads one node at a time, so a large branch never ties up a single machine.
- Every node's children pass a quality gate before they are kept, and branches grow without a fixed depth limit while rules guarantee each one ends.
- Joins, leaves and sudden power loss are handled at runtime, with no restart of the other devices.
- Git-like history: every change is signed, hash-chained and revertible by a later record.

**Non-goals**

- Defending against hostile members. Members are trusted; signatures give tamper evidence, not Byzantine consensus.
- Strong consistency. Devices converge within seconds; brief divergence costs duplicate LLM calls, never corrupt data.
- Proof-of-work, mining or any leader election.

**Design principles**

- Single writer per log, so write conflicts cannot exist.
- Derived state is disposable: the SQLite view can always be rebuilt from the logs.
- Deterministic decisions: lanes and merges are computed the same way on every device, so no device has to ask another. Budgets are bounded, not exact: a small, stated overshoot is allowed.
- One portable codebase: OS differences live in a small platform layer, never in the engine.

*(Diagram in the original doc: architecture · one device's stack and its peers.)*

Workers commit signed batches to the device's own log. Sync threads serve that log to peers, pull theirs into verified replicas, fold everything into `onion.db`, and hand every change to one state-writer thread that owns the ticket index and the SQLite store.

**Decisions taken**

| Topic | Decision |
| --- | --- |
| Availability | Devices are usually online; sudden shutdowns must be safe |
| A device goes silent | Its work moves only when a majority of members is still visible; stealing obeys the same rule |
| Network | Home LAN only for now; no hard-coded addresses |
| Platforms | macOS 12 and later, Linux, Windows 10 and 11, same binary |
| Machine settings | A member profile from a built-in hardware scan at join, at every start, after reconnecting and every 48 h; `node.conf` overrides win |
| LLM provider | DeepSeek, configured explicitly per device, text only |
| Image understanding | Dropped: `vision.py` and image input are removed |
| LLM keys | One key per device, never synced |
| LLM budget | Per device; reserved before each task; LLM work stops when it runs out; refilled by hand only |
| Concurrency | Async requests; an adaptive limit on DeepSeek requests in flight, within the profile's ceiling (2,000 on a Mac Pro) |
| Unit of work | One ticket = one open node; a worker generates that node's direct children only |
| Who works on what | Tickets in home lanes (weighted rendezvous hashing); idle devices steal from busy lanes with a leased claim |
| Depth | No fixed cap; four rules end every branch (no repeated ancestor, cannot-split leaf, branch share, optional depth checkpoint) |
| Node ids | 64-bit: 20-bit device slot × 2^40 + counter, about 1.1 trillion ids per device |
| Existing corpus | Imported once into signed logs (genesis import), then cloned by every device |
| Emergencies | An offline admin key, kept off the machines, signs forced retirements and mass revocations only |
| Laptops | Full members with base weight 0.25 (a Mac Pro is 1.0) |
| Quality | A decomposability pre-check, then a gate; up to 3 attempts; the best attempt is kept |
| Telling the fleet what to build | Campaigns: scope, loop budget, gate, stop rule, branch shares, with a cost projection before start |
| `llm` title sweep | Each device checks only the nodes in its home lane |
| Storage | Signed logs are the truth; SQLite is the local store; CSVs are an export only |
| Sync | C++, in the same binary as the funnel |
| Parquet exports | Each device makes its own, on demand |

## Prerequisites

Seven fixes land before any multi-device work, because sync would copy their damage to every device.

| # | Fix | Where | Why it blocks |
| --- | --- | --- | --- |
| P1 | Now: `mkstemps(tmp, 5)`. Then: replace `popen("curl …")` with libcurl in-process, pointed at DeepSeek | `curl_post`, depot_io.cpp | `mkstemp` rejects a suffixed template (EINVAL, confirmed in a C test), so every real LLM call fails. `popen` and `mkstemp` do not exist on Windows. libcurl also gives HTTP status codes and `Retry-After`. |
| P2 | Move load, RAM, network and GPU sampling into the C++ platform layer | controller.py `Sampler` | All `/proc` reads fail on macOS and Windows and fall back to load 0 and RAM 99 GB, so the policy never backs off. |
| P3 | Fail loudly on duplicate paths at load, then migrate the 131 known duplicates | funnel loader | `by_path[n.path] = n` silently keeps the last row; a deterministic merge would turn these into hard conflicts. |
| P4 | Check `finish_reason`; treat `length` as a failed attempt | funnel agents | Truncated JSON is accepted as a smaller child set (ledger C4). |
| P5 | Make the LLM timeout configurable | transport | `--max-time 120` is hard-coded. |
| P6 | Circuit breaker: pause the local funnel after 20 consecutive transport failures | `run_funnel` | An outage or a revoked key otherwise settles every node at score 0, poisoning the ledger. |
| P7 | Commit children only after the quality decision | `work()` | Today children are written to the CSV and queued for expansion before the score is checked, so a failed attempt is kept and expanded, and its retry adds more children on top. |

After P1, run one real `data_depot funnel --limit 1` on a Mac and check `funnel_state.tsv` for failure rows written while the bug was live.

## Migrating the existing corpus

The current tree moves into the new system once, through a genesis import on one device; every other device then clones it instead of importing again.

1. **Freeze and back up.** Stop all runs; copy the level CSVs, `funnel_state.tsv`, `funnel_critique.tsv`, `atomic_items.tsv` and `expand_decisions.csv` off-machine.
2. **Clean.** Resolve the 131 duplicate paths (P3): keep one title per path, record each dropped variant in a report you review.
3. **Import.** One device writes every node, settle, critique and atomic flag as records in its own log, marked `legacy` in their payload. Legacy ids keep their numbers in slot 0.
4. **Verify.** Node count per layer, every parent present, and the Merkle root of the imported view must match a fresh count from the CSVs.
5. **Checkpoint.** The importing device writes a checkpoint; a second device verifies it and co-signs.
6. **Clone.** Every other device joins by cloning that checkpoint. From here on, the CSVs are only an export.

## Cross-platform foundation

One C++17 codebase builds with CMake on macOS, Linux and Windows. Everything the operating systems do differently lives behind one small platform layer, `depot_platform.{hpp,cpp}`; the engine never calls an OS API directly.

**Dependencies** (vendored in `third_party/` like the existing ones, so a Windows build needs no package manager)

| Need | Library | Why this one |
| --- | --- | --- |
| Sync HTTP server and client | cpp-httplib | Single header, portable |
| LLM calls to DeepSeek over HTTPS | libcurl | Replaces `popen` + curl on all three OSes |
| Signatures and hashing | Monocypher, with its optional Ed25519 file | Two C files; BLAKE2b and Ed25519 |
| Local view | SQLite amalgamation | One C file |
| Segment compression | zstd | Fast, portable |
| JSON | nlohmann/json | Already vendored as `json.hpp` |

**Platform layer**

| Concern | macOS | Linux | Windows |
| --- | --- | --- | --- |
| Supported versions | 12 (Monterey) and later | Current LTS distributions (e.g. Ubuntu 22.04 and later) | Windows 10 and Windows 11, 64-bit |
| Compiler | clang (Xcode command-line tools) | gcc or clang | MSVC 2022 or clang-cl |
| Durable write | `fcntl(F_FULLFSYNC)`; plain `fsync` does not flush the disk cache | `fsync` / `fdatasync` | `FlushFileBuffers` |
| Atomic file replace | `rename` | `rename` | `MoveFileExW` with replace + write-through |
| Run as a service | launchd | systemd | Windows Service |
| Load, RAM, network sampling | `getloadavg`, `host_statistics64`, `getifaddrs` | `/proc` | `GetSystemTimes`, `GlobalMemoryStatusEx`, `GetIfTable2` |
| Private key storage | Keychain, or a 0600 file | 0600 file | DPAPI-encrypted file |
| Data directory | `~/Library/Application Support/Onion` | `$XDG_DATA_HOME/onion` or `/var/lib/onion` | `%LOCALAPPDATA%\Onion` |
| Firewall | Allow prompt on first listen | ufw or firewalld rule | Defender Firewall rule at install, Private network profile only |
| GPU (optional) | Apple OpenCL 1.2 | OpenCL ICD | OpenCL ICD from the GPU vendor |

**Rules that keep every OS byte-identical**

- Logs and CSVs are opened in binary mode: LF line endings, UTF-8, never CRLF. Hashes and signatures cover exact bytes.
- Numbers are written with `std::to_chars` (locale-independent, shortest round-trip), so a score folds to the same value everywhere.
- File paths use `std::filesystem`; tree paths (`P01-IN01-…`) are data and never become file names.
- The bash scripts `onion-start` and `onion-stop` become subcommands, `data_depot node start | stop | status`, identical on all OSes.
- Scope is the home LAN for now. Devices on other networks can join later through an overlay such as WireGuard, which runs on all three OSes; nothing in the design depends on the LAN except discovery.

## Device identity and ID slots

Each device names itself and allocates node IDs from its own slot, so nobody hands out names or numbers, however many devices join.

**Identity.** On first start a device generates an Ed25519 keypair and stores the private key with the OS's key storage (platform layer). Its `device_id` is the first 16 hex characters of BLAKE2b-256 of the public key.

**Capabilities.** Each device's hardware and derived settings live in its member profile (see Member profiles); fast-changing values such as load, budget left and current speed travel in gossip.

**ID slots.** TK ids move from 32-bit to 64-bit:

```
tk_id = slot × 2^40 + counter
```

- `slot` = the top 20 bits of BLAKE2b-256(public key): about 1M slots. Each slot holds about 1.1 trillion ids, so no device can run out; a 24-bit counter would have run out within days at full concurrency.
- The largest id stays below 2^60, inside a signed 64-bit integer.
- Slot 0 is the legacy namespace. All existing ids are far below 2^40, so they keep their meaning.
- With 50 devices the chance of any slot collision is about 0.1%. Admission checks it, and a collision is fixed by generating a new key.
- At startup, `tk_next` = the highest id inside the device's own slot + 1, never the global maximum.

**Code changes**

- Ids are `int64_t` everywhere; the `-TK` scan uses `std::atoll`.
- The id format is `TK%05lld`. Legacy ids print unchanged; new ids print up to 19 digits.
- Ingest uses the same slot rule, which also removes the cross-level collision risk in ledger O7.

## Member profiles

Every device scans its own hardware and writes a signed member profile, which sets its starting values and hard ceilings; live values then adapt inside those bounds.

**The scan.** Built into the binary as `data_depot node scan`, using the platform layer, so the same code runs on macOS, Linux and Windows; there is no separate script. It spends no LLM tokens.

| Group | Collected | Used for |
| --- | --- | --- |
| System | OS and version, CPU model, physical and logical cores, total RAM | Worker pool size, request ceiling |
| Storage | Free disk in the data directory, durable-write latency (short timed test) | Full-replica check, segment size |
| GPU | OpenCL devices with fp64, a short run of the existing similarity benchmark | GPU dedup on or off |
| Power | Battery present (laptop or desktop) | Base weight, on-battery cap |
| Network | Link speed, round-trip time to DeepSeek through the free `/models` request | Starting request limit, timeout |
| Software | Binary version, record format version | Upgrade compatibility |

The profile carries nothing that identifies the machine beyond what the fleet needs: no serial numbers, user names or MAC addresses.

**Derived settings.** Fixed rules turn the scan into settings. Any value in `node.conf` overrides the derived one, and the profile records both. Starting values, to be tuned with real measurements:

| Setting | Mac Pro (12 threads, 64 GB, desktop) | Windows laptop (8 threads, 16 GB, battery) |
| --- | --- | --- |
| Base weight | 1.0 | 0.25 |
| Starting request limit | 16 | 4 |
| Request ceiling | 2,000 | 32 |
| CPU pool | 12 threads | 8 threads |
| On-battery cap | none | 8 requests |
| RAM floor (halve the limit below it) | 2 GB | 1 GB |
| GPU dedup | On (FirePro D500s) | Only with an OpenCL fp64 GPU |
| Serves checkpoints to joiners | Yes | Only with enough free disk |
| Full replica | Yes | Yes, with a warning if disk is tight |

**Where it lives.** Hardware facts and derived settings go into a `MEMBER_PROFILE` record in the device's own log, signed by its key, so only the device itself can write it and every member has a copy. Fast-changing values (current request limit, speed factor, budget left, on battery or not) travel in gossip instead, so the log does not fill with noise.

**When it refreshes**

- At join: the scan runs before admission, and the first profile travels with `MEMBER_ADD`, so every member knows the newcomer's capacity from the start.
- At every start of the node, to catch hardware or OS changes such as a RAM upgrade or a Windows update.
- After reconnecting from a long absence.
- Every 48 hours while running.
- A new record is written only when something changed, so a stable machine adds almost nothing to the log.

**Versions.** Every profile is versioned, so you can see what changed on a machine, when and why.

| Field | Meaning |
| --- | --- |
| `version` | 1, 2, 3 … per device, never reused |
| `prev` | Hash of the previous profile record, so the versions form a chain |
| `reason` | join, start, reconnect, scheduled (48 h), override changed, rules updated |
| `changed` | The fields that differ from the previous version, with old and new values |
| `rules_version` | Version of the derivation rules, so a settings change caused by new rules is told apart from a hardware change |
| `schema` | Profile format version, so older and newer binaries can read each other's profiles |

- `data_depot node profile show [device]`, `history [device]` and `diff <device> <v1> <v2>` list and compare versions from any machine, since every member holds every profile.
- The dashboard's fleet view shows each machine's current version and its history with the changed fields.
- Going back to earlier settings creates a new version (for example, an override removed); history is never rewritten.
- Work scheduling and concurrency always use the latest version; an older version is never applied.

**A wrong profile** (a bug or a bad override) only changes that device's own share. The speed factor and the quality guard correct it within minutes.

## Storage

Log segment files are the source of truth and the unit of sync; SQLite is the local store every component reads; CSVs and Parquet are exports only. All of it lives in the OS data directory from the platform layer.

| Tier | What | Written by | Survives sudden shutdown by |
| --- | --- | --- | --- |
| Truth | Per-origin log segments: `oplog/<device_id>/<first_seq>.log` | The owning device for its own log; sync for copies of others | One durable write per commit batch; a torn tail is cut on restart |
| Store | `onion.db` (SQLite, WAL mode), read by the funnel, scheduler, API and dashboards | The state writer thread | Rebuilt from the logs if damaged |
| Export | `Onion - level_NN.csv` (up to L9) and `onion.parquet` | Each device, on demand | Regenerated on demand |

**Segments**

- One JSON line per commit batch, LF only (format in the next section).
- One durable write per batch: `F_FULLFSYNC` on macOS, `fsync` on Linux, `FlushFileBuffers` on Windows.
- A segment is sealed at 64 MB, compressed with zstd and never modified again. Sealed segments transfer whole; the active one transfers as a byte range.
- **Compaction.** `ATTEMPT` and `CRITIQUE` records older than 30 days are summarized per node inside checkpoints. Segments older than the oldest kept checkpoint may be archived off-machine, so a laptop can keep only recent segments plus a checkpoint.
- **Growth estimate.** Every campaign shows its expected log growth, measured in bytes per node, next to its cost projection.

**SQLite schema (view)**

```sql
CREATE TABLE nodes (
  id        INTEGER PRIMARY KEY,   -- local surrogate key, may differ per device
  code      TEXT NOT NULL,         -- visible id: P01, IN01, T01, ST01, TD1362, TK...
  path      TEXT NOT NULL UNIQUE,
  parent_id INTEGER,
  layer     INTEGER NOT NULL,      -- 0 and deeper, no cap
  title     TEXT NOT NULL,
  origin    TEXT NOT NULL,         -- device_id
  hlc       INTEGER NOT NULL,      -- hybrid logical clock of the winning write
  tombstone INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX nodes_parent ON nodes(parent_id);
CREATE TABLE settles   (path TEXT PRIMARY KEY, kept INT, atomic INT, attempts INT, score TEXT,
                        campaign TEXT, hlc INT, origin TEXT);
CREATE TABLE attempts  (path TEXT, n INT, score TEXT, passed INT, tokens_in INT, tokens_cached INT,
                        tokens_out INT, cost TEXT, campaign TEXT, hlc INT, origin TEXT);
CREATE TABLE critiques (path TEXT, score TEXT, issues TEXT, remove TEXT, add_ TEXT, hlc INT, origin TEXT);
CREATE TABLE reviews   (path TEXT PRIMARY KEY, reason TEXT, state TEXT, note TEXT, hlc INT, origin TEXT);
CREATE TABLE campaigns (id TEXT PRIMARY KEY, spec TEXT, state TEXT, hlc INT, origin TEXT);
CREATE TABLE members   (device_id TEXT PRIMARY KEY, pubkey BLOB, slot INT, state TEXT, incarnation INT,
                        profile_version INT, evict_cutoff INT);
CREATE TABLE profiles  (device_id TEXT, version INT, prev TEXT, reason TEXT, changed TEXT,
                        spec TEXT, rules_version INT, schema INT, hlc INT,
                        PRIMARY KEY (device_id, version));      -- every version kept
CREATE TABLE applied   (origin TEXT PRIMARY KEY, seq INTEGER NOT NULL);  -- the version vector
```

Scores and weights are stored as the exact decimal text from the op, so every OS folds them identically.

**Subtree query.** A range scan uses the path index, and `T01` never matches `T010`:

```sql
SELECT * FROM nodes WHERE path = :p OR (path >= :p || '-' AND path < :p || '.');
```

**Checkpoints.** `VACUUM INTO 'checkpoint-<hlc>.db'` produces a compact snapshot. Its manifest holds the version vector, a Merkle root over `nodes` sorted by path, and two signatures: the device that made it, and a second device that rebuilt the same root independently.

## Op log format

Every state change is a record. All records of one commit go into one signed, hash-chained batch, written as one JSON line in its origin's log; the store is a deterministic fold of all logs. Signing once per batch instead of once per record keeps the logs small and fast.

```json
{"v":1,"o":"9f2c41ab07de5531","b":312,"prev":"<blake2b of batch 311>","hash":"<blake2b of recs>","sig":"<ed25519 over prev and hash>","recs":[{"s":4121,"h":1760010000000001,"t":"ADD_NODE","p":{"path":"P01-IN01-T03-ST47-TD1362-TK164926744166401","layer":6,"title":"Order Capture"}},{"s":4122,"h":1760010000000002,"t":"SETTLE","p":{"path":"P01-IN01-T03-ST47-TD1362","kept":1,"end":"children"}}]}
```

| Field | Meaning |
| --- | --- |
| `v` | Format version, so devices on different releases can tell what they read |
| `o` | Origin `device_id` |
| `b` | Per-origin batch number, contiguous from 1 |
| `prev` | BLAKE2b-256 of the previous batch; batch 1 uses the device's public-key hash |
| `hash` | BLAKE2b-256 of the canonical bytes of `recs` (RFC 8785 JSON canonicalization) |
| `sig` | Ed25519 signature over `prev` and `hash` |
| `recs[].s` | Per-origin record sequence number, contiguous from 1 across batches |
| `recs[].h` | Hybrid logical clock: 48-bit physical ms + 16-bit logical counter |
| `recs[].t` | Op type (below) |
| `recs[].p` | Payload |

| Op | Payload | Emitted by |
| --- | --- | --- |
| `CAMPAIGN` | id, spec (scope, loop budget, mode, gate, stop rule, branch share, depth checkpoint, light funnel, critic model, auto-accept, structure, spillover, model), state | You, from any device |
| `RESERVE` | campaign, attempts reserved | A device taking its next chunk of a campaign's loop budget |
| `CLAIM` | path, lease length, renewal count | A device stealing a ticket from another lane |
| `REEXPAND` | path, reason | A review retry or forced re-run of a settled node |
| `ATTEMPT` | path, attempt number, score, checks passed, input / cached / output tokens, cost, campaign | Every attempt, kept or discarded |
| `ADD_NODE` | path, layer, title, legacy? | Commit of the accepted attempt, ingest, genesis import |
| `CRITIQUE` | path, score, issues, remove[], add[] | Commit of the accepted attempt |
| `ATOMIC` | path, verified | Commit of the accepted attempt |
| `SETTLE` | path, kept, atomic, attempts, score, end (children, cannot split, allowance spent), campaign | Commit, always last in its batch |
| `NEEDS_REVIEW` | path, reason (below floor, structure work, depth checkpoint) | A worker or the scheduler, instead of `SETTLE` |
| `REVIEW` | path, decision (accept, retry, reject), note | You, from any device |
| `RETITLE` | path, title | `llm` sweep (lane owner), ingest merges |
| `TOMBSTONE` | path, reason, merged_into? | Dedup merges, rejected reviews, reverts |
| `MEMBER_ADD` | device_id, pubkey, slot, invite, first profile | Admitting member |
| `MEMBER_PROFILE` | version, prev, reason, changed fields, scan results, derived settings, overrides, rules and schema versions | The device itself, only when something changed |
| `MEMBER_LEAVE` | device_id | The leaving device |
| `MEMBER_EVICT` | device_id, accept_until_seq, majority signatures or the admin key's signature | Any member; also retires a device that never returns |
| `CHECKPOINT` | version vector, Merkle root, snapshot hash, two device signatures | Any device, on a schedule |

**Verification on receipt.** A batch is applied only if its signature verifies, `prev` matches the stored previous batch, `hash` matches its records, record numbers continue without a gap, and the origin is a member whose eviction cutoff (if any) is above them. A batch that fails is quarantined, never applied, and reported in the node's status.

## Membership

Devices track each other with SWIM gossip (the protocol behind HashiCorp memberlist, Consul and Serf), so failure detection needs no central monitor and works the same on every OS.

**Failure detection**

1. Every protocol period (1 s) a device pings one random member over UDP.
2. No ack within 500 ms: it asks 2 other members to ping that device indirectly.
3. Still no ack: the device is marked SUSPECT and the suspicion is gossiped.
4. A suspected device refutes by gossiping ALIVE with its incarnation number + 1.
5. No refutation within the suspicion timeout: DEAD. The timeout is 5 min, longer than an LLM call plus a reboot, so a device that crashes and restarts quickly never loses its work.

**Dissemination.** Membership updates (state, incarnation, capabilities, sync vector) piggyback on ping and ack messages, so there is no separate broadcast channel.

**Discovery (no fixed addresses)**

- Every device sends a signed UDP multicast beacon on the home LAN every 5 s, implemented in the platform layer, so Macs, Linux and Windows find each other without Bonjour, Avahi or any address list.
- Some Wi-Fi routers isolate clients or block multicast. Every invitation therefore also carries the inviter's current addresses as seeds, so joining never depends on the beacon.
- Every device remembers the addresses it has seen, so after a reboot or a DHCP change it reconnects on its own.
- Later, for devices outside the LAN: an overlay such as WireGuard plus one seed address.

| State | Owns work? | Meaning |
| --- | --- | --- |
| INVITED | No | Holds a valid invitation, not yet admitted |
| JOINING | No | Admitted; cloning and catching up |
| ACTIVE | Yes | Synced and counted in ownership |
| SUSPECT | Yes, until DEAD | Missed probes; may still be alive |
| LEAVING | No new work | Draining in-flight nodes |
| LEFT / DEAD | No | Gone; its log stays in every replica |
| EVICTED | No | Key revoked or retired; terminal |

*(Diagram in the original doc: membership lifecycle · 8 states.)*

Only ACTIVE (and SUSPECT, until it becomes DEAD) counts in ownership; LEFT and DEAD devices come back through JOINING, while EVICTED is terminal.

## Joining, leaving and sudden shutdowns

A device can be switched off at any instant: it loses at most the node it was working on, and nothing already committed, because the log is written before anything else and torn tails are cut on restart.

**Join**

1. An active member runs `data_depot node invite --pubkey <key> --ttl 24h`. It prints a signed token: public key, expiry, inviter, nonce.
2. The new device (any OS) starts with the token and finds a peer over the LAN beacon or a seed address.
3. The inviter checks the token and that the new slot collides with no member, then appends `MEMBER_ADD`.
4. The device clones the newest checkpoint, verifies its Merkle root and signature, then pulls each origin's log tail.
5. When its lag behind every origin is under 100 ops, it gossips ACTIVE and ownership includes it.

**Commit order (what makes power loss safe)**

1. While a node is being worked on, attempts live in memory only; each one adds just an `ATTEMPT` record to the next batch.
2. When the gate decides, the accepted attempt's records (`ADD_NODE` children, `CRITIQUE`, `ATOMIC`, then `SETTLE` last) form one batch, or the batch holds a single `NEEDS_REVIEW`.
3. One durable write for the batch (platform layer).
4. Only then is the SQLite store updated, and new children are pushed to their lane owners.

**Restart after a sudden shutdown**

1. Scan the tail of the active segment; drop a trailing batch that has no final LF or fails its hash or signature.
2. A node whose batch was cut never got its `SETTLE`, so it simply runs again; children that landed in an earlier commit are filtered by the canonical-title dedup.
3. Replay batches past the `applied` vector into `onion.db`, so the store agrees with the log.
4. Rejoin: back within 5 min → refute the suspicion with incarnation + 1 and keep its lane. Away longer → JOINING, catch up, then take its fresh share.

The OS service manager restarts the node automatically: launchd `KeepAlive`, systemd `Restart=always`, Windows Service recovery set to restart (which also covers forced Windows Update reboots).

**Graceful leave.** `data_depot node stop --leave` sets LEAVING: no new nodes, in-flight ones finish, the log tail is pushed to at least one peer, then `MEMBER_LEAVE`.

**Eviction and retirement.** For a lost, stolen or permanently dead device, any member drafts `MEMBER_EVICT` with `accept_until_seq` = its last trusted record. It is valid once signed by a majority of members.

- **Automatic proposal.** A member absent for 30 days is proposed for retirement automatically; it takes effect once a majority signs. This keeps vanished laptops from making a majority impossible over time.
- **Admin key for emergencies.** If too few members remain to form a majority, an eviction signed by the offline admin key is accepted instead. That key is generated once, kept off every machine (for example in a password manager or on a USB drive), and every device learns its public key at genesis. No single device can evict another on its own.

## Campaigns

You tell the whole fleet what to build with a signed `CAMPAIGN` record, created on any device; every device works only inside running campaigns. Campaigns replace the per-machine `--limit`, `--rounds`, `--deep` and `--force` flags.

| Field | Meaning | Example |
| --- | --- | --- |
| Scope | One or more path prefixes | `P06` |
| Loop budget | Attempts allowed across the whole fleet | 10,000 |
| Mode | Breadth-first (root down) or depth-first (finish branches) | depth-first |
| Gate | Minimum score, maximum attempts, floor | 0.7, 3, 0.5 |
| Stop rule | When a branch counts as done | last 50 settled nodes ≥ 90% atomic and average score ≥ 0.8 |
| Starting pools | How the loop budget is split across scope roots | in proportion to each root's open tickets |
| Branch share | The most a child branch may spend of its parent's remaining pool | 50% |
| Depth checkpoint | Review pause every N levels past L9 (optional) | 5 |
| Light funnel | Depth from which a lighter funnel runs (2 to 3 aspects, one trim) | L8 |
| Critic model | Model for the critique agent | same as Model |
| Auto-accept reviews | Accept review items left untouched for N days (optional) | off |
| Structure | Whether nodes at L1 to L3 may be expanded (always reviewed) | no |
| Spillover | Whether devices with budget take over tickets of devices without | allowed |
| Model | DeepSeek model id | `deepseek-flash` |

- **Commands.** `data_depot campaign start <file>`, `pause <id>`, `resume <id>`, `stop <id>`. Each writes a `CAMPAIGN` record; for one id the latest `(hlc, origin)` wins, and `stop` is final.
- **Cost projection first.** Before the `CAMPAIGN` record is written, `start` shows the projected cost (loop budget × measured average cost per attempt over the last 7 days, or from a 20-attempt calibration run for a new model) and the expected log growth, and asks you to confirm.
- **Loop budget in reserved chunks.** Devices take the budget in chunks through `RESERVE` records (default 50 attempts, or about one minute of that device's throughput). Reservations count in `(hlc, origin)` order; one that would pass the budget is void, and its device stops starting attempts for that campaign. Overshoot is at most one chunk per device.
- **Several campaigns** can run at once; a node belongs to the campaign whose scope prefix matches it most specifically.

## Work scheduling

Work is split into tickets: every open node in a running campaign is a ticket. Each ticket has a home lane, the device with the highest weighted rendezvous score for its path. A device works its own lane first and, when the lane runs dry, steals tickets from busy lanes with a short claim.

```
score(n, d) = −w_d / ln(h(n, d))
h = (x + 0.5) / 2^64
x = first 64 bits of BLAKE2b(node_path ‖ device_id)
```

The `+ 0.5` keeps `h` strictly between 0 and 1, so the logarithm is always defined.

**Tickets.** A node is a ticket when it is inside a running campaign's scope, not settled, not a leaf, not waiting for review, has allowance left (see Depth without a cap), and is not under a re-expansion lock. Open tickets live in an index (SQLite plus an in-memory queue), so no worker ever scans the tree to find work. Each worker runs one ticket at a time; a worker is a task slot, not an operating-system thread (see Concurrency), and a Mac Pro runs hundreds of them.

**Order.** Breadth-first campaigns take the shallowest tickets first, from the root down; depth-first campaigns take the deepest first, to finish branches. Ties go by path.

**Home lanes, the normal case.** Every device computes the same lanes from the same member list and weights, so taking a ticket from your own lane needs no message and cannot conflict.

**Weight.** `w = base weight × speed factor`.

- Base weight comes from the member profile: 1.0 for a desktop such as a Mac Pro, 0.25 for a laptop; node.conf can override it.
- Speed factor = the device's settled home-lane nodes per hour divided by the fleet median, smoothed over 10 minutes and clamped to 0.5 to 2. Stolen tickets do not count, so stealing and weighting cannot feed each other into swings.
- `w` is 0 while the device is paused, has no budget left, or is flagged by the quality guard.
- Because tickets are small, a weight change moves only a few of them.

**Stealing, when a lane runs dry**

1. A device whose lane is empty, or down to fewer tickets than its workers, looks at the busiest lanes; lane sizes travel in gossip.
2. It never steals from the lane of a DEAD device unless it can see a majority of members, and never from a budget-empty device's lane when that campaign disallows spillover. Both rules keep stealing from bypassing your decisions.
3. It picks from the far end of a busy lane, the tickets that lane's owner would reach last, choosing at random among the top 5 candidates so that stealers do not collide with each other.
4. It writes a `CLAIM` record with a 10-minute lease, renewed every 3 minutes while it works, and pushes it to every device at once.
5. A lane owner checks claims before starting a ticket and skips claimed ones; every device skips tickets claimed by another.
6. Each device times a lease from the moment it received the claim, never from the claimer's clock, so clock differences cannot shorten or stretch it. A lease that is not renewed expires and the ticket returns to its lane; committing the ticket ends the claim.

**Re-expansion lock.** Re-running a settled node (a review retry or a forced re-run) writes a `REEXPAND` lock, and tickets in its subtree pause until it settles. Normal expansion needs no lock, because a node's children do not exist until it is done.

**Eligibility.** ACTIVE and SUSPECT devices have home lanes. A DEAD device keeps its lane, so its tickets wait, until this device can see a majority of members; then its tickets re-hash to the survivors. Its claims simply expire with their leases. A 2-and-2 split moves nothing.

**New children.** After a commit, the device pushes each new non-atomic child straight to its lane owner, so the next level starts at once instead of waiting for the 15 s sync round.

**Structure work.** Expanding an L1, L2 or L3 node creates tasks, sub-tasks or TDs. It runs only when the campaign allows it, and its result always goes to review before its children become tickets. L0 phases are never generated.

**Collisions.** If two devices still take the same ticket in the same moment, both commits merge: children are deduped by canonical title and the higher-scoring `SETTLE` wins. The cost is one duplicated node, never corrupt data.

**Branch reports.** Atomic %, average score, attempts, spend, depth and open review items per branch are computed from the shared logs, so any device's dashboard shows any branch.

## Quality gate and retries

A node's children are kept only after they pass a gate. A worker tries up to 3 times, keeps the best attempt, and sends the node to review if even the best is poor.

1. **Pre-check.** A short call (at most 400 tokens) asks whether the node splits into at least 3 distinct parts. A clear no makes it a leaf at once (end = cannot split), without running the funnel. This keeps leaves cheap; without it, every leaf would cost 3 full attempts.
2. **Attempt.** Run the funnel for the node: planner, workers, dedup, trims, critique, synthesizer. From the campaign's light-funnel depth onward, a lighter funnel runs (2 to 3 aspects, one trim). Nothing is committed yet; only an `ATTEMPT` record is written.
3. **Checks that do not depend on the model's judgement.** The reply is complete (`finish_reason` = `stop`) and valid JSON; 3 to 100 children; no child repeats the node, an ancestor, a sibling or an existing child (canonical title); every title passes the style gates.
4. **Score.** The critique score of the attempt, from the campaign's critic model.
5. **Decide.**
   - Checks pass and score ≥ 0.7: commit this attempt.
   - Otherwise, if fewer than 3 attempts: retry with the previous critique issues injected and the temperature raised (0, then 0.3, then 0.5), so the new attempt actually differs.
   - After 3 attempts, if no attempt produced at least 3 distinct valid children: the node becomes a leaf (end = cannot split).
   - Otherwise: commit the best attempt that passed the checks if its score ≥ 0.5; else write `NEEDS_REVIEW` and spend nothing more on this node.

*(Diagram in the original doc: quality gate · one node, up to 3 attempts.)*

A failed attempt loops back with the critique's issues and a higher temperature; after the third, the best attempt is kept only if it clears the 0.5 floor.

**Atomic is never a goal.** The model that writes children also labels them atomic, so rewarding atomic labels would teach it to stop decomposing. Atomic % is used only for reports and the stop rule. A child labelled atomic is treated as a leaf only after a short separate check (the `llm` sweep's `admits_children` verdict) agrees; otherwise it stays a task.

**Quality guard.** Each device's rolling average score and failure rate are compared with the fleet median. A clear outlier gets weight 0 and an alert on the dashboard; this catches a wrong model id, an exhausted key or a bad config within minutes.

**Review queue.** `NEEDS_REVIEW` nodes, structure results and depth checkpoints appear on the dashboard. You accept (the node settles and its children become tickets), retry with a note (injected into the next attempt), or reject (tombstoned). Each decision is a `REVIEW` record.

- **Batch actions:** accept or reject everything in a branch at once.
- **Priority:** items are sorted by how much their branch has already spent, so expensive branches are decided first.
- **Auto-accept (optional, off by default):** a campaign can accept review items left untouched for N days, so one person is never the bottleneck.

## Depth without a cap

The tree has no fixed depth limit. Four rules end every branch, so it always stops growing without the old L9 cap, and the campaign's loop budget stays the outer bound on spending.

| Rule | What it does | Why it guarantees an end |
| --- | --- | --- |
| No repeating an ancestor | A child is rejected if its canonical title matches its parent at similarity ≥ 0.85, or any higher ancestor at ≥ 0.95. The stricter threshold higher up avoids rejecting legitimately narrower children that share words | Cycles become impossible |
| Cannot split, then leaf | The pre-check says no, or no attempt yields at least 3 distinct valid children: the node becomes a leaf (`SETTLE` with end = cannot split) | Decomposition stops where the ideas run out |
| Branch share | Each scope root has a pool of attempts. A child branch may spend at most 50% of what its parent's pool has left when it starts; what leaves and short branches do not use stays in the pool for their siblings. A node whose share cannot pay one attempt becomes a leaf (end = allowance spent) | A single chain can only halve its pool per level, so it ends; broad trees share the pool instead of dividing it |
| Depth checkpoint (optional) | A ticket at every Nth level past L9 (L14, L19, … with N = 5) is not started; the scheduler writes `NEEDS_REVIEW` (reason depth checkpoint) without spending an attempt, and accepting it releases that subtree until the next checkpoint | You decide on very deep branches |

- **Why a share, not an even split.** Dividing a pool evenly among 20 to 100 children shrinks it by that factor at each level, which would end most branches about 3 levels down, shallower than the old L9 cap. A 50% share lets a 10,000-attempt pool support a single chain about 13 levels deep while still guaranteeing an end.
- **Starting pools.** A campaign splits its loop budget across its scope roots in proportion to each root's open tickets, so roots that already exist in the corpus get a pool too.
- **Leaves are final but reversible.** A leaf can be re-expanded later by a review retry, which takes a re-expansion lock and a fresh share.

**What changes in storage and the UI**

- SQLite is the store from the start of the rebuild; the 10 fixed level CSV files cannot hold unlimited depth, so they are an export only.
- The loader's fixed list of 10 level files and the `path_segments >= 10` guard are removed.
- Dashboards draw as many rings or levels as the tree actually has.
- Paths grow longer: with 64-bit ids, a 20-level path can pass 350 characters. SQLite handles that; its path index grows accordingly.

## LLM provider, keys and budgets

All LLM calls go to DeepSeek. Each device uses its own key and its own budget, and stops LLM work when its budget runs out.

**Provider config** (`node.conf`, set explicitly instead of guessing the provider from the key prefix):

```
llm.provider    = deepseek
llm.base_url    = https://api.deepseek.com/chat/completions
llm.model       = deepseek-flash
llm.max_tokens  = 6000
llm.timeout_s   = 120
llm.price_in    = <USD per 1M input tokens, cache miss>
llm.price_cache = <USD per 1M input tokens, cache hit>
llm.price_out   = <USD per 1M output tokens>
llm.budget_usd  = <budget>
base_weight     = 1.0
```

- **Reasoning knob.** Every DeepSeek request sends `reasoning_effort: "none"`; otherwise hidden reasoning can use the whole `max_tokens` and the reply comes back empty. A reply cut at `max_tokens` (`finish_reason` = `length`) is a failed attempt.
- **Text only.** No image input anywhere; the old `vision.py` path is removed.
- **Model ids and prices.** Read them from DeepSeek's `/models` endpoint and pricing page when setting up; nothing is hard-coded. A campaign's model and critic model must exist in every device's config.
- **Keys.** One DeepSeek key per device, held in the OS key store. It never enters the log, gossip or sync traffic, so a leak or revocation affects one device only.
- **Metering.** After each call the device reads the token counts in the response's usage block (cache-hit input, cache-miss input, output) and multiplies them by the configured prices. Spend and remaining budget are gossiped, so any dashboard shows the whole fleet.
- **Prompt caching.** Fixed instructions come first in every prompt and node-specific text last, so DeepSeek's cache can bill the shared prefix at the cheaper cache-hit rate.
- **Errors.** HTTP 429 and 503 are retried with backoff, honouring `Retry-After`; the circuit breaker (P6) pauses the device after 20 consecutive failures.
- **Reservation.** Before a ticket starts, the worker reserves that ticket's worst case (the pre-check plus every agent call of 3 attempts at `max_tokens`) from the remaining budget, and releases the unused part when the ticket ends. With hundreds of tickets in flight, reservations decide whether a new ticket may start, so the budget is never overspent.
- **Stop.** When no reservation fits, the device starts no new tickets, finishes those in flight, and its weight drops to 0.
- **While stopped.** The device keeps syncing and serving its log, so it stays a full member. The `llm` title sweep stops too, since it also spends budget.
- **Refill.** Only by hand: `data_depot node budget set <usd>`. Its weight returns and tickets flow back with minimal movement.
- **Spillover.** With spillover allowed (the default), a budget-empty device's tickets re-hash to devices that still have budget, paid from their budgets. With spillover not allowed, those tickets wait until you refill it, and nobody steals them.
- **Whole fleet out of budget.** Every device idles and keeps syncing; the dashboard shows why.

## Concurrency

Each device controls one number, the DeepSeek requests it has in flight, and adjusts it from DeepSeek's behaviour within its profile's ceiling; threads are not the limit.

**Waiting and working are separated**

- **Waiting on DeepSeek:** `depot_llm` uses libcurl's multi interface, which is event-driven. One or two threads keep thousands of requests in flight, with no `curl` process and no thread per call.
- **CPU work:** parsing replies, deduplication and the gate checks run on a pool sized to the CPU (about 12 threads on a Mac Pro).
- A Mac Pro therefore runs roughly 15 to 20 threads in total while keeping 1,000 or more requests going. This replaces `popen` per call and `std::async` per aspect, and with them the unbounded thread growth in ledger O5.

**The request limit (adjusted every 30 s)**

- Any HTTP 429 or 503, or timeouts above 5% of calls: halve the limit, then hold for one window.
- Median latency within 1.5 times its low-load baseline: add 4.
- Latency rising without errors: hold, because more requests would only queue at DeepSeek.
- It starts at the profile's starting value and never exceeds the profile's ceiling.

**Safety caps on top** (from the platform sampler)

- Available RAM below the profile's floor: halve the limit.
- CPU above 90% for several windows: subtract 4.
- On battery: cap at the profile's on-battery value.
- The OS's open file and socket limit.
- Your pin or cap in `node.conf` or through the local API.

**Workers.** A worker is a task slot (a small state machine), not an operating-system thread. The scheduler keeps about limit ÷ 3 workers busy (a ticket has several calls in flight at once), never more than this device's ready tickets.

**How it feeds work scheduling.** A device that can safely run more requests settles more nodes per hour, which raises its speed factor and so its share of tasks. A device that DeepSeek slows down, or that is on battery, ends up with less. Both values are smoothed (30 s for the request limit, 10 minutes for the speed factor), so they settle instead of swinging.

**Network cards.** Bandwidth is not a bottleneck: 1,000 requests in flight move roughly 10 Mbps. A second connection adds no speed, but a second internet provider gives automatic failover when one line drops.

## Sync protocol

Sync is pull-only and incremental: devices compare version vectors, the DNS SOA-serial idea, and fetch only missing per-origin ranges (IXFR); Merkle hashes and checkpoints cover full repair (AXFR).

**Transport.** UDP 7946 for SWIM gossip and the LAN beacon; TCP 7947 for sync over HTTP (cpp-httplib), on the home LAN interface only. Every request is signed with the device key (see Security model).

**Round (every 15 s)**

1. Pick one random ACTIVE peer, preferring a peer whose gossiped vector is ahead of ours for some origin.
2. `GET /sync/v1/vector` and compare with the local `applied` table.
3. For each origin where the peer is ahead: fetch sealed segments whole, then the active range as batches.
4. Verify each batch (signature, `prev`, `hash`, contiguous numbers) and append it, with a durable write, to the local copy of that origin's log.
5. Fold the batches into `onion.db` in one transaction and update `applied`.
6. Hand new nodes, settles, claims and locks to the state writer, which updates the ticket index.

| Endpoint | Returns |
| --- | --- |
| `GET /sync/v1/vector` | `device_id`, incarnation, `{origin: last_seq}`, per-origin segment list (first, last, hash, sealed), newest checkpoint manifest |
| `GET /sync/v1/log/<origin>?from=<seq>&max_bytes=8388608` | NDJSON records from `seq`, capped at 8 MB per response |
| `GET /sync/v1/segment/<origin>/<first_seq>` | A sealed, zstd-compressed segment |
| `GET /sync/v1/checkpoint/latest` | Manifest: version vector, Merkle root, snapshot hash, signature |
| `GET /sync/v1/checkpoint/<hlc>.db` | The snapshot file |
| `GET /sync/v1/merkle/<origin>?depth=<n>` | Merkle hashes over 1,000-record leaves |

**Anti-entropy (every 10 min).** Compare per-origin Merkle roots with one peer. A mismatch at the same `seq` range means local corruption: descend the tree to the bad leaves and re-fetch only those records.

**Checkpoints.** Each device writes one every 6 hours or every 100,000 applied ops, whichever comes first, and keeps the last 3. New and long-absent devices clone from one instead of replaying every log from record 1.

**Push-before-settle (off by default).** A sudden shutdown keeps the disk, so committed records survive and sync after restart. Only a dead disk loses the last 15 s; turn this on if even that is unacceptable, at the cost of one sync round per node.

**New-task notices.** After a commit, the device sends each new non-atomic child's path to that child's owner over TCP 7947, signed like any request. The owner pulls the matching records at once and queues the task. A lost notice is harmless: the next sync round brings the same records. Claims and re-expansion locks are pushed the same way, so other devices skip claimed tickets within moments.

## Conflict and merge rules

Every conflict has one deterministic rule, so all devices fold the same ops into the same state with no negotiation. Ties always break on the highest `(hlc, origin)`.

| Situation | Rule | Notes |
| --- | --- | --- |
| Same path added twice | Impossible by construction | Paths end in a slot-scoped TK id |
| Two children with the same canonical title under one parent | Lowest `(hlc, origin)` survives; the other gets `TOMBSTONE` with `merged_into` | Its children keep their paths and are shown under the survivor; paths never change |
| Two claims on one ticket | Lowest `(hlc, origin)` keeps it; the other device drops the ticket as soon as it hears | At most one duplicated attempt |
| A lane owner and a stealer start the same ticket | Owners skip claimed tickets; if both still started, both commits merge as below | Rare: stealers take from the far end of the lane |
| Budget reservations that pass the campaign's loop budget | Counted in `(hlc, origin)` order; the ones past the budget are void | Overshoot is at most one chunk per device |
| One node settled by two devices | The higher score wins; a tie goes to the higher `(hlc, origin)` | Both settles and both attempt histories stay in the logs |
| `SETTLE` and `NEEDS_REVIEW` for the same node | `SETTLE` wins | Some device found an acceptable attempt |
| `REEXPAND` while subtree tickets are in flight | In-flight tickets finish; no new subtree ticket starts until the re-expansion settles | |
| Two `REVIEW` decisions for one node | Latest `(hlc, origin)` wins | |
| Campaign updates | Latest `(hlc, origin)` per campaign id; `stop` is final | |
| Concurrent `RETITLE` | Last writer wins on `(hlc, origin)` | HLC, not wall time: clocks drift |
| `ADD_NODE` under a tombstoned parent | Kept but hidden; shown again if the parent is restored | Tombstones are never physically deleted |
| `ATTEMPT` and `CRITIQUE` records | All kept until compaction summarizes them | Audit and cost records, not state |
| Membership | Add-wins set; eviction needs a majority-signed record or the admin key | `MEMBER_LEAVE` is valid only from the device itself |

**Revert.** History is never rewritten. Undoing a node, a subtree or a whole bad run is a new batch of `TOMBSTONE` ops, so the revert itself is signed and auditable.

## Modules and what happens to today's code

Everything runs in one process, `data_depot node run`: workers, sync, gossip and the local API. One state-writer thread owns all mutable state (the ticket index and SQLite writes); every other part hands it changes through a queue, which replaces today's single global mutex.

**Modules**

| Module | Responsibility |
| --- | --- |
| `depot_core` | Pure algorithms kept from today: canonical titles, similarity, parsers, style gates |
| `depot_gpu` | Optional OpenCL similarity, kept from today |
| `depot_platform` | Every OS call: durable write, atomic rename, sampling, key storage, data directory, service hooks |
| `depot_profile` | Hardware scan, derived settings, overrides, `MEMBER_PROFILE` records |
| `depot_llm` | Async DeepSeek client on libcurl multi: adaptive request limit, metering, reservations, backoff, circuit breaker |
| `depot_gate` | Pre-check, attempt loop, deterministic checks, best-of-3, leaf rules, `NEEDS_REVIEW`, atomic verification |
| `depot_sched` | Campaigns, home lanes, stealing, claims and leases, re-expansion locks, branch shares, loop-budget chunks, CPU pool |
| `depot_state` | The state writer: ticket index and every SQLite write, fed through one queue |
| `depot_log` | Records, batches, canonical bytes, signing, admin-key checks, segments, torn-tail repair |
| `depot_store` | SQLite schema, fold, checkpoints, Merkle roots, compaction, exports |
| `depot_swim` | Membership, failure detection, LAN beacon, gossip of live values |
| `depot_sync` | Version vectors, pulls, new-task notices, HTTP endpoints |

**What happens to today's code**

| Today | Becomes |
| --- | --- |
| `depot_core` (canonical titles, similarity, parsers, style gates) and its tests | Kept as the pure core |
| `depot_gpu` and its tests | Kept, optional |
| `run_funnel` and `work()` in depot_io.cpp | Rewritten as `depot_gate`, `depot_sched` and `depot_state`; attempts commit only after the gate (P7) |
| `curl_post` with `popen` and `mkstemp` | Replaced by `depot_llm` (libcurl multi, DeepSeek) |
| `std::async` per aspect | Removed; calls go to `depot_llm`, CPU work to the pool (ledger O5) |
| Single global mutex | Replaced by the state-writer thread (ledger O4) |
| Level CSVs as storage, the 10-file loader, the L9 guard | SQLite store; CSVs become an export; the guard is removed |
| `funnel_state.tsv` and the audit TSVs | Imported once by the genesis import, then replaced by records |
| `run_ingest`, `run_llm` | Emit records instead of rewriting CSVs (ledger O6); `run_llm` gets the `AskFn` test seam (O3), checks lane nodes only and serves atomic verification |
| main.cpp CLI, `onion-start`, `onion-stop` | `node`, `campaign` and `review` subcommands on every OS |
| controller.py | Retired: sampler, policy and HTTP API move into the node |
| Thread-policy copies (`depot_core_wasm.js`, the dashboard's JS fallback, tui.py) | Removed; the request limit lives in one place |
| `vision.py` | Removed: image understanding is dropped |
| rebuild_json.py, gen_onion_html.py | Replaced by SQLite queries; the review page loads subtrees lazily |
| gen_funnel_report.py, demo_preview.cpp | Removed (they carry stale paths and model ids) |
| dashboard.html, tui.py | Kept; read the new `/api/state` with campaigns, reviews, profiles, lanes and spend |
| onion_gl | Optional, Linux with X11 only; not ported |

## Security model

Data is self-verifying (every record is signed), so the network only has to keep strangers out; trust comes from device keys, never from IP addresses or the OS.

| Threat | Control |
| --- | --- |
| A stranger on the network talks to sync | Sockets bind to the home LAN interface only, and the Windows firewall rule allows the Private network profile only. Every sync request carries an Ed25519 signature over method, path, body hash and a timestamp (60 s window); only member keys are accepted. |
| A tampered or forged batch | Signature, `prev` chain, `hash` and contiguous numbers; failures are quarantined, never applied. |
| A replayed request | Timestamp window + per-key nonce cache for that window. |
| A stolen or lost device | Majority-signed `MEMBER_EVICT`, or the admin key when no majority can form; batches above its cutoff are rejected everywhere. Revoke that device's own DeepSeek key at the provider. |
| One rogue or buggy device evicting others | Impossible: eviction needs a majority of members or the offline admin key, which no machine holds. |
| Invitation leak | Invitations expire (default 24 h), name one public key and are single-use (nonce recorded in `MEMBER_ADD`). |
| Device key theft from disk | macOS Keychain; Linux file with mode 0600 in a 0700 directory; Windows file encrypted with DPAPI for the service account. |
| Admin key theft or loss | Kept offline, never on a fleet machine, used only for emergency records. Losing it removes only the emergency path; the majority path still works. |
| LLM API key exposure | One key per device, held in the OS key store. It never enters the log, gossip or sync traffic, so a leak or revocation affects one device and its budget only. |
| Prompt content leaves the LAN | Every node title and its context is sent to DeepSeek. Keep confidential material out of campaigns, and check DeepSeek's data-retention terms before using a sensitive scope. |
| Prompt injection through titles or ingested text | Style gates (at most 8 words, no question marks or trailing punctuation) limit what a title can carry; ingest strips instruction-like phrases; the checks and the critic reject off-topic children. |
| A bad checkpoint served to a joiner | Checkpoints carry two device signatures, and the joiner checks the Merkle root before trusting one. |

**Gaps in today's code this design must not inherit**

- Never serve the project or data directory with a generic file server such as `python3 -m http.server --bind 0.0.0.0`; it can expose keys and backups.
- The local API (moving from controller.py into the node) gets a per-device token and checks `Origin` and `Content-Type` on every POST, so a web page cannot trigger `/api/stop` or `/api/threads`.
- Service definitions take secrets from the OS key store, never from inline environment lines such as systemd `Environment=`.

## Failure scenarios

Every device keeps working through any single failure; the majority rule limits duplicate work during partitions, and the price is that a minority side waits instead of taking over.

| Scenario | What happens | Cost |
| --- | --- | --- |
| Power cut, back within 5 min | Torn tail cut; in-flight tickets resume at their next attempt number, with the stored critique issues | The interrupted attempts' LLM calls |
| Power cut, away longer | DEAD after 5 min; with a majority visible its lane re-hashes to the others; on return it catches up and gets its lane back | Its tickets wait 5 min |
| A device dies holding claims | Each device expires the leases on its own clock after 10 min and the stolen tickets return to their lanes | Those tickets wait up to 10 min |
| Disk destroyed | Batches newer than the last sync (about 15 s) are lost; those nodes run again on their new owner | Duplicate LLM calls |
| Partition, one side has a majority | The majority side takes over the minority's lanes; the minority keeps its own and cannot steal from the dead side | Duplicate work on those tickets only |
| Partition with no majority (e.g. 2 and 2) | Nobody takes over or steals across the split; every device keeps its own lane | Dead devices' tickets wait until the split heals |
| Most devices gone for good | No majority can form; you sign their retirement with the offline admin key | Manual step |
| Members vanish over months | Each absent member is proposed for retirement after 30 days and retired once a majority signs | None |
| A branch keeps decomposing | The ancestor check, the pre-check, the cannot-split rule and the branch share end it; a depth checkpoint pauses it for review | At most its share of the pool |
| Hundreds of tickets start at once | Each reserves its worst case first, so the device budget is never overspent; campaign chunks bound the loop overshoot | At most one chunk per device |
| Wrong model id, bad key or bad config on one device | The quality guard sees its scores or failures drift from the fleet and sets its weight to 0 | The attempts made before detection |
| A device's budget is spent | It stops LLM work and keeps syncing; its lane re-hashes (spillover) or waits untouched | None |
| A node never reaches the gate | Best attempt ≥ 0.5 is kept; otherwise `NEEDS_REVIEW`, and spending on it stops | At most 3 attempts |
| Review queue grows | Other tickets continue; batch actions and optional auto-accept clear it | Idle subtrees until reviewed |
| Clock jumps (sleep, NTP, Windows time sync) | HLC keeps order monotonic; leases are timed locally | None |
| Corrupt local SQLite | Delete it; rebuild from logs or a checkpoint | Rebuild time |
| Mixed releases during an upgrade | A device that meets a batch version it cannot read stops applying that origin and reports it | Sync pauses for that origin until it upgrades |

## Testing plan

The acceptance test is convergence: after any mix of joins, leaves, power cuts and partitions, on any mix of operating systems, every device's `onion.db` has the same Merkle root once sync settles.

**Build matrix (CI on every commit)**

- macOS, Linux, Windows 10 and Windows 11 builds of the same tree; the existing 92 tests plus the new ones must pass on all three.
- A cross-OS golden test: the same op batch is canonicalized, hashed and signed on each OS, and the bytes must be identical.

**Unit (gtest)**

- Ids: slot × 2^40 allocation, legacy ids in slot 0, the largest id below 2^60.
- Batches: canonical bytes, one signature per batch, `prev` and `hash` chaining, rejection of a gap, a bad signature or an unknown `v`.
- Torn tail: cut a segment at every byte offset of its last batch; restart must keep exactly the complete batches.
- Merge rules: each conflict-table row in one record order and its reverse; both must give the same state.
- Home lanes: same view gives the same lane; adding a device moves about its weight share; the majority rule blocks takeover in a 2-and-2 split; `h` never reaches 0 or 1.
- Stealing: claims are honoured; two simultaneous claims resolve to one owner; leases time out on the receiver's clock; no stealing from a DEAD lane without a majority or from a budget-empty lane without spillover; stolen work does not raise the speed factor.
- Re-expansion lock: no subtree ticket starts while it is held; in-flight tickets finish.
- Quality gate: pre-check leaf; each deterministic check; best-of-3 selection; the 0.5 floor sending a node to review; a truncated reply counted as a failed attempt; nothing committed before the decision (P7 regression).
- Termination: a child similar to an ancestor is rejected at the right threshold; a single chain under a 50% share ends; a fake model that never says atomic still ends.
- Budgets: reservations never let concurrent tickets overspend; campaign chunks keep overshoot within one chunk per device.
- Admin key: an eviction signed by one ordinary device is rejected; one signed by the admin key is accepted.
- Genesis import: counts per layer, parents and Merkle root match the source CSVs.
- Profiles: derived settings for sample machines on each OS; overrides win; new versions chain correctly.
- Concurrency: with a fake DeepSeek, the limit halves on 429 or 503 and on timeouts, grows on healthy latency, holds when latency rises, and never exceeds the profile ceiling; 1,000 requests in flight use no more than about 20 threads.

**Integration (hermetic, one process)**

- Several node instances in temp directories, each with the existing `AskFn` mock, linked by an in-memory transport.
- Fault injection: kill a node at random points in the commit order, add one mid-run, partition and heal, reorder and duplicate deliveries.
- Assert on files, as the current suite does: logs, CSV rows, no duplicate paths, no orphans, identical Merkle roots.

**Live**

- 24-hour soak on the three Mac Pros, then on a mixed group once a Linux and a Windows machine join.
- Pull the power on one device mid-run; check it rejoins within 5 min with no lost committed work.
- Run the real LLM transport with a real key at least once per release; the mock-only suite cannot catch transport bugs like P1.

## Rollout

The design is delivered as a rebuild in ten milestones; the rebuild plan (`docs/REBUILD_PLAN.md`) holds each milestone's scope, exit gate, migration steps, tests and risks.

| Milestone | Delivers |
| --- | --- |
| M0 · Stabilize | P1 to P7 in today's code, full backup |
| M1 · Foundations | Repo, CI on macOS, Linux, Windows 10 and 11, platform layer |
| M2 · DeepSeek client | Async calls, reservations, adaptive limit |
| M3 · One-machine engine | SQLite store, quality gate, tickets, campaigns, review |
| M4 · Signed log | Keys, ids, batches, genesis import, cutover |
| M5 · Profiles and service | Hardware scan, OS services, tuning |
| M6 · Sync | Pulls, checkpoints, home lanes |
| M7 · Dynamic fleet | Gossip, invites, stealing, eviction, admin key |
| M8 · Depth and scale | Branch shares, light funnel, compaction, projections |
| M9 · Mixed fleet | Linux and Windows join, hardening, old code deleted |

## Open questions

The design picks defaults for each of these; confirm or change them.

- [ ] Gate defaults: minimum score 0.7, 3 attempts, floor 0.5?
- [ ] Atomic verification costs one short extra call per child labelled atomic. On by default?
- [ ] Default stop rule: a branch is done when its last 50 settled nodes are at least 90% atomic with an average score of at least 0.8?
- [ ] Who may start and stop campaigns: any device, or only the ones you name?
- [ ] Branch share of 50% per level, or smaller for cheaper, shallower trees?
- [ ] Depth checkpoint every 5 levels past L9 by default, or off unless a campaign turns it on?
- [ ] Claim lease of 10 minutes renewed every 3, and budget chunks of 50 attempts: keep or tune?
