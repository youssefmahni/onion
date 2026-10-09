# GPU Layer — OpenCL similarity engine

Optional layer (`depot_gpu.cpp`), built only when `libOpenCL.so.1` is found (`DEPOT_OPENCL=ON`, auto-skips). Headers vendored in `third_party/OpenCL-Headers`.

## 1. CLI (`data_depot gpu`)

| Mode | Output |
|---|---|
| `--info` | human device list |
| `--json` | machine-readable device list (name/vendor/type/version/driver/global_mem_mb/max_work_group/fp16/fp64) — consumed by the controller |
| `--selftest` | known-answer 1024-element vector-add on the first GPU-preferred device |
| `--bench similarity [--k 64 --n 4096]` | CPU vs GPU pairs/s + speedup on the engine in use |

## 2. Engine design (`SimIndex` / `sim_keys_batch`)

- Every **fp64-capable** device (GPUs first, then CPU devices) gets its own context/queue/program.
- Calibration (3 reps of a 128×1024 matrix, avg of last two) ranks throughput; work split proportionally.
- **All devices enqueued before any read-back** → true concurrency.
- A **native CPU-thread engine** is always added and started *before* the GPU reads → real CPU+GPU overlap even on runtimes that serialize kernels (Apple).
- Kernel `sim_pair` reproduces `sim_keys` exactly (empty→0, equal→1, max(LCS-ratio, Jaccard-on-distinct-tokens)).
- Over-long (>256 chars) or unsorted inputs return −1.0 and are transparently recomputed on the CPU post-pass.
- Batch cap: 2^20 pairs per launch. `-cl-std=CL3.0` primary, `-DSIM_CL12` fallback (Apple OpenCL 1.2).

## 3. Env vars

| Var | Meaning |
|---|---|
| `ONION_CPU_THREADS` | CPU-share threads (default = hardware concurrency; 0 disables) |
| `ONION_GPU_ONLY <idx>` | pin one OpenCL device |
| `ONION_GPU_DEBUG` | split + per-engine timing + queue profiling |

## 4. Consumers

- `ingest --gpu` — accelerates the global spelling search over ~75k canonical candidates (CPU remains default and parity oracle; `--dry` runs are byte-identical).
- Funnel **DEDUP** step — per-node near-duplicate sweep (`sim_keys ≥ 0.95` drops) with CPU fallback on any GPU failure.
- Controller policy — caps threads at 2 when `gpu_pct > 90`.

## 5. Measured performance

- Mac Pro (two AMD FirePro D500, OpenCL 1.2, fp64): **~17–30× CPU-thread**.
- Linux/POCL (CPU+GPU): **~5.5× single thread**.

## 6. Known quirks

- POCL miscompiles dual unaligned global streams → copy strings to aligned private memory.
- Apple runtime serializes kernels across devices in one process → use the single fastest CL device + native CPU threads.
- Native CPU threads must start before the blocking GPU reads to get overlap.
- fp64 required; devices without it are ignored.
