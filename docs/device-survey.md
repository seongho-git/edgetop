# Device Survey — NVIDIA DGX Spark (2026-10-07)

Facts gathered on the target machine before writing any code. Raw dumps and the
NVML probe script live in `log/` (not committed); this document is the durable record.

## Platform

| Item | Value |
|---|---|
| Product | NVIDIA DGX Spark (`/sys/class/dmi/id/product_name` = `NVIDIA_DGX_Spark`, board `P4242`) |
| OS / kernel | Ubuntu 24.04.3 LTS, Linux 6.11.0-1014-nvidia, **aarch64** |
| Uptime at survey | 63 days, load 1.13 / 1.07 / 1.01 |
| Memory | 127,606,860 kB (127.6 GB) **unified** (shared by CPU and GPU), **no swap** |
| Storage | NVMe 3.7 TB (`/dev/nvme0n1p2`, 13 % used) |
| Network | 4× Mellanox `mlx5` ports, 1× `enP7s7`, Wi-Fi `mt7925`, `docker0` |
| Shell env | `TERM=xterm-256color`, default 80×24 |

## CPU

20 cores, no SMT, one socket, two heterogeneous clusters.

| Cores | MIDR part | Type | min MHz | max MHz |
|---|---|---|---|---|
| 0–4 | `0xd87` | Cortex-A725 (efficiency) | 338 | 2808 |
| 10–14 | `0xd87` | Cortex-A725 (efficiency) | 338 | 2860 |
| 5–9 | `0xd85` | Cortex-X925 (performance) | 1378 | 3900 |
| 15–18 | `0xd85` | Cortex-X925 (performance) | 1378 | 3978 |
| 19 | `0xd85` | Cortex-X925 (performance) | 1378 | 4004 |

- Source for type: `/sys/devices/system/cpu/cpuN/regs/identification/midr_el1`, part number = bits [15:4].
- cpufreq: driver `cppc_cpufreq_epp`, governor `performance`, **one policy per core** (`policy0`..`policy19`).
- **`scaling_cur_freq` is unusable for monitoring:** ≈ 100 µs per core (2 ms for 20), sends an IPI to the
  target core, and returns CPPC estimates that exceed `cpuinfo_max_freq` (7332 MHz seen on a 2860 MHz core).
- **`cpuinfo_cur_freq` is root-only** (`Permission denied` as a normal user).
- **`cpuinfo_avg_freq` is the right source** (kernel 6.11, AMU counters): 5 µs for all 20 cores, values are
  plausible and never exceed max, and it returns `EAGAIN` ("Resource temporarily unavailable") when the core
  has been idle, which doubles as an idle indicator.
- `/proc/stat` has the standard 10 fields per CPU line.
- PSI available: `/proc/pressure/cpu`, `/proc/pressure/memory` (all 0.00 at survey).
- Flags of note: `sve`, `sve2`, `bf16`, `i8mm`.

## GPU — NVIDIA GB10

| Item | Value |
|---|---|
| PCI | `000f:01:00.0`, vendor `0x10de`, device `0x2e12`, rev a1 |
| Driver | 580.82.09 (open kernel module), CUDA 13.0 |
| NVML library | `/usr/lib/aarch64-linux-gnu/libnvidia-ml.so.1` |
| Device nodes | `/dev/nvidia0`, `/dev/nvidiactl`, `/dev/nvidia-uvm`, 16× `/dev/nvidia-fsN` (GPUDirect Storage) |
| Runtime PM | `active`, `D0` |
| At survey | 0 % util, P0, SM 2405 MHz, video 2067 MHz, 11.4 W, 48 °C |

### NVML support matrix (measured with `ctypes` against `libnvidia-ml.so.1`)

| Call | Result |
|---|---|
| `nvmlDeviceGetMemoryInfo` / `_v2` | **NOT_SUPPORTED (rc 3)** — no FB memory on a unified-memory part |
| `nvmlDeviceGetComputeRunningProcesses_v3` | OK — per-process `usedGpuMemory` is valid (e.g. sglang 97,716 MiB) |
| `nvmlDeviceGetUtilizationRates` | OK — `gpu` %, `memory` % (memory-controller busy %, *not* capacity) |
| `nvmlDeviceGetPowerUsage` | OK — mW, average. `nvidia-smi -q` also exposes an instantaneous reading |
| `nvmlDeviceGetTemperature(GPU)` | OK |
| `nvmlDeviceGetClockInfo` GRAPHICS / SM / VIDEO | OK |
| `nvmlDeviceGetClockInfo` MEM | NOT_SUPPORTED |
| `nvmlDeviceGetPerformanceState` | OK |
| `nvmlDeviceGetFanSpeed` | NOT_SUPPORTED (passively reported; chassis fan not exposed) |
| Power limits (current/default/min/max) | N/A in `nvidia-smi -q` |
| Memory temperature | N/A |

