# Onion Engine — Implementation Brief for the Coding Agent

> **For Youssef, before you hand this over**
> 1. Export the two Claude Docs as Markdown (doc menu › Export › Markdown) and save them in the repo as
>    `docs/DESIGN.md` (Onion Engine — Decentralized Multi-Device Architecture) and
>    `docs/REBUILD_PLAN.md` (Onion Engine — Rebuild Plan).
> 2. Save this file as `docs/agent/IMPLEMENTATION_BRIEF.md` and commit all three.
> 3. Fill in the three values in §0 below.
> 4. Give the agent the kickoff message at the very end of this file.

---

## 0. Values set by the owner

| Setting | Value |
|---|---|
| Real DeepSeek calls allowed per milestone without asking | `0` (change only if you want the agent to spend money unsupervised) |
| Location of the corpus copy for tests | `~/onion-data` (a copy; the live corpus is never touched) |
| DeepSeek model id to use | `<fill in after checking https://api.deepseek.com/models>` |

---

## 1. Your role and mission

You are the implementation engineer for the **Onion Knowledge Engine** rebuild. The engine grows a knowledge tree with an LLM funnel. It will run as **one C++ binary per machine** on a fleet of macOS, Linux and Windows machines on a home LAN. The machines share work with no master and sync through signed, append-only logs.

Your job is to implement and test milestones **M1 to M9** of the rebuild plan, one milestone at a time, and to support M0's last live step. You write code, tests, build files, CI and documentation. You stop at every milestone gate and wait for the owner's approval.

You are working in the owner's own git repository on Kali Linux. Work carefully, verify everything you claim, and report honestly, including failures.

---

## 2. Sources of truth, in order of precedence

1. **The owner's instructions in the current session.**
2. **`docs/DESIGN.md`**: the architecture, every rule and every constant.
3. **`docs/REBUILD_PLAN.md`**: milestones, gates, migration, risks.
4. **This brief**: how to work, plus a digest of the spec (§6) and interface guidance (§8). If this brief and `DESIGN.md` disagree, `DESIGN.md` wins: report the difference.
5. **Your own judgement**, only for things none of the above decide. Record each such decision as a decision record (§4.9).

Never change a constant from the spec (thresholds, timings, sizes, formats) on your own. If you believe one is wrong, implement it as specified, then raise it in your gate report.

---

## 3. Every session starts the same way

1. Read `docs/agent/PROGRESS.md` (create it in your first session from the template in §11).
2. Run `git status` and `git log --oneline -10`. If the tree is dirty with changes you did not make, stop and ask.
3. Build and test what exists:
   ```bash
   cmake -S legacy -B legacy/build && cmake --build legacy/build -j"$(nproc)"
   ctest --test-dir legacy/build 2>&1 | tail -3
   (cd legacy && python3 -m unittest test_controller.py 2>&1 | tail -2)
   # once the new tree exists:
   cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
   ```
4. Write a three-line status in `PROGRESS.md`: date, milestone and branch, test totals.
5. Continue with the next unchecked task of the current milestone.

---

## 4. Ground rules (non-negotiable)

### 4.1 One milestone at a time
- Work on milestone N only. Create the branch `mN-<short-name>` from `main`.
- When the exit gate passes, write the gate report (§11), push the branch, and **stop**. Do not start the next milestone until the owner replies `APPROVED MN`.
- After approval, merge into `main` with a merge commit and tag it `mN`.

### 4.2 Commits
- Small, focused commits: one module or one rule with its tests. Message format: `mN: <what changed>`.
- Never force-push `main`, never rewrite pushed history, and never commit a build directory, data or secrets.
- Before every commit, run the secret check:
  ```bash
  git diff --cached | grep -nEi 'sk-[a-z0-9]{16,}|BEGIN (RSA|OPENSSH|PRIVATE)|onion_key' && echo "STOP: secret?" || true
  ```

### 4.3 Tests first
- Every rule in `DESIGN.md` that you implement gets at least one test that fails before your change and passes after it.
- Nothing merges unless the full suite passes on Linux, and from M1 onward in CI on all targets.
- Keep the existing 98 legacy tests passing until `legacy/` is deleted in M9.

### 4.4 Money and real API calls
- All development and tests use the **fake DeepSeek server** (§9.1).
- A real DeepSeek call is allowed only within the per-milestone cap in §0, or with the owner's explicit approval in the session stating a number of calls.
- Log every real call batch in `PROGRESS.md`: date, purpose, number of calls, measured cost.
- Never read, print, log, store or commit an API key. Keys come from the environment (`DEEPSEEK_API_KEY`) until the OS key store exists (M5).

