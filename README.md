# edgetop

An htop-style terminal monitor for CPU, GPU, memory, and temperature that costs almost nothing to run.
Built for the NVIDIA DGX Spark (GB10, unified memory) and works on any Linux host, with or without a GPU.

```
 edgetop  eos  up 64d 00:37  load 0.20 0.68 0.75  2/1481               02:55:49
 X925    5[           0% 3519]   6[           1% 3728]   7[           0% idle]
         8[           0% idle]   9[           0% 3896]  15[           2% 3585]
        16[           0% 3791]  17[           0% 3733]  18[           0% 3915]
        19[           0% 3824]
 A725    0[           0% 2182]   1[           0% idle]   2[           0% 2404]
         3[           0% idle]   4[           2% 2596]  10[           0% 2522]
        11[           0% 2617]  12[           0% idle]  13[           3% 2569]
        14[           0% idle]
 CPU   [                                              0.5%]  psi 0.0%  20 cores
 GPU   GB10 [                             0%]  membw  0%  208MHz  4.6W  P8  43C
 Mem   [|||||                                                      6.1G/121.7G]
       apps 2.5G  gpu 2.9G  kernel 736M  cache 3.8G  avail 114.1G  psi 0.0%
 Temp  cpu 45C  gpu 43C  nvme 41C
       soc 45  X925 44/44  A725 43/43  gpu(acpi) 45  uncore 43
 Procs     PID  USER         CPU%     RSS     GPU  NAME
        320844  seongho      4.5%    440M          claude
          1457  avahi        2.3%      7M          avahi-daemon
        541573  seongho      0.5%    331M          claude
          3263  corelab      0.5%     41M     55M  Xorg
       561 procs, sort cpu  |  4 on gpu: nvml 662M, meminfo resid 2.9G
 q quit  +/- 1s  p pause  g procs  s sort  c cells           self  0.05%    20M
```

## Footprint

Measured at a 1 s refresh on the DGX Spark:

| | CPU (one core) | RSS |
|---|---|---|
| edgetop, machine idle | 0.28 % (0.07 % with `--no-procs`) | 21 MB (1.7 MB with `--no-gpu`, 1.4 MB with both off) |
| edgetop, GPU at 87 % and 4 cores busy | 0.87 % (0.18 % with `--no-procs`) | 21 MB |
| htop 3.3.0 | 2.26 % | 5.4 MB |
| nvtop 3.0.2 | 0.80 % | 23 MB |

Three quarters of edgetop's CPU time is the process list: a full `/proc` scan every 2 s (about 560
processes here). `--no-procs` or the `g` key turns it off. 15 MB of the RSS is allocated by the NVIDIA
management library itself; `--no-gpu` skips it.
The binary links only libc, runs as a normal user, wakes about once per second, and never creates GPU work.
Running it alongside a pinned CPU benchmark or next to the idle GPU produced no measurable change in
benchmark throughput, GPU power, or GPU utilization.

## Install

Requires gcc and make. No other packages, and no root unless you install system-wide.

```sh
make                              # builds ./edgetop
./edgetop                         # run from the build directory

make install                      # to ~/.local/bin as a normal user, /usr/local/bin as root
sudo make install                 # system-wide
make install PREFIX=/opt/edgetop  # anywhere else
edgetop                           # if $PREFIX/bin is on your PATH
make uninstall                    # same PREFIX rule
```

Without an NVIDIA driver the GPU panel is simply absent; nothing else changes.

## Usage

```
edgetop [options]
  -d, --delay SEC    refresh interval, 0.25-10 s (default 1)
      --once         print one sample as text and exit
      --json         print one sample as JSON and exit
      --watch SEC    print a JSON line every SEC seconds
      --no-gpu       do not load NVML (saves ~15 MB)
      --no-procs     skip the process list and its /proc scan
      --no-color     disable colors (also honors NO_COLOR)
      --unicode      draw bars with block glyphs
      --bench N      time N sample+render ticks and exit
  -h, --help         show this help
  -V, --version      show version

keys: q quit, +/- interval, p pause, g process list, s sort (cpu/gpu/rss), c core cell density
```

The TUI is designed for 80x24 and works down to 60x12. The process list takes whatever rows are left and
is the first thing to shrink when the window gets shorter; core cells switch to a denser form when that
keeps at least two process rows on screen, or when the fixed panels would not fit at all. Below 60x12 it
shows a "too small" message instead of exiting.
`--once` and `--json` sample twice, one interval apart, so utilization has a real window.

## What the numbers mean

- **CPU %** uses htop's definition: user+nice+system+irq+softirq+steal over the interval; iowait counts as idle.
- **MHz** comes from `cpuinfo_avg_freq`; `idle` means the core did not run during the window.
- **Mem gpu** is memory used by the GPU on unified memory. NVML cannot report it on GB10, so it is the part of
  used memory not explained by applications or the kernel; the NVML per-process sum is shown next to it.
- **membw** is the GPU memory controller's busy time, not memory capacity.
- **Temp cpu** is the hottest of the on-die CPU sensors; on the DGX Spark the firmware exposes them as ACPI
  zones named `TSOC` (SoC), `TS0P`/`TS1P` (performance cores of cluster 0 and 1: cpus 0–9 and 10–19), and
  `TS0E`/`TS1E` (efficiency cores). The second Temp row lists them as `soc`, `X925 c0/c1`, `A725 c0/c1`.
  The mapping was verified by loading each cluster separately. On a machine whose zones carry no usable
  name, `cpu` falls back to the hottest zone of all and is marked with `?`.
- **Temp gpu** is the GPU's own die sensor read through NVML; `gpu(acpi)` on the second row is the firmware's
  GPU zone, which runs a few degrees apart from it.
- **Procs** lists every process sorted by CPU% (the `s` key cycles cpu / gpu / rss); CPU% is percent of one
  core over the last scan interval, like htop. GPU is the process's GPU memory from NVML.
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

Expensive sources run on their own cadence and carry their last value in between: the `/proc` scan for the
process list every 2 s (skipped entirely when the panel is off or has no room), GPU process list and throttle
reasons every 3 s, NVMe temperature every 10 s. Thermal zones are read every tick. Anything the driver reports as
unsupported (fan, memory clock, power limit on GB10) is disabled for the run and hidden rather than shown as 0.

| File | Role |
|---|---|
| `src/main.c` | options, modes (TUI, `--once`, `--json`, `--watch`, `--bench`), tick loop, keys, signals |
| `src/proc.c` | `/proc/stat`, `/proc/meminfo`, `/proc/loadavg`, PSI parsers (pure functions, unit-tested) |
| `src/cpufreq.c` | core type detection from MIDR, per-core frequency from `cpuinfo_avg_freq` |
| `src/thermal.c` | ACPI thermal zones with name classification, NVMe hwmon |
| `src/procs.c` | `/proc/PID/stat` scan every 2 s, cpu% deltas, top-N selection |
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
