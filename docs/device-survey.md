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
| `nvmlDeviceGetPowerUsage` | OK — mW, driver average |
| `nvmlDeviceGetFieldValues(POWER_INSTANT=186)` | OK — mW, instantaneous |
| `nvmlDeviceGetTotalEnergyConsumption` | OK — mJ since driver load, but **2.4–2.7 ms per call**; its window average matched `GetPowerUsage` within 0.12 W, so edgetop does not use it |
| `nvmlDeviceGetViolationStatus(POWER)` | OK — cumulative ns power-capped, **1–2.3 ms per call**; advances ≈ 50 % of the time at P8 idle, 0 % during a 75 % compute load |
| `nvmlDeviceGetPowerManagementLimit` / `Enforced` / `Default` / `Constraints` | all NOT_SUPPORTED (no power limit exposed) |
| `nvmlDeviceGetTemperature(GPU)` | OK |
| `nvmlDeviceGetClockInfo` GRAPHICS / SM / VIDEO | OK |
| `nvmlDeviceGetClockInfo` MEM | NOT_SUPPORTED |
| `nvmlDeviceGetPerformanceState` | OK |
| `nvmlDeviceGetProcessUtilization` | OK — per-pid SM/mem/enc/dec % samples since a timestamp; returns NOT_FOUND (6) when idle and INSUFFICIENT_SIZE (7) if the buffer is smaller than the sample count (72 seen). **6–11 ms per call** |
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
Decomposition from `/proc/meminfo` (corrected 2026-10-07 during implementation; see note below):

```
cache       = Buffers + (Cached - Shmem) + SReclaimable
used        = MemTotal - MemFree - cache                                        = 108.3 GiB
apps        = AnonPages + Shmem                                                 =   7.5 GiB
kernel      = SUnreclaim + KernelStack + PageTables + SecPageTables
            + Percpu + VmallocUsed                                              =   2.0 GiB
residual    = used - apps - kernel                                              =  98.8 GiB
NVML sum of per-process usedGpuMemory (compute + graphics, 6 processes)         =  96.2 GiB
```

The residual exceeds the NVML process total by ≈ 2.5 GiB, which is GPU memory owned by the driver rather
than by any process. edgetop reports the residual as "gpu" and prints the NVML sum next to it.

Correction: the first version of this survey reported "98.3 GiB vs 97.9 GiB, within 0.4 GiB". That
mixed units (the NVML figure 97,886 MiB is 95.6 GiB, not 97.9) and double-counted Shmem (it is inside
`Cached`, so subtracting `Cached` and then adding `Shmem` back as used under-reports the residual by Shmem).
`tools/check.py` now verifies these numbers against nvidia-smi on every run.

## Thermal sensors

- `/sys/class/thermal/thermal_zone0..6`: all `type=acpitz`, one `critical` trip at 104.8 °C, no cooling
  devices, policy `step_wise`. The `type` carries no label, but the ACPI device path
  (`/sys/class/thermal/thermal_zoneN/device/path`) does:

  | zone | ACPI path | meaning | evidence |
  |---|---|---|---|
  | 0 | `\_TZ_.TSOC` | SoC, tracks the hottest cluster | rose with either cluster loaded |
  | 1 | `\_TZ_.TS0E` | cluster 0 efficiency cores (A725, cpus 0–4) | |
  | 2 | `\_TZ_.TS0P` | cluster 0 performance cores (X925, cpus 5–9) | 44 → 58 °C with cpus 5–9 spinning; others ≤ +5 |
  | 3 | `\_TZ_.TS1E` | cluster 1 efficiency cores (A725, cpus 10–14) | 44 → 48 °C with cpus 10–14 spinning; others ≤ +3 |
  | 4 | `\_TZ_.TS1P` | cluster 1 performance cores (X925, cpus 15–19) | |
  | 5 | `\_TZ_.TGPU` | GPU, firmware sensor | 2–5 °C above NVML's die reading under load |
  | 6 | `\_TZ_.TUNC` | uncore / memory controller | +8 °C during the X925 test |

  Cluster ids come from `/sys/devices/system/cpu/cpuN/topology/cluster_id` (56 for cpus 0–9, 1144 for
  10–19); each cluster holds five A725 and five X925 cores. The experiment (`taskset` spinners on one group
  of five cores for 25 s, all zones sampled before, during, and after) is reproducible with the shell loop
  recorded in `docs/design.md` §Accuracy.
- GPU temperature from NVML is the GPU's own die sensor and is the value edgetop labels `gpu`; the ACPI
  zone is shown as `gpu(acpi)`.
- hwmon: `hwmon0 acpitz` (same 7 zones), `hwmon1 nvme` (Composite / Sensor 1 / Sensor 2, ≈ 44 °C),
  `hwmon2–5 mlx5` (NIC), `hwmon6 mt7925_phy0` (Wi-Fi). Only `nvme` has labels.
- No `/sys/class/power_supply` entries (no battery, no reported PSU).
- **No CPU or system power sensor.** No `/sys/class/powercap` (no RAPL on this Arm platform), no hwmon
  `power*`/`energy*`/`curr*` attributes; the `LNXPOWER` ACPI objects are power resources, not meters. The
  only power figures on this machine are the GPU's.
- `/proc/stat` and `/proc/PID/stat` are in `USER_HZ` = 100 ticks per second (`getconf CLK_TCK`), so a
  per-core or per-process CPU percentage over a 1 s window has 1 % resolution, the same as htop; the
  kernel itself runs at `CONFIG_HZ=1000`.

## Measured costs of the monitoring process itself

| Measurement | Result |
|---|---|
| Static-feeling C binary that reads `/proc/self/status` | RSS 0.93–0.98 MB |
| Same with idiomatic C++ (`iostream`, `string`, `vector`) | RSS 2.8 MB, +3 shared libraries |
| After `dlopen("libnvidia-ml.so.1")` | RSS 1.8 MB |
| After `nvmlInit` | RSS 19.7 MB, **15.0 MB private anonymous**, 1 extra thread |
| `nvmlInit` wall time | 5.2 ms |
| Effect of `NVML_INIT_FLAG_NO_ATTACH`, `MALLOC_ARENA_MAX=1`, `malloc_trim(0)` | none (±10 kB) |
| edgetop, steady state at 1 s, machine idle, process list on | 0.21–0.29 % of one core (80x24 and 160x50 with graphs), RSS 21 MB (2.0 MB with `--no-gpu`) |
| edgetop, same with `--no-procs` | 0.05–0.08 % of one core, RSS 20.4 MB (1.5 MB with `--no-gpu`) |
| edgetop while GPU at 75–87 % and cores spinning | 0.9–1.3 % of one core with the process list (≈ 0.3 % of it is `GetProcessUtilization`, ≈ 0.1 % the power-cap counter), 0.18 % without |
| A full `/proc` scan (readdir + `/proc/PID/stat` for ≈ 560 processes) | 1.8 ms hot, 3.3 ms cold, before parsing |
| edgetop's effect on a pinned CPU benchmark / on GPU idle power | none measurable (36608 vs 36608 iterations; 11.46 vs 11.48 W) |
| htop 3.3.0 / nvtop 3.0.2, same harness | 2.26 % / 0.80 % of one core, RSS 5.4 / 23 MB |

Hot vs cold per-source costs are tabulated in `design.md` ("Measured sampling costs"). After a 1 s
sleep, sources cost 3–10× their hot-loop figures (cold caches, and wake-up on an A725 core at low clock);
NVML utilization and power are ≈ 300 µs each when cold.

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