### 4.5 Data safety
- Never touch the live corpus. Use the copy in §0, or fixtures you create in temp directories.
- Never delete or move files outside the repository and the temp directories you created.
- Never run destructive commands such as `rm -rf` with a variable or wildcard path you have not printed and checked first.

### 4.6 The legacy folder
- `legacy/` is today's working system with the M0 fixes. Change it only to fix a bug the owner asks you to fix.
- New code never includes or links legacy files, except the pure-core files copied in M1.
- `legacy/` is deleted only in M9, and only after the owner approves.

### 4.7 Dependencies
- Allowed third-party code: libcurl, cpp-httplib, Monocypher (with its optional Ed25519 file), the SQLite amalgamation, zstd, nlohmann/json (already vendored), the OpenCL headers (already vendored), GoogleTest.
- Anything else needs the owner's approval first.
- Pin exact versions, prefer releases at least two weeks old, and record each in `third_party/VERSIONS.md`: name, version, source URL, SHA-256, license.
- Vendored source or CMake `FetchContent` with `URL_HASH` are both fine. Unpinned downloads are not.

### 4.8 Portability and determinism
- Every OS call lives in `src/platform/`. No `#ifdef _WIN32` or `__APPLE__` anywhere else.
- Logs and exports are written in binary mode: LF line endings, UTF-8.
- **Records never contain floating-point numbers.** Scores, weights and costs are decimal strings (e.g. `"0.85"`) written with `std::to_chars`. Hashes and signatures therefore come out identical on every OS.
- C++17, CMake 3.20 or later. Warnings on (`-Wall -Wextra` / `/W4`), with no new warnings in your code.

### 4.9 Ambiguity and decision records
- If the spec does not decide something you need, either ask, or make the smallest reversible choice and write a decision record in `docs/decisions/NNNN-short-title.md`: context, options, choice, consequences.
- Examples that need a record: the multicast group address, the campaign file format, the Merkle leaf encoding, the TLS backend per OS.

### 4.10 Time and resources
- Ask before running anything expected to take more than 30 minutes (soaks, large imports), or that needs `sudo`.
- Do not leave background processes running at the end of a session. List any you started in `PROGRESS.md`.

---

## 5. Environment

| Item | Value |
|---|---|
| Development machine | Kali Linux (Debian-based), GCC 16.2, CMake, Python 3, OpenCL via POCL (the GPU tests run here) |
| Fleet targets | macOS 12+ on Mac Pros (6-core / 12-thread Xeon, 64 GB, FirePro D500 GPUs, clang); Linux; Windows 10 and 11 (MSVC 2022) |
| CI | GitHub Actions: `ubuntu-latest`, `macos-latest`, `windows-latest`. A self-hosted runner on a Mac Pro for macOS 12 is added when the owner sets it up. |
| Network | Home LAN only. Ports: UDP 7946 (gossip + LAN beacon), TCP 7947 (sync), 127.0.0.1:8080 (local API) |

---

## 6. Spec digest (constants and formats you will need constantly)

Everything here comes from `DESIGN.md`. Check there for context.

### 6.1 Identity and ids
| Item | Rule |
|---|---|
| Device key | Ed25519 keypair generated on first start |
| `device_id` | First 16 hex chars of BLAKE2b-256(public key) |
| `slot` | Top 20 bits of BLAKE2b-256(public key); slot 0 = legacy ids |
| Node id | `TK` + decimal of `slot × 2^40 + counter`; `int64_t` everywhere; format `TK%05lld` |
| `tk_next` at start | Highest id inside this device's own slot + 1 |
| Path | Parent path + `-` + own id; identity is the path, never the title |

### 6.2 Records and batches
- One commit = one **batch** = one JSON line in the origin's log:
  `{"v":1,"o":<device_id>,"b":<batch no>,"prev":<hex>,"hash":<hex>,"sig":<hex>,"recs":[{"s":<seq>,"h":<hlc>,"t":<type>,"p":{...}}, ...]}`
- `hash` = BLAKE2b-256 of the canonical bytes of `recs`. `prev` = hash of the previous batch; for batch 1, BLAKE2b-256(public key). `sig` = Ed25519 over the 64 raw bytes `prev ‖ hash`. Hex is lowercase.
- Canonical bytes: RFC 8785 JSON canonicalization. Since records hold no floats and all keys are ASCII, this means sorted keys, no whitespace, minimal string escaping and integers in decimal.
- HLC = `(physical_ms << 16) | logical_counter`; it is monotonic per device.
- Record types: `CAMPAIGN, RESERVE, CLAIM, REEXPAND, ATTEMPT, ADD_NODE, CRITIQUE, ATOMIC, SETTLE, NEEDS_REVIEW, REVIEW, RETITLE, TOMBSTONE, MEMBER_ADD, MEMBER_PROFILE, MEMBER_LEAVE, MEMBER_EVICT, CHECKPOINT`. Payload fields are listed in `DESIGN.md` › Op log format.
- **Commit order:** the batch is written (records, then `SETTLE` last) → one durable write → only then the SQLite store is updated and new children are pushed to their owners.
- **Verification on receipt:** signature valid, `prev` matches, `hash` matches, record numbers contiguous, origin is a member with no eviction cutoff below them. Anything else is quarantined, never applied.

