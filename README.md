# edgetop

An htop-style terminal monitor for CPU, GPU, memory, and temperature that costs almost nothing to run.
Built for the NVIDIA DGX Spark (GB10, unified memory) and works on any Linux host, with or without a GPU.

```
 edgetop  eos  up 63d 13:47  load 1.10 1.10 1.03  2/1580               16:05:05
 X925    5[           0% idle]   6[           0% 3663]   7[           2% idle]
         8[           0% 3896]   9[           0% idle]  15[           0% 3857]
        16[           0% 3713]  17[           0% 3706]  18[           0% 3900]
        19[|||||||||100% 4000]
 A725    0[           0% 2610]   1[           0% 2177]   2[           0% 2637]
         3[           0% 2698]   4[           0% idle]  10[           1% 2287]
        11[           0% idle]  12[           0% 2293]  13[           0% 2379]
        14[           0% idle]
 CPU   [||                                            5.3%]  psi 0.00  20 cores
 GPU   GB10 [                           0%]  membw  0%  2405MHz  11.4W  P0  48C
 Mem   [|||||||||||||||||||||||||||||||||||||||||||||||||||||    108.3G/121.7G]
       apps 7.5G  gpu 98.8G  kernel 2.0G  cache 5.2G  avail 12.5G  psi 0.00
 Temp  cpu 63C  gpu 48C  nvme 43C   zones 63 48 50 49 63 50 52
 GPU procs      PID  USER           MEM  NAME
             253274  root         95.4G  sglang::scheduler
           6 procs  nvml sum 96.2G  meminfo resid 98.8G

 q quit  +/- 1s  p pause  g procs  c cells                   self  0.04%    20M
```

## Footprint

Measured at a 1 s refresh on the DGX Spark:

| | CPU (one core) | RSS |
|---|---|---|
| edgetop, machine idle | 0.04–0.08 % | 20 MB (1.5 MB with `--no-gpu`) |
| edgetop, GPU at 88 % and 4 cores busy | 0.18 % | 20 MB |
| htop 3.3.0 | 2.26 % | 5.4 MB |
| nvtop 3.0.2 | 0.80 % | 23 MB |

15 MB of edgetop's RSS is allocated by the NVIDIA management library itself; `--no-gpu` skips it.
The binary links only libc, runs as a normal user, wakes about once per second, and never creates GPU work.
Running it alongside a pinned CPU benchmark or next to the idle GPU produced no measurable change in
benchmark throughput, GPU power, or GPU utilization.

## Build

Requires gcc and make. No other packages and no root.

```sh
make
./edgetop
```

## Usage

```
edgetop [options]
  -d, --delay SEC    refresh interval, 0.25-10 s (default 1)
      --once         print one sample as text and exit
      --json         print one sample as JSON and exit
      --watch SEC    print a JSON line every SEC seconds
      --no-gpu       do not load NVML (saves ~15 MB)
      --no-color     disable colors (also honors NO_COLOR)
      --unicode      draw bars with block glyphs
      --bench N      time N sample+render ticks and exit
  -h, --help         show this help
  -V, --version      show version

keys: q quit, +/- interval, p pause, g GPU processes, c core cell density
```

The TUI is designed for 80x24 and works down to 60x12; core cells get denser automatically when the
other panels would not fit. Below 60x12 it shows a "too small" message instead of exiting.
`--once` and `--json` sample twice, one interval apart, so utilization has a real window.

## What the numbers mean

- **CPU %** uses htop's definition: user+nice+system+irq+softirq+steal over the interval; iowait counts as idle.
- **MHz** comes from `cpuinfo_avg_freq`; `idle` means the core did not run during the window.
- **Mem gpu** is memory used by the GPU on unified memory. NVML cannot report it on GB10, so it is the part of
  used memory not explained by applications or the kernel; the NVML per-process sum is shown next to it.
- **membw** is the GPU memory controller's busy time, not memory capacity.
- **Temp cpu** is the hottest ACPI thermal zone (the zones are unlabeled on this platform).
- **psi** is the kernel's pressure-stall figure (`some avg10`): the share of the last 10 s in which at least
  one task waited for CPU or memory. It shows contention that a utilization figure can hide.
- **self** in the footer is edgetop's own CPU share and RSS, from `/proc/self/schedstat` and `statm`.

## How it is built

One process, one thread, no dependencies beyond libc. Each tick is read, compute, draw, write:

1. **Read.** Every `/proc` and `/sys` file is opened once at startup and re-read with a single `pread` per
   tick. GPU values come from direct NVML calls through `dlopen("libnvidia-ml.so.1")`; `nvidia-smi` is never run.
2. **Compute.** Two consecutive samples give per-core and total utilization (htop semantics). Memory is split
   into apps, GPU, kernel, and cache from `/proc/meminfo`.
3. **Draw.** Panels are written into a fixed character grid plus a color grid. No heap allocation in the loop.
4. **Write.** Only rows that differ from the previous frame are sent, in one `write()`.

Expensive sources run on their own cadence and carry their last value in between: thermal zones every 2 s,
GPU process list and throttle reasons every 3 s, NVMe temperature every 10 s. Anything the driver reports as
unsupported (fan, memory clock, power limit on GB10) is disabled for the run and hidden rather than shown as 0.

| File | Role |
|---|---|
| `src/main.c` | options, modes (TUI, `--once`, `--json`, `--watch`, `--bench`), tick loop, keys, signals |
| `src/proc.c` | `/proc/stat`, `/proc/meminfo`, `/proc/loadavg`, PSI parsers (pure functions, unit-tested) |
| `src/cpufreq.c` | core type detection from MIDR, per-core frequency from `cpuinfo_avg_freq` |
| `src/thermal.c` | ACPI thermal zones, NVMe hwmon |
| `src/nvml.c` | runtime-loaded NVML wrapper with NOT_SUPPORTED handling |
| `src/sample.c` | sampler, utilization deltas, memory split, pid name/user cache |
| `src/render.c` | panels, row diffing, text and JSON output |
| `src/term.c` | raw mode, alternate screen, signal-safe terminal restore |

## Development

```sh
make                    # build ./edgetop
make debug              # AddressSanitizer + UBSan build
make test               # unit tests against fixtures captured on the DGX Spark
make test-one T=stat    # a single test by name
make check              # cross-check against /proc/stat, htop's memory formula, nvidia-smi (report in log/)
make check-footprint    # libc-only linkage and RSS budget
make bench              # hot-loop per-tick cost, with and without GPU
make measure            # steady-state CPU and RSS in a pseudo-terminal, key and resize test
make snap               # render the TUI at 80x24, 140x40, 60x12 and a too-small size
make profile            # hot and cold cost of every data source
make loadtest           # CPU/GPU/memory load (GPU part needs nvcc), then screen, accuracy and cost checks
make clean
```

Design and hardware notes: [`docs/design.md`](docs/design.md), [`docs/device-survey.md`](docs/device-survey.md).