### Cost

| Method | Cost |
|---|---|
| 4 NVML calls via `ctypes` | ≈ 1–2 µs total (driver-side cached) |
| `nvidia-smi --query-gpu=...` one shot | ≈ 20 ms wall, 20 MB RSS |

Conclusion: never shell out to `nvidia-smi` in the sampling loop; load NVML directly.

### GPU memory on unified memory

GPU allocations do not appear in `AnonPages`, `Cached`, or `Slab`; they only reduce `MemFree`.
Verified decomposition from `/proc/meminfo` at survey time:

```
used        = MemTotal - MemFree - Buffers - Cached - SReclaimable            = 107.9 GiB
accounted   = AnonPages + SUnreclaim + KernelStack + PageTables
            + SecPageTables + Shmem + Percpu + VmallocUsed                     =   9.6 GiB
unaccounted = used - accounted                                                 =  98.3 GiB
NVML sum of per-process usedGpuMemory                                          =  97.9 GiB
```

The residual matches the NVML process total within 0.4 GiB (driver overhead). Report the residual
as "GPU/driver" and show the NVML process sum next to it as a cross-check.

## Thermal sensors

- `/sys/class/thermal/thermal_zone0..6`: all `type=acpitz`, backed by ACPI `LNXTHERM:00..06`,
  **no labels**, one `critical` trip (zone0: 104.8 °C), no cooling devices, policy `step_wise`.
  Readings at survey: 63.6 / 48.7 / 49.8 / 49.6 / 63.6 / 50.7 / 52.8 °C. Zones 0 and 4 are the
  hottest and track each other; they are the best CPU proxy, but the mapping is a guess.
- GPU temperature comes from NVML (48 °C) and is authoritative.
- hwmon: `hwmon0 acpitz` (same 7 zones), `hwmon1 nvme` (Composite / Sensor 1 / Sensor 2, ≈ 44 °C),
  `hwmon2–5 mlx5` (NIC), `hwmon6 mt7925_phy0` (Wi-Fi). Only `nvme` has labels.
- No `/sys/class/power_supply` entries (no battery, no reported PSU).

## Measured costs of the monitoring process itself

| Measurement | Result |
|---|---|
| Static-feeling C binary that reads `/proc/self/status` | RSS 0.98 MB |
| After `dlopen("libnvidia-ml.so.1")` | RSS 1.8 MB |
| After `nvmlInit` | RSS 19.7 MB, **15.0 MB private anonymous**, 1 extra thread |
| `nvmlInit` wall time | 5.2 ms |
| Effect of `NVML_INIT_FLAG_NO_ATTACH`, `MALLOC_ARENA_MAX=1`, `malloc_trim(0)` | none (±10 kB) |
| Full sampling tick (stat + meminfo + 20 freq + 7 thermal + 5 NVML) with `cpuinfo_avg_freq` | ≈ 125 µs |
| Same tick using `scaling_cur_freq` | ≈ 2134 µs |
| nvtop for reference | RSS 23 MB |

Per-source numbers are tabulated in `design.md` ("Measured sampling costs"). Harness sources are kept in
`log/bench_*.c` for the step 6 perf pass.

## Toolchain and existing tools

| Tool | Status |
|---|---|
| gcc / g++ 13.3, GNU make 4.3, cmake 3.28 | present |
| Python 3.12.3, stdlib `curses`, `psutil` 5.9.8 | present; `pynvml` absent |
| ncurses | runtime `libncursesw6` only — no headers (installable; not needed by the chosen design) |
| clang, rustc/cargo, go, node/npm | absent (installable if a design change requires them) |
| htop 3.3.0, nvtop 3.0.2, nvidia-smi | present (useful as references for cross-checking) |
| tegrastats, jtop | absent (this is not a Jetson) |

## Permissions

Everything needed is readable as an unprivileged user: `/proc/stat`, `/proc/meminfo`, `/proc/pressure/*`,
`/sys/devices/system/cpu/*/cpufreq/*`, `/sys/class/thermal/*`, `/sys/class/hwmon/*`, `/dev/nvidiactl`.
No root required.