### 6.3 Storage
- Log segments: `oplog/<device_id>/<first_seq>.log`. Sealed at 64 MB, zstd-compressed, never modified again.
- Durable write per batch: `F_FULLFSYNC` (macOS), `fsync` (Linux), `FlushFileBuffers` (Windows).
- Torn tail: on start, drop a trailing batch without a final LF or with a bad hash or signature.
- SQLite `onion.db` in WAL mode, schema exactly as in `DESIGN.md` › Storage, plus a `schema_version` table.
- Subtree query: `path = :p OR (path >= :p || '-' AND path < :p || '.')`.
- CSV export up to L9 only. Parquet export is optional (M8).
- Checkpoints: `VACUUM INTO`, manifest with version vector, Merkle root over `nodes` sorted by path, signed by two devices. Written every 6 h or every 100,000 applied records; keep the last 3.
- Compaction: `ATTEMPT` and `CRITIQUE` records older than 30 days are summarized per node in checkpoints.

### 6.4 LLM (DeepSeek only, text only)
- `node.conf` keys: `llm.provider, llm.base_url, llm.model, llm.max_tokens (6000), llm.timeout_s (120), llm.price_in, llm.price_cache, llm.price_out, llm.budget_usd, base_weight`.
- Every request: `temperature` as set by the gate, `stream:false`, `reasoning_effort:"none"`.
- `finish_reason == "length"` means a failed attempt.
- Fixed instruction text first in every prompt and node-specific text last, so DeepSeek's prompt cache can apply.
- Metering reads the usage block (cache-hit input, cache-miss input, output tokens) × configured prices. Verify the exact usage field names against DeepSeek's current API docs and record them in a decision record.
- Budget: before a ticket starts, reserve its worst case (pre-check + every agent call of 3 attempts at `max_tokens`); release the unused part at the end.
- Errors: HTTP 429/503 → backoff honouring `Retry-After`. Circuit breaker opens after 20 consecutive transport failures.
- Request limit (per device, adjusted every 30 s): any 429/503 or more than 5% timeouts → halve, then hold one window. Median latency ≤ 1.5 × the low-load baseline → +4. Latency rising without errors → hold. Starts at the profile's starting value and never exceeds the profile's ceiling.
- Async: libcurl multi interface on 1–2 threads; CPU work on a pool sized to the cores. Target: 1,000 requests in flight on about 20 threads total.

### 6.5 Quality gate
| Step | Rule |
|---|---|
| Pre-check | Call of at most 400 tokens: does the node split into at least 3 distinct parts? A clear "no" means leaf (`SETTLE` end = `cannot split`) |
| Attempt | Funnel: planner (4–8 aspects) → workers (≤100 each, merged ≤500) → dedup (sim ≥ 0.95) → trims (400, 300) → critique → synthesizer (3–100 children). Light funnel from the campaign's depth (default L8): 2–3 aspects, one trim |
| Checks | Complete reply, valid JSON, 3–100 children, no child repeats the node / an ancestor / a sibling / an existing child, style gates pass |
| Ancestor check | Parent similarity ≥ 0.85 → reject; any higher ancestor ≥ 0.95 → reject |
| Pass | Checks pass and critique score ≥ 0.7 → commit |
| Retry | Up to 3 attempts, previous critique issues injected, temperatures 0 → 0.3 → 0.5 |
| After 3 attempts | No attempt with ≥ 3 distinct valid children → leaf. Else best passing attempt with score ≥ 0.5 → commit, otherwise `NEEDS_REVIEW` |
| Atomic labels | Never a goal; a child labelled atomic is a leaf only after a short separate verification agrees |

