# edgetop — Design Proposal

Status: proposal (design by Fable, implementation by Opus 5.5). Change this document first when a
decision changes during implementation.

## Goal

An htop-style terminal monitor that shows CPU, GPU, memory, and temperature as accurately as the
kernel and driver allow, while being light enough that it does not register on the machine it watches:
no measurable CPU load, no GPU work, and a memory footprint that is noise on a 128 GB box.
Target: NVIDIA DGX Spark (see `device-survey.md`); must still run on a generic Linux box without a GPU.
Installing a toolchain or library is acceptable; the constraint is the runtime footprint, not the build.

## Language

**C11, no runtime dependencies beyond libc (and NVML when a GPU is present).**

| Option | Own RSS without NVML | Runtime overhead | Verdict |
|---|---|---|---|
| C11 + termios/ANSI, `dlopen` NVML | ≈ 1.0 MB (measured) | none; one `write()` per frame | **chosen** |
| Rust (std, no TUI crate) | ≈ 1.5–2.5 MB | none | acceptable alternative if memory safety is worth +1 MB and a toolchain install |
| C + ncurses | ≈ 2–3 MB | ncurses screen buffers, extra `write()`s | rejected: adds cost for nothing we need |
| Go | ≈ 6–10 MB | GC, runtime threads | rejected: background runtime work |
| Python 3.12 stdlib | ≈ 12–20 MB | interpreter, GC jitter | rejected: 10× footprint |

Rationale: the budget is the headline requirement and installing a toolchain is allowed, so the choice is
whichever language leaves the smallest, most predictable footprint. That is C: a 1 MB static-feeling binary,
no runtime threads, no allocator activity in the loop, and direct control of every syscall. NVML is loaded
with `dlopen("libnvidia-ml.so.1")` so the same binary runs where there is no NVIDIA driver (GPU panel hidden).

The one footprint we cannot engineer away is NVML itself: `nvmlInit` allocates **≈ 15 MB of private
anonymous memory** inside the driver library (measured; unchanged by `NVML_INIT_FLAG_NO_ATTACH`,
`MALLOC_ARENA_MAX=1`, or `malloc_trim`). Total RSS with GPU support is therefore ≈ 20 MB, of which
edgetop's own share is ≈ 1 MB. On the 128 GB target this is 0.015 % of memory. `--no-gpu` skips NVML
entirely and runs at ≈ 1 MB.

### C vs C++ — final decision: C11

Measured on the target with equivalent 10-line programs (read `/proc/self/status`, print one line):

| Variant | RSS | Shared libs | Notes |
|---|---|---|---|
| C11 (`gcc -O2`) | 932 kB | libc | baseline |
| C++20 subset: `std::array`, `std::string_view`, `-fno-exceptions -fno-rtti` | 932 kB | libc | libstdc++ not linked at all |
| C++20 idiomatic: `iostream`, `std::string`, `std::vector` | 2804 kB | libc, libstdc++, libm, libgcc_s | +1.9 MB, heap allocation on every line |

So C++ *can* match C, but only as a disciplined subset that must be policed (one stray `std::string`
pulls in libstdc++ and an allocator). The things C++ would buy do not pay for that policing here:

- **RAII for terminal restore is not a real win.** Destructors do not run on `SIGINT`/`SIGTERM`/`SIGHUP`.
  The terminal must be restored from a signal handler with a single async-signal-safe `write()` of the
  reset sequence, which is the same code in either language. C makes the constraint explicit.
- **Templates/`constexpr`** would serve two or three small parsers. Plain functions over fixed-size
  `char` buffers are shorter to read and have no hidden instantiation cost.
- **Zero heap in the loop** is natural in C (static buffers, fixed arrays sized by `sysconf(_SC_NPROCESSORS_CONF)`
  at startup); in C++ it is a rule that must be enforced against the standard library.
- **NVML is a C API** loaded through `dlopen`; function-pointer typedefs are idiomatic C.

Build contract: `gcc -std=c11 -O2 -Wall -Wextra -Wconversion -Wshadow -Werror -D_POSIX_C_SOURCE=200809L`,
link with `-ldl` only. `make check-footprint` fails the build if `ldd edgetop` lists anything beyond
`libc`, `libdl`, `ld-linux`, and `vdso`, or if a `--once` run reports VmRSS above budget.

## Form

Single static-ish binary `edgetop`, three output modes sharing one sampler:

1. **TUI (default)** — full-screen, redraws only changed lines, `q` quits, `+`/`-` change interval,
   handles `SIGWINCH`. Minimum 80×24; wider terminals get more cores per row.
2. **`--once`** — one sample, plain text, exit. For scripts and for cross-checking against htop/nvidia-smi.
3. **`--json`** — one sample as a flat JSON object (with `--watch N` to stream NDJSON). For logging.

