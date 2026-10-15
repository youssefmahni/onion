# Onion operator guide

How to run the engine, add a machine to the fleet, and provision keys —
as shipped today. Commands are identical on every OS; only the service
installation and the binary names differ.

## The binary

One static-ish binary per machine, no installer:

| OS | Binary | Shared libraries it needs (install, don't copy) |
|---|---|---|
| macOS 12+ | `data_depot` | Nothing extra. Links the system libs only: `/usr/lib/libcurl.4.dylib`, `/System/Library/Frameworks/OpenCL.framework`, `/usr/lib/libc++.1.dylib`, `/usr/lib/libSystem.B.dylib` — all ship with macOS. |
| Linux x86_64 | `data_depot` | Distro packages: `libcurl4` (curl ≥ 7.66), an OpenCL ICD loader (`ocl-icd-libopencl1` or the GPU driver's), `libstdc++6` / `libgcc-s1` (a GCC version at least as new as the build toolchain), glibc. Nothing is bundled next to the binary. |
| Windows 10/11 x64 | `data_depot.exe` | The VC++ redistributable (`vc_redist.x64`, provides `vcruntime140.dll` etc.), `OpenCL.dll` (from the GPU driver; Windows ships a fallback), and the system API DLLs (always present). curl is linked **statically** (Schannel) — no `libcurl` DLL needed. |

SQLite, monocypher and nlohmann/json are compiled in. `data_depot --help`
lists every command; `data_depot <command> --help` lists its subcommands.

## Keys

The engine needs two keys and they are different things:

- **DeepSeek API key** — the `sk-...` key DeepSeek's API authenticates.
  Three ways to provide it (checked in this order):
  1. environment: `DEEPSEEK_API_KEY`
  2. `--key-file <path>` on `node run` — a plain-text file holding the
     API key on one line (the flag exists so the key can live in a
     root-only file or a mounted secret without touching the shell env)
  3. stored in the node key store: `data_depot node key set deepseek-key`
     (writes `keys/deepseek-key` in the data dir)
- **Device key** — Ed25519 identity, auto-generated on first use, stored
  as `keys/device-key` in the data dir. It identifies the machine in the
  fleet. Never copy it between machines.

## Running a single node

```sh
data_depot node run \
  --base-url https://api.deepseek.com --model deepseek-flash \
  --price-in 0.30 --price-cache 0.006 --price-out 1.20 \
  --budget-usd 100
```

The dashboard (and the knowledge tree) serves at `http://<host>:8080`
(API port is `--api-port`). Start a campaign:

```sh
data_depot campaign start campaign.json
# campaign.json:
# { "id": "p01-first", "scope": ["P01"], "loop_budget": 1000,
#   "mode": "breadth-first", "chunk": 25, "model": "deepseek-flash",
#   "structure": true, "audit_rate": "0.02" }
```

Notes: a campaign record written by the CLI folds at the next **node
startup** — restart the node after `campaign start` (or raise budgets via
the dashboard API). Raising `loop_budget` and re-starting the same id
resumes the open tickets (decision 0010).

## Ports

| Port | Proto | Use |
|---|---|---|
| 7947 | TCP | fleet sync (pull/join endpoint) — `--sync-port` |
| 7946 | UDP | swim membership gossip (multicast beacon group 239.255.77.31) |
| 8080 | TCP | dashboard + API — `--api-port` |

Open all three between fleet machines (LAN fleets: allow them in the
machine firewall).

## Adding a machine to the fleet

The fleet is masterless: any member can invite. The invitation token is
the credential — it is signed by the inviter, single-use (its nonce is
recorded), and expires after its TTL (default 24 h).

### 1. On the NEW machine: get its identity

```sh
data_depot node identity
# device_id=4e38671180165d7c
# public_key=51101c9b67805e381443f69ea8f331b6341c68a5d51bbacee30aca53eff8a660
# slot=320390
```

The first run creates the device key; send only the **public_key**
(64 hex chars) to whoever operates the fleet.

### 2. On an EXISTING fleet machine (the seed): issue the invitation

```sh
data_depot node invite --pubkey 51101c9b67805e381443f69ea8f331b6341c68a5d51bbacee30aca53eff8a660
# {"expires_at_s":1792092678,"inviter":"83c3fc6da1237b2b", ... "sig":"..."}
```

Copy that JSON to a file on the new machine, e.g. `invite.json`.
The seed node must be running with `--members` listing at least itself,
so its sync server binds and serves the join endpoint:

```sh
printf '[{"device_id":"83c3fc6da1237b2b","public_key":"<seed pubkey>",' \
' "url":"192.168.1.63:7947","swim":"192.168.1.63:7946"}]\n' > members.json
data_depot node run ... --members members.json --host 192.168.1.63
```

`--host` must be the machine's LAN IP, not `0.0.0.0` — the join response
advertises it as the sync URL.

### 3. On the NEW machine: join

```sh
data_depot node join --token @invite.json --seed 192.168.1.63:7947
# {"device_id":"4e38671180165d7c","members":[...],"ok":true,"slot":320390}
# joined: members.json written, checkpoint cloned from 192.168.1.63:7947
```

This writes `members.json` into the data dir and clones the seed's
checkpoint — the new machine starts with the fleet's whole tree.
(If the seed has no checkpoint yet the tails sync once the node runs.)

### 4. On the NEW machine: run the node

```sh
data_depot node run \
  --host 192.168.1.57 --members "$HOME/Library/Application Support/Onion/members.json" \
  --base-url https://api.deepseek.com --model deepseek-flash \
  --budget-usd 100
```

On macOS the data dir is `~/Library/Application Support/Onion`; on
Windows it is `%LOCALAPPDATA%\Onion`; on Linux `~/.local/share/onion`.
The node flips from JOINING to ACTIVE once its lag drops below 100 ops
(a few sync rounds). It then takes its share of the open tickets.

To detach a machine cleanly: `data_depot node stop --leave` (drains work,
records MEMBER_LEAVE). To force one out: `data_depot node evict --device <id>`.

## Service installation (autostart)

```sh
data_depot node install-service --binary /path/to/data_depot \
  --base-url https://api.deepseek.com --model deepseek-flash
```

prints a systemd unit (Linux), a launchd plist (macOS) or a Windows
service script — install it with the OS tooling and put the flags (the
same as `node run`, including `--members` and `--host`) into the printed
template. On macOS a LaunchDaemon needs the plist in
`/Library/LaunchDaemons` and the owner's sudo.