### 6.6 Scheduling
- Ticket = an open node in a running campaign's scope: not settled, not a leaf, not in review, has allowance left, not under a `REEXPAND` lock.
- Home lane = the eligible device with the highest `score = −w / ln(h)`, where `h = (x + 0.5) / 2^64` and `x` = the first 64 bits (big-endian) of BLAKE2b-256(`node_path` ‖ `device_id`).
- Weight `w = base weight × speed factor`. Speed factor = settled home-lane nodes per hour ÷ fleet median, smoothed over 10 min, clamped to [0.5, 2]; stolen work excluded. `w = 0` when the device is paused, out of budget or flagged by the quality guard.
- Eligibility: ACTIVE and SUSPECT devices. A DEAD device keeps its lane until a majority of members is visible.
- Stealing: only when your own lane is short. Never from a DEAD lane without a majority, never from a budget-empty lane when spillover is off. Take from the far end, choosing at random among the top 5. `CLAIM` lease of 10 min, renewed every 3 min, timed on the **receiver's** clock.
- Campaign loop budget is taken in `RESERVE` chunks (default 50 attempts); reservations past the budget are void.
- Depth: no cap. Branch share = a child branch may spend at most 50% of its parent's remaining pool. Depth checkpoint (optional) every 5 levels past L9.
- Order: breadth-first = shallowest first; depth-first = deepest first; ties broken by path.

### 6.7 Membership and sync
| Item | Value |
|---|---|
| SWIM | Ping 1 random member per 1 s; ack timeout 500 ms; 2 indirect pings; SUSPECT; refute with incarnation + 1; DEAD after 5 min |
| Beacon | Signed UDP multicast every 5 s on the LAN; invitations also carry seed addresses |
| States | INVITED, JOINING, ACTIVE, SUSPECT, LEAVING, LEFT, DEAD, EVICTED |
| Join | Invite (24 h, single use, one public key) → slot collision check → `MEMBER_ADD` with first profile → clone checkpoint → pull tails → ACTIVE when lag < 100 records |
| Eviction | Majority signatures, or the offline admin key; auto-proposal after 30 days absent |
| Sync round | Every 15 s: compare vectors with one peer, pull missing ranges (sealed segments whole, active range as batches), verify, append durably, fold, hand to the state writer |
| Endpoints | `GET /sync/v1/vector`, `/sync/v1/log/<origin>?from=&max_bytes=8388608`, `/sync/v1/segment/<origin>/<first_seq>`, `/sync/v1/checkpoint/latest`, `/sync/v1/checkpoint/<hlc>.db`, `/sync/v1/merkle/<origin>?depth=` |
| Anti-entropy | Every 10 min, compare per-origin Merkle roots over 1,000-record leaves |
| Request auth | Ed25519 over method, path, body hash and timestamp (60 s window) + per-key nonce cache |
| New-task notices | After a commit, push new child paths to their lane owners; claims and locks are pushed the same way |

### 6.8 Profiles
- `data_depot node scan`: OS, CPU, RAM, disk, durable-write latency, GPU (OpenCL fp64), battery, link speed, round-trip time to DeepSeek `/models`. No LLM tokens spent.
- Rules version 1 defaults:

  | Setting | Desktop | Laptop |
  |---|---|---|
  | Base weight | 1.0 | 0.25 |
  | Start limit | 16 | 4 |
  | Ceiling | 2,000 | 32 |
  | CPU pool | cores | cores |
  | On-battery cap | none | 8 |
  | RAM floor | 2 GB | 1 GB |

- `MEMBER_PROFILE` is written only when something changed: `version` (+1), `prev`, `reason`, `changed`, `rules_version`, `schema`. `node.conf` overrides always win.

### 6.9 Security
- Sockets bind to the LAN interface (sync, gossip) or 127.0.0.1 (local API) only.
- The local API requires a per-device token and checks `Origin` and `Content-Type` on every POST.
- Key storage: Keychain (macOS), a 0600 file in a 0700 directory (Linux), DPAPI (Windows).
- The admin key is generated by `data_depot admin keygen --out <path the owner chooses>` and is never stored in the data directory.
- Never serve the data directory over HTTP.

---

## 7. Target repository layout

```
onion/
  CMakeLists.txt  CMakePresets.json
  third_party/    VERSIONS.md + pinned dependencies
  src/{core,gpu,platform,profile,llm,gate,sched,state,log,store,swim,sync,api,cli}/
  tests/{unit,golden,fleet}/
  tools/{fake_deepseek,chaos,import}/
  web/dashboard.html   tui/tui.py (optional)
  docs/{DESIGN.md,REBUILD_PLAN.md,decisions/,agent/}
  legacy/          today's code with M0 fixes (until M9)
```

**Dependency rule.** Each layer may use only the layers below it. Enforce this in CMake target dependencies.

| Layer | Modules |
|---|---|
| Interface | cli, api |
| Engine | gate, sched, state |
| Fleet | sync, swim, profile |
| Data | log, store, llm |
| Base | core, gpu, platform |

---

## 8. Interface guidance