### Resource budget

Derived from measured per-source costs (see "Measured sampling costs" below), to be re-verified in step 6.

| Budget item | Target | Basis |
|---|---|---|
| CPU time per 1 s tick | < 150 µs sampling + rendering (< 0.02 % of one core) | measured sources sum to ≈ 70 µs |
| Own RSS, `--no-gpu` | < 1.5 MB | 1.0 MB measured for a comparable static binary |
| Own RSS, with NVML | < 21 MB (≈ 15 MB is NVML's, fixed) | measured 19.7 MB after `nvmlInit` |
| Heap allocations in the loop | 0 | all buffers static or allocated at startup |
| Syscalls per tick | ≈ 30 `pread` + 5 NVML ioctls + 1 `write` | fds opened once, reused |
| GPU work | none | NVML queries are driver ioctls; no CUDA context is created |
| Wakeups | 1 per tick (`clock_nanosleep` to the next boundary) | no timers, no threads |

Techniques: keep `/proc` and `/sys` fds open and `pread` at offset 0 each tick; one `nvmlInit` per run;
no heap allocation inside the tick loop; one `write()` per frame; slow sensors on a longer cadence.

## Metrics

"Must" items are the user's hard requirements. Everything is sampled once per tick from the sources listed.

### Must

| Metric | Source | Notes |
|---|---|---|
| CPU utilization, total and per core | `/proc/stat` deltas | htop semantics: busy = user+nice+system+irq+softirq+steal; idle = idle+iowait |
| CPU memory usage | `/proc/meminfo` | used / available / cached / buffers; see decomposition below |
| GPU utilization | NVML `GetUtilizationRates.gpu` | % of time a kernel was running over the last sample window |
| GPU memory usage | `/proc/meminfo` residual + NVML process sum | no FB counters on unified memory (survey §GPU memory) |
| CPU temperature | `thermal_zone0..6` | 6 µs per zone; show max as "CPU", all zones in a compact row; zones are unlabeled |
| GPU temperature | NVML `GetTemperature` | authoritative |

### Should

| Metric | Source | Notes |
|---|---|---|
| Per-core frequency (MHz) | `policyN/cpuinfo_avg_freq` | AMU-based, 5 µs for 20 cores, already ≤ max; `EAGAIN` means the core is idle. **Never poll `scaling_cur_freq` per tick** (≈ 100 µs/core, IPIs every core, values exceed max) |
| Core type / cluster grouping | `midr_el1` part number | X925 vs A725 rows; generic fallback = one group |
| Load average, task counts | `/proc/loadavg` | |
| PSI cpu / memory | `/proc/pressure/*` | `some avg10`, highlights contention that util % hides |
| GPU SM clock, P-state | NVML `GetClockInfo(SM)`, `GetPerformanceState` | |
| GPU power (W) | NVML `GetPowerUsage` | average; no limit available on GB10 |
| GPU memory-controller busy % | NVML `GetUtilizationRates.memory` | label it "membw", not "mem" |
| GPU process list | NVML `GetComputeRunningProcesses_v3` + `/proc/PID/comm` | pid, user, GPU MiB, name |
| NVMe temperature | `hwmon` with `name=nvme`, `temp1_input` | ≈ 700 µs per read (NVMe admin command): sample every 10 s, not every tick |

### Nice to have (later, off by default)

Disk throughput (`/proc/diskstats`), network throughput (`/sys/class/net/*/statistics`), per-core
history sparklines, `NO_COLOR` / `--no-color`, config file for panel order.

### Memory decomposition shown in the Mem bar

```
apps    = AnonPages + Shmem
gpu     = (MemTotal - MemFree - Buffers - Cached - SReclaimable)
          - (AnonPages + SUnreclaim + KernelStack + PageTables + SecPageTables + Shmem + Percpu + VmallocUsed)
kernel  = SUnreclaim + KernelStack + PageTables + SecPageTables + Percpu + VmallocUsed
cache   = Buffers + Cached + SReclaimable
avail   = MemAvailable
```

On a machine without NVML the `gpu` segment still appears (it is just unaccounted memory) and is
labelled `other`.

## Implementation plan

Single process, single thread, one `struct sample` per tick. No heap allocation after startup.

```
startup:  parse args → discover cpus/policies/zones/hwmon (open fds once) → dlopen NVML (optional)
          → enter raw mode + alternate screen (TUI only) → install SIGINT/SIGTERM/SIGHUP/SIGWINCH handlers
loop:     t = next tick boundary
          sample_read(&cur)                      # pread every fd at offset 0, NVML ioctls
          compute(&prev, &cur, &view)            # /proc/stat deltas, meminfo decomposition, clamps
          render(&view) → frame buffer           # fixed-size char grid, rows × cols
          diff(frame, prev_frame) → one write()  # only changed rows, cursor-addressed
          swap(prev, cur); poll(stdin, until t)  # keys: q, +, -, p (pause), g (toggle GPU procs)
exit:     restore terminal (also from the signal handler via a single write()), nvmlShutdown
```

| Module | Responsibility | Key decisions |
|---|---|---|
| `main.c` | args, tick loop, signals | `clock_nanosleep(TIMER_ABSTIME)` to tick boundaries; `poll()` on stdin with the remaining time, so key presses redraw immediately without extra wakeups |
| `sample.h` | `struct sample` (plain data, fixed arrays) | sized at startup from `_SC_NPROCESSORS_CONF`; max 256 cpus, 16 thermal zones, 64 GPU processes |
| `proc.c` | `/proc/stat`, `/proc/meminfo`, `/proc/loadavg`, `/proc/pressure/*` | hand-written `strtoull` scanners, no `sscanf`; a line that fails to parse leaves the previous value and sets a stale flag |
| `cpufreq.c` | policy discovery, `cpuinfo_avg_freq`, MIDR cluster map | `EAGAIN` → `freq_khz = 0`, rendered as `idle`; falls back to `scaling_cur_freq` every 5 s only if `cpuinfo_avg_freq` is absent |
| `thermal.c` | `thermal_zone*/temp`, hwmon `nvme` | cadence table per sensor: zones every tick, nvme every 10 ticks |
| `nvml.c` | `dlopen`, symbol table, device 0 queries, process list | every call checks rc; `NOT_SUPPORTED` marks the field absent so the UI hides it rather than printing 0 |
| `render.c` | TUI frame, `--once` text, `--json` | one `char frame[rows][cols]` plus a parallel color byte grid; bars drawn with ASCII (`|`, `#`) by default, `--unicode` for block glyphs |
| `term.c` | termios raw mode, alt screen, size, restore | restore sequence pre-built in a static buffer so the signal handler only calls `write()` |

Error policy: a missing file or unsupported NVML call hides that metric; it never aborts. The only fatal
errors are an unwritable stdout and a terminal smaller than 60×12.

Testing: `tests/test_proc.c` runs the parsers against fixtures captured from this machine (`/proc/stat`,
`/proc/meminfo`, one `cpuinfo_avg_freq` EAGAIN case) and a synthetic 256-cpu `/proc/stat`. `make check`
compares a `--once` sample with `htop`-equivalent values computed from two `/proc/stat` reads, with
`free -b`, and with `nvidia-smi -q`, writing the diff to `log/check-<timestamp>.txt`.

## What is shown and how

Panels top to bottom. Every panel is independent: when its source is absent the panel is omitted and the
ones below move up.

| Panel | Rows | Content |
|---|---|---|
| **Header** | 1 | `edgetop`, hostname or DMI product, uptime, load 1/5/15, task counts (running/total from `/proc/loadavg`), clock |
| **CPU cores** | ⌈cores ÷ per-row⌉ per cluster | one cell per core: `id[bar pct MHz]`. Cells grouped by cluster with a label (`X925`, `A725`; `CPU` on homogeneous machines). Cell width 24 → 3 per row at 80 cols, 5 at 120. Bar is busy %; MHz from `cpuinfo_avg_freq`, `idle` when EAGAIN |
| **CPU total** | 1 | aggregate busy % bar with user/system/iowait split by color, PSI `cpu some avg10`, core count |
| **GPU** | 1–2 | `GB10 [bar util%]`, `membw %`, `SM MHz`, `W`, `°C`, `P-state`. Second row only if any clock-event reason (thermal/power slowdown) is active. Hidden with `--no-gpu` or no driver |
| **Memory** | 2 | row 1: stacked bar `apps|gpu|kernel|cache|free` over `MemTotal`, with `used/total`. row 2: the five numbers, plus `avail` and PSI `memory some avg10`. Swap row appears only if `SwapTotal > 0` |
| **Temperature** | 1 | `cpu <max zone>`, `gpu <NVML>`, `nvme <composite, 10 s>`, then all zone values compactly. Labels from hwmon when present |
| **GPU processes** | up to 5 (toggle `g`) | `PID USER MEM NAME` sorted by GPU memory; `USER` via `/proc/PID/status` Uid → `getpwuid` cached per pid; sum shown vs the meminfo residual as the cross-check |
| **Footer** | 1 | keys, current interval, and edgetop's own cost: `self 0.01% 1.1M` from `getrusage`/`VmRSS` so the budget is always visible |

Color: 16-color ANSI only. Thresholds: utilization green < 50 %, yellow < 80 %, red ≥ 80 %; temperature
green < 70 °C, yellow < 85 °C, red ≥ 85 °C. `NO_COLOR` or `--no-color` disables. No background colors,
so it reads on light and dark terminals.

Keys: `q` quit, `+`/`-` interval ×2 / ÷2 (0.25–10 s), `p` pause, `g` toggle process panel, `c` cycle
core cell density (bar+MHz / bar only / compact).

`--once` prints the same panels as plain text without bars redrawn. `--json` emits one object:
`{ts, uptime_s, load:[...], cpu:{total_pct, user_pct, system_pct, iowait_pct, psi_some10, cores:[{id, cluster, pct, mhz}]}, gpu:{util_pct, membw_pct, sm_mhz, power_w, temp_c, pstate, procs:[{pid, user, mem_mib, name}]}, mem:{total, apps, gpu, kernel, cache, avail}, temp:{cpu_c, gpu_c, nvme_c, zones:[...]}}`.
Absent metrics are `null`. `--watch N` repeats every N seconds as NDJSON.

## Layout (80×24 target)

```
 edgetop  NVIDIA_DGX_Spark  up 63d 12:32  load 1.13 1.07 1.01  2/1629     14:50:54
 X925   5[|||       23% 3858]   6[|          8% 3869]   7[           0% idle]
        8[           1% 3896]   9[           2% 3892]  15[|          4% 3725]
       16[||        11% 3861]  17[           0% idle]  18[           1% 3849]
       19[|          3% 3898]
 A725   0[|          5% 2728]   1[           2% 2624]   2[           0% idle]
        3[           1% 2753]   4[           1% 2808]  10[           1% 2709]
       11[           0% idle]  12[           0% idle]  13[           2% 2698]
       14[           0% idle]
 CPU   [|||                                          6.2%]  psi 0.0   20 cores
 GPU   GB10 [                                          0%]  membw 0%  2405MHz 11W 48C P0
 Mem   [AAAGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGKKCCC      ] 107.9/127.6G
       apps 7.5G  gpu 98.3G  kernel 2.1G  cache 5.3G  avail 12.4G   psi 0.0
 Temp  cpu 63C  gpu 48C  nvme 44C   zones 63 49 50 50 63 51 53
 GPU procs    PID  USER      MEM   NAME
           253274  seongho  95.4G  sglang::scheduler
           252809  seongho   170M  python3
                                                         sum 95.6G / resid 98.3G

 q quit  +/- 1.0s  p pause  g procs  c cells                      self 0.01% 1.1M
```

## Measured sampling costs (DGX Spark, 2026-10-07)

Measured with a C harness (`log/bench_break.c`) using persistent fds and `pread`; 200–1000 iterations each.

| Source | Cost per tick | Note |
|---|---|---|
| `/proc/stat` (20 cpus) | 44.5 µs | largest unavoidable item |
| `/proc/meminfo` | 1.5 µs | |
| `/proc/loadavg`, `/proc/pressure/cpu` | 0.4 µs, 1.2 µs | |
| `cpuinfo_avg_freq` × 20 | 5.0 µs | `EAGAIN` on idle cores (≈ 20 % of reads) |
| `scaling_cur_freq` × 20 | **1997 µs** | rejected; also perturbs every core with an IPI |
| `cpuinfo_cur_freq` | n/a | root only |
| `thermal_zone*/temp` × 7 | 43.5 µs | ACPI `_TMP` evaluation |
| nvme `temp1_input` | **714 µs** | rejected per tick; 10 s cadence |
| NVML util / power / temp / clock / pstate | 15.8 / 11.9 / 0.4 / 0.3 / 0.2 µs | after one-time `nvmlInit` (5.2 ms, +15 MB anon) |

Per-tick total with the chosen sources: ≈ 125 µs including thermal zones, ≈ 70 µs if thermal zones move
to a 2 s cadence. At a 1 s interval this is 0.007–0.013 % of one core.

## Accuracy policy

Every number must agree with `htop`, `nvidia-smi -q`, and `free -b` sampled at the same moment to within
1 % (utilization) or 1 unit (MHz, °C, MiB). `make check` runs `./edgetop --once` alongside those tools
and writes the comparison to `log/check-<timestamp>.txt`.

## Source layout (planned)

```
src/
  main.c        arg parsing, tick loop, signal handling
  sample.h/.c   struct sample; one function that fills it from all readers
  proc.c        /proc/stat, /proc/meminfo, /proc/loadavg, /proc/pressure
  cpufreq.c     policy discovery, cpuinfo_avg_freq (EAGAIN = idle), midr cluster detection
  thermal.c     thermal_zone and hwmon readers
  nvml.c        dlopen wrapper; every call tolerates NOT_SUPPORTED
  render.c      TUI (termios + ANSI), --once text, --json
tests/
  test_proc.c   parsers against fixture files in tests/fixtures/
Makefile
```