Sketches only. Adapt the names, but keep the separation: **pure decision functions** with no I/O (easy to test), and thin I/O around them.

```cpp
// src/platform
namespace onion::platform {
  std::filesystem::path data_dir();                           // per-OS location from DESIGN.md
  class AppendFile { public: bool open(const std::filesystem::path&);
                     bool append(std::string_view bytes); bool sync(); };  // sync = durable write
  bool atomic_replace(const std::filesystem::path& tmp, const std::filesystem::path& dst);
  struct Sample { std::optional<double> load1, cpu_pct, ram_avail_gb, net_rx_mbps, net_tx_mbps, gpu_pct;
                  std::optional<bool> on_battery; };          // unknown = nullopt, never a made-up value
  Sample sample();
  class KeyStore { public: bool put(std::string_view name, std::string_view secret);
                   std::optional<std::string> get(std::string_view name); };
}

// src/log
namespace onion::log {
  using Hlc = uint64_t;  Hlc hlc_now();                       // monotonic
  std::string canonical_json(const nlohmann::json&);          // RFC 8785 subset, no floats allowed
  struct Batch { /* v,o,b,prev,hash,sig,recs */ };
  Batch make_batch(const DeviceKey&, uint64_t b, std::string prev_hex, std::vector<Record>);
  enum class Verify { Ok, BadSig, BadPrev, BadHash, Gap, NotMember, PastCutoff, BadVersion };
  Verify verify(const Batch&, const VerifyContext&);
  class Segments { /* append batch + durable sync, seal at 64 MB with zstd, torn-tail repair on open */ };
  struct Signer { virtual ~Signer() = default; virtual std::string sign(std::string_view) = 0; };  // NullSigner in M3, Ed25519 in M4
}

// src/llm
namespace onion::llm {
  struct Request { std::string model, system, user; int max_tokens; double temperature; };
  enum class Kind { Ok, Transport, Truncated, RateLimited, ServerError, Timeout };
  struct Result { Kind kind; int http_status; std::string content, finish_reason;
                  uint64_t in_hit, in_miss, out; std::string cost_usd; double latency_s; };
  class Client { public: void submit(Request, std::function<void(Result)> done); size_t in_flight() const; };
  class Limiter { public: void on_result(const Result&); int limit() const; };   // AIMD rules from §6.4, pure
  class Budget  { public: std::optional<Reservation> reserve(std::string usd); void settle(Reservation, std::string actual_usd); };
}

// src/gate (pure where possible)
namespace onion::gate {
  struct NodeCtx { std::string path, title; int layer; std::vector<std::string> ancestors, siblings, existing; };
  std::vector<std::string> deterministic_checks(const Attempt&, const NodeCtx&);  // pure
  enum class Decision { Commit, Retry, Leaf, NeedsReview };
  struct Outcome { Decision d; int chosen; std::string end_reason; };
  Outcome decide(const std::vector<Attempt>&, const GateParams&);                 // pure
}

// src/sched (pure where possible)
namespace onion::sched {
  double hrw_score(std::string_view path, std::string_view device_id, double weight);
  std::string lane_owner(std::string_view path, const MembersView&);             // includes majority rule
  bool may_steal_from(const Lane&, const MembersView&, const CampaignSpec&);
}
```

---

## 9. Test harnesses (build them when their milestone needs them)

### 9.1 Fake DeepSeek server, `tools/fake_deepseek/` (M2)
- Python 3 standard library only. It implements `POST /chat/completions` and `GET /models` in DeepSeek's response format, including `usage` with cache-hit and cache-miss input tokens.
- Driven by a JSON scenario file:
  - routes chosen by a substring of the user prompt, each with a response template;
  - latency (fixed, or normal with mean and standard deviation);
  - error rates for 429 and 503, with a `Retry-After` value;
  - truncation rate (`finish_reason:"length"` with cut-off JSON);
  - malformed-body rate;
  - an optional concurrency threshold above which latency rises (to test the limiter).
- It records every request (headers without the `Authorization` value, body, timing) to a JSONL file that tests can assert on.

### 9.2 Fleet harness, `tests/fleet/` (M6)
- Several node instances in one process, each with its own temp data directory, connected by an in-memory transport.
- The transport can drop, delay, duplicate and reorder messages, and partition and heal groups.
- Assertions on files and state: identical Merkle roots after settling, no duplicate paths, no orphans, budgets within bounds.

### 9.3 Chaos tools, `tools/chaos/` (M7)
- Kill a node process at a chosen commit step (environment variable hook).
- Simulate clock jumps (injectable clock).
- Simulate a full disk.
- Run real processes on one machine using different ports and data directories.

### 9.4 Golden files, `tests/golden/` (M1)
- Fixed inputs → expected canonical bytes, BLAKE2b hashes and Ed25519 signatures (deterministic keys from fixed seeds) stored as hex in the repo. The same test runs on every OS in CI.

---

## 10. Milestones

Each milestone lists its goal, tasks, the tests you must add, and the exit gate. Sizes and full context are in `REBUILD_PLAN.md`.

### M0 · Stabilize today's code (mostly done)
The P1–P7 fixes are already applied in `legacy/` and all 98 tests pass.
1. Confirm the build and tests still pass (§3).
2. Help the owner run **one real node** on the corpus copy (needs the owner's key and approval): pick an unprocessed TD and run `legacy/build/data_depot funnel --root <copy> --root-node <TD> --limit 1`. Look for `NODE_DONE … FINAL` and `errors=0`.
3. Record the result, the `DUP_PATHS` count and the cost in `PROGRESS.md`.

**Gate:** one real DeepSeek node succeeds; P7 tests pass. Tag `m0` on `main` after approval.

### M1 · Foundations
1. Branch `m1-foundations`. Create the layout from §7 (empty modules get a placeholder header and one smoke test).
2. Copy `legacy/depot_core.{hpp,cpp}` to `src/core/` and `legacy/depot_gpu.*` to `src/gpu/`, unchanged except for include paths.
3. Split `legacy/depot_core_test.cpp`:
   - pure-core tests (parsers, canonical, similarity, style, policy) move to `tests/unit/core_test.cpp`;
   - funnel, ingest and llm-loop tests stay in `legacy/`.
   - Record the counts.
4. Write the root `CMakeLists.txt` and `CMakePresets.json` (`linux-debug`, `linux-release`, `macos-release`, `windows-msvc`). The GPU layer is optional and auto-detected; `onion_gl` is not part of the new build.
5. Pin and add the dependencies (§4.7) and write `third_party/VERSIONS.md`. Choose the TLS backend per OS and write a decision record.
6. Implement `src/platform`: `data_dir`, `AppendFile` with a real durable `sync` per OS, `atomic_replace`, and `sample()` with Linux `/proc`, macOS (`getloadavg`, `host_statistics64`, `sysctl`, `getifaddrs`) and Windows (`GetSystemTimes`, `GlobalMemoryStatusEx`, `GetIfTable2`). Unknown values are `nullopt`. Key storage is file-based for now (0600 on Unix, DPAPI on Windows); Keychain comes in M5.
7. Implement the golden tests (§9.4): canonical JSON, BLAKE2b-256, Ed25519 on fixed inputs.
8. Add `.github/workflows/ci.yml`:
   - new tree on `ubuntu-latest`, `macos-latest`, `windows-latest`;
   - legacy tree on Ubuntu and macOS only;
   - cache the dependency downloads.
9. Write `docs/decisions/0001-repository-layout.md`.

**Tests to add:** platform (durable append survives reopen, atomic replace, sample returns plausible or `nullopt` values), golden vectors.
**Gate:** CI green on all three OSes; golden bytes identical everywhere; moved core tests pass; legacy suite still green on Linux and macOS.

### M2 · DeepSeek client
1. Build `tools/fake_deepseek` (§9.1) with example scenarios: `healthy.json`, `rate_limited.json`, `truncating.json`, `slow_under_load.json`.
2. Implement `src/llm`:
   - libcurl multi event loop on 1–2 threads, building requests per §6.4;
   - result classification;
   - metering, `Limiter` (AIMD), `Budget` reservations, the circuit breaker, `Retry-After` handling, timeouts;
   - the CPU pool.
3. Add a developer command `data_depot llm bench --scenario <file> --requests N` that reports throughput, latency percentiles, peak in-flight, thread count and spend.

**Tests to add:**
- Limiter: halves on 429/503 and on timeouts above 5%, +4 on healthy latency, holds when latency rises, never above the ceiling (unit tests, pure).
- Reservations prevent overspend with 500 concurrent submissions.
- Breaker opens after 20 consecutive transport failures.
- Truncation is classified.
- No API key appears in any log or recorded request.

**Gate:** against the fake server, 1,000 requests in flight on about 20 threads, no budget overspend, and the limiter behaves as specified. Optional, only with approval: ≤ 10 real calls to confirm the request format and usage fields.

### M3 · One-machine engine
1. `src/store`: SQLite schema from `DESIGN.md` + `schema_version`, WAL mode, the subtree query, CSV export (≤ L9).
2. `src/state`: the single state-writer thread fed by one queue; nothing else writes SQLite.
3. `src/log` (part 1): the `Record` and `Batch` types, canonical bytes and segment writing **with a `NullSigner`** (empty signature), so the commit order is real from the start. M4 swaps in Ed25519.
4. `src/gate`:
   - port the five agent prompts from `legacy/depot_io.cpp`, restructured with fixed text first;
   - add the pre-check prompt and the light funnel;
   - add the deterministic checks with the ancestor thresholds, `decide()`, atomic verification and the review outcomes.
5. `src/sched` (single device):
   - campaign file (JSON; decision record) with start, pause, resume and stop;
   - the ticket index;
   - breadth-first and depth-first order;
   - loop budget (local);
   - stop rule;
   - the review queue.
6. A **development import** from the legacy CSV/TSV files into SQLite (unsigned, for development only; M4 replaces it).
7. `src/api` and `src/cli`:
   - `node run`;
   - `campaign start|pause|resume|stop`, with the cost projection from a 20-attempt calibration run against the configured model;
   - `review accept|retry|reject`;
   - `/api/state` with campaigns, reviews and spend;
   - POST endpoints with token + `Origin` + `Content-Type` checks.
8. Update `web/dashboard.html` to read the new `/api/state`: campaign panel and review queue with buttons.

**Tests to add:**
- every gate rule (one test per row of §6.5);
- the P7 rule (nothing committed before the decision);
- review accept/retry/reject;
- campaign stop is final;
- the API refuses a POST without a token or with a foreign `Origin`.

**Gate:** a test campaign of a few hundred nodes on one machine against the fake server: no failed attempt committed, review works from the dashboard, the cost projection is shown before start. Optional, with approval: a small real campaign within the agreed number of calls.

### M4 · Signed log and genesis import
1. Device keys, `device_id`, slot ids (§6.1). Generate on first `node run`; store with the platform key store.
2. Replace the `NullSigner` with Ed25519.
3. Implement batch verification (§6.2), segment sealing with zstd, torn-tail repair, and checkpoints (single device: one signature, with the co-sign field ready).
4. `data_depot admin keygen --out <path>`: writes the admin key pair to the path given, prints the public key, refuses to write inside the data directory.
5. `tools/import`: the genesis import from a corpus copy.
   - Duplicate paths produce a report of dropped variants that the owner must approve before the import continues.
   - Legacy ids keep their numbers in slot 0.
   - Settles, critiques and atomic flags are imported with `legacy: true`.
6. The import verifier: counts per layer, every parent present, Merkle root against a fresh read of the CSVs.
7. Write `docs/CUTOVER.md` following `REBUILD_PLAN.md` › Migration and cutover. The cutover itself happens only with the owner.

**Tests to add:**
- a batch round-trip;
- every rejection case of `verify()`;
- a torn tail at every byte offset of the last batch;
- the slot id bounds;
- a lone eviction signature is rejected (prepares M7);
- import of a fixture corpus with duplicates.

**Gate:** the import of the corpus copy matches counts, parents and Merkle root; the torn-tail test passes. The owner then performs the cutover with you.

### M5 · Profiles and service
1. `src/profile`: the scan (§6.8), rules version 1, overrides from `node.conf`, versions with `prev`, `reason` and `changed`, and `node profile show|history|diff`.
2. OS key stores: macOS Keychain, Windows DPAPI; the Linux 0600 file stays.
3. Services:
   - `node install-service` generates and installs a systemd unit (Linux), a launchd plist (macOS) or a Windows Service;
   - `node start|stop|status`;
   - automatic restart after a crash.
4. Concurrency defaults from the profile feed the limiter.

**Tests to add:** derived settings for sample machines on each OS (fixtures); overrides win; a version increments only on change; the service file contents (golden).
**Gate:** profiles correct on Linux in CI and on a Mac Pro (the owner runs it); the service restarts after `kill -9`; a live Mac Pro run holds its limit without 429 storms (owner-run, with approval for real calls).

### M6 · Sync between known machines
1. A static member list (`members.json`) for this milestone only.
2. `src/sync`:
   - HTTP server and client (cpp-httplib) on TCP 7947 bound to the LAN interface;
   - signed requests with the nonce cache;
   - all endpoints from §6.7;
   - vector comparison, range pulls, verification, durable append, fold;
   - co-signed checkpoints and new-task notices;
   - anti-entropy.
3. `src/sched`: home lanes via HRW over the static members; `RESERVE` chunks across machines.
4. The fleet harness (§9.2).

**Tests to add:**
- convergence under reorder, duplicates and drops;
- a verification failure quarantines and does not apply;
- the lane function is the same on every node;
- reservations past the budget are void;
- a notice lost in transit is recovered by the next round.

**Gate:** the harness converges in all fault scenarios; three Mac Pros run a campaign for 24 h and converge to one Merkle root (owner-run). Budget overshoot is at most one chunk per machine.

### M7 · Dynamic fleet
1. `src/swim`: SWIM with all the timings from §6.7, incarnations, piggybacked gossip of live values (lane sizes, limit, budget left, speed factor), the signed LAN beacon (multicast group chosen in a decision record), and the membership states.
2. Invitations (`node invite --pubkey --ttl`), `node join --token` with seeds, slot collision check, clone from checkpoint, the ACTIVE threshold.
3. Majority rule for DEAD lanes; eviction signature collection; admin-key eviction; auto-proposal after 30 days.
4. Stealing with `CLAIM` leases (receiver-timed), `REEXPAND` locks and the spillover rule.
5. Chaos tools (§9.3).

**Tests to add:** every row of the conflict table in `DESIGN.md` in both orders; a 2-and-2 split moves nothing; no stealing from a DEAD lane without a majority; leases expire on the receiver's clock; a lone eviction is rejected and an admin-signed one accepted; the speed factor ignores stolen work.
**Gate:** a Linux machine joins mid-run; a Mac loses power mid-run, with no restart of the others and no lost committed work; an idle machine steals without duplicates. The chaos suite passes.

### M8 · Depth and scale
1. Branch shares and starting pools (§6.6), depth checkpoints (no attempt spent), and the L9 guard removed everywhere.
2. Compaction of `ATTEMPT` and `CRITIQUE` records into checkpoints.
3. Cost and log-growth projection refined from measured data.
4. Observability in `/api/state` and the dashboard: lanes, claims, steals, review backlog, spend rate, disk growth per device.
5. Optional: Parquet export (ask before adding DuckDB, since it is a large dependency).

**Tests to add:** a single chain under a 50% share ends; a fake model that never says atomic still ends; a checkpoint ticket creates `NEEDS_REVIEW` without spending; compaction keeps the totals.
**Gate:** a test campaign that goes past L9 ends within its pools; the measured log growth matches the projection.

### M9 · Mixed fleet and hardening
1. Windows 10 and 11 machines join (owner-run); fix whatever they reveal.
2. Security review against the threat table in `DESIGN.md` › Security model, reported as a checklist with evidence for each row. Fuzz the batch parser and the HTTP request parser with libFuzzer or a simple random-input harness.
3. The full chaos suite across processes; a 24 h soak on macOS + Linux + Windows (owner-run).
4. Documentation: operator guide, troubleshooting, the full CLI reference.
5. With the owner's approval: delete `legacy/`, remove the legacy CI job, tag `v1.0`.

**Gate:** the mixed 24 h soak converges; the chaos suite passes; the security checklist is complete; `legacy/` is deleted.

---

## 11. Templates

### `docs/agent/PROGRESS.md`
```markdown
# Progress
## Current
- Milestone: M1 · branch m1-foundations · started YYYY-MM-DD
- Tests: legacy 98/98 · new N/N · CI: green/red
## Log (newest first)
- YYYY-MM-DD: what was done · tests · decisions · open questions
## Real API usage
| Date | Milestone | Purpose | Calls | Cost (USD) |
## Background processes started
- none
```

### Gate report (post it in the session and save it as `docs/agent/gates/MN.md`)
```markdown
# Gate report — MN <name>
**Result:** PASS / FAIL
**Exit gate checks:** each criterion from §10 with the command run and its output (last lines)
**Tests:** totals per suite and per OS (CI link)
**What was built:** modules, commands, files
**Decisions made:** links to docs/decisions/*
**Deviations from DESIGN.md:** none / list with reasons
**Known issues and risks:**
**Real API calls used:** N (cost)
**Next:** what MN+1 starts with
```

---

## 12. Definition of done (whole project)

- [ ] A macOS + Linux + Windows fleet runs a campaign for 24 h, converges to one Merkle root, stays within budget, and survives power cuts and partitions
- [ ] Every rule in `DESIGN.md` has a passing test; CI is green on all targets
- [ ] The security checklist is complete
- [ ] Operator documentation is written
- [ ] `legacy/` is deleted and `v1.0` is tagged

---

## 13. Kickoff message to give the agent

> Read `docs/agent/IMPLEMENTATION_BRIEF.md` completely, then `docs/DESIGN.md` and `docs/REBUILD_PLAN.md`.
> Follow §3 to check the repository, build and tests, and create `docs/agent/PROGRESS.md`.
> Then give me, in this order: (1) your environment check results, (2) anything in the three documents you find unclear or contradictory, (3) your task breakdown for M1 with an estimate per task.
> Do not write code until I reply `START M1`. Real DeepSeek calls are not allowed unless I approve them with a number.
