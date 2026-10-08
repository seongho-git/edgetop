# edgetop — Design Proposal

Status: implemented in v0.1.0 (design by Fable, implementation by Opus 5.5). Change this document first when a
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
link with `-ldl` only (on glibc ≥ 2.34 `dlopen` lives in libc, so `ldd` shows libc alone).
`make check-footprint` fails if `ldd edgetop` lists anything beyond `libc`, `libdl`, `ld-linux`, and `vdso`,
or if a `--json` run reports RSS above budget.

## Form

Single static-ish binary `edgetop`, three output modes sharing one sampler:

1. **TUI (default)** — full-screen, redraws only changed lines, `q` quits, `+`/`-` change interval,
   handles `SIGWINCH`. Designed for 80×24, usable down to 60×12 (core cells switch to a denser mode when
   the other panels would not fit); below that it shows a "too small" message instead of exiting.
2. **`--once`** — two samples one interval apart, printed as plain text. Same panels as the TUI without
   the footer; colors only when stdout is a terminal.
3. **`--json`** — the same window as one JSON object; `--watch SEC` streams NDJSON until SIGINT/SIGTERM.
4. **`--bench N`** — runs N sample+render ticks back to back and prints the per-tick cost (dev tool).

### Resource budget

Measured on the DGX Spark with `make measure` (pseudo-terminal, default 1 s interval, 30 s steady state,
`/proc/PID/schedstat`). htop and nvtop were measured the same way for reference.

| Item | Budget | Measured |
|---|---|---|
| CPU per 1 s tick, steady state, machine idle | < 1 ms (< 0.1 % of one core) | 0.36–0.78 ms (0.04–0.08 %); 0.44 ms pinned to an X925 core, 0.82 ms pinned to an A725 core |
| CPU per 1 s tick, machine under load (GPU 88 %, 4 cores spinning) | < 2.5 ms (< 0.25 %) | 1.76 ms (0.18 %); every source is ≈ 3× slower, `/proc/stat` included, so the cause is the loaded machine, not NVML contention |
| Wakeups per second | ≈ 1 | 1.3 (incl. startup/exit); timer slack raised to 5 ms so the kernel can merge the wakeup with other timers |
| Interference with a pinned CPU benchmark | none measurable | 36608 vs 36608 loop iterations in 8 s, with and without edgetop |
| Interference with GPU idle power / utilization | none measurable | 11.46 W vs 11.48 W, 0 % vs 0 % over 20 nvidia-smi samples |
| CPU per tick, hot loop (`--bench`) | < 100 µs | 53 µs without GPU, 56 µs with GPU (render ≈ 6 µs) |
| Own RSS, `--no-gpu` | < 2 MB | 1.3–1.5 MB |
| Own RSS, with NVML | < 21 MB (≈ 15 MB is NVML's, fixed) | 20.2–20.4 MB |
| Output to the terminal | — | ≈ 1.8–2.3 KB/s (only changed rows) |
| Heap allocations in the loop | 0 | 0 (all buffers static) |
| GPU work | none | NVML queries are driver ioctls; no CUDA context is created |
| Wakeups | 1 per tick | one `poll()` timeout per tick, no timers, no threads of our own |
| Reference: htop 3.3.0 | | 2.26 % of one core, 5.4 MB |
| Reference: nvtop 3.0.2 | | 0.80 % of one core, 23 MB |

The hot-loop and steady-state numbers differ by 6–10× because after a 1 s sleep every kernel path, NVML
ioctl, and cache line is cold, and the process may wake on an A725 core at a low clock. The original
"< 150 µs per tick" target was derived from hot-loop measurements and is replaced by the steady-state budget
above. Further reduction would mean dropping NVML utilization or power, which are core metrics, so they stay.

Thermal impact is not directly measurable at this level; as an estimate, 0.5 ms/s of work on a core that
draws a few watts when busy is on the order of 1–3 mW average, against a system idle draw of tens of watts.
The 20 MB RSS is 0.016 % of the 128 GB; no page cache is touched (every read is `/proc` or `/sys`).

Alternatives considered for going lighter and rejected: sampling GPU utilization and power less often than
the display refresh (hides the main metric's motion), pinning the process to a core or raising its nice value
(intrusive to the scheduler for no measurable gain), replacing `/proc/stat` with per-CPU cpuidle residency
files (20+ reads instead of one and no user/system split). `make profile` measures each source so a future
change can be judged against the table below.

Cadences (all others are read every tick):

| Source | Period | Why |
|---|---|---|
| thermal zones | 2 s | 7 ACPI `_TMP` evaluations, ≈ 150 µs cold |
| GPU process list + clock-event reasons | 3 s | two process-list calls ≈ 770 µs cold |
| NVMe temperature | 10 s | NVMe admin command, ≈ 700 µs per read |
| `scaling_cur_freq` fallback | 5 s | only when `cpuinfo_avg_freq` is missing |

Techniques: keep `/proc` and `/sys` fds open and `pread` at offset 0 each tick (one syscall per file,
a short read is EOF); one `nvmlInit` per run; no heap allocation inside the tick loop; one `write()` per
frame; slow sensors on a longer cadence.

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
| GPU process list | NVML compute + graphics `Get*RunningProcesses_v3`, merged by pid; name from `/proc/PID/cmdline` (keeps `setproctitle` names), user from the owner of `/proc/PID` | cached per pid; refreshed every 3 s |
| NVMe temperature | `hwmon` with `name=nvme`, `temp1_input` | ≈ 700 µs per read (NVMe admin command): sample every 10 s, not every tick |

### Nice to have (later, off by default)

Disk throughput (`/proc/diskstats`), network throughput (`/sys/class/net/*/statistics`), per-core
history sparklines, config file for panel order. (`NO_COLOR` / `--no-color` shipped in v0.1.0.)

### Memory decomposition shown in the Mem bar

```
cache   = Buffers + (Cached - Shmem) + SReclaimable
used    = MemTotal - MemFree - cache                       # identical to htop's "used"
apps    = AnonPages + Shmem
kernel  = SUnreclaim + KernelStack + PageTables + SecPageTables + Percpu + VmallocUsed
gpu     = used - apps - kernel                             # clamped at 0
avail   = MemAvailable
```

Shmem is part of `Cached` but cannot be reclaimed, so it counts as used (as in htop). The survey's first
formula subtracted it as cache and added it back as used, under-reporting the GPU share by Shmem (≈ 0.45 GiB).

On a machine without NVML the `gpu` segment still appears (it is just unaccounted memory) and is
labelled `other`.

## Implementation plan

Single process, single thread, one `struct sample` per tick. No heap allocation after startup.

```
startup:  parse args → discover cpus/policies/zones/hwmon (open fds once) → dlopen NVML (optional)
          → enter raw mode + alternate screen (TUI only) → install SIGINT/SIGTERM/SIGHUP/SIGWINCH handlers
loop:     t = previous sample time + interval
          sample_read(&cur)                      # pread every fd at offset 0, NVML ioctls
          compute(&prev, &cur, &view)            # /proc/stat deltas, meminfo decomposition, clamps
          render(&view) → frame buffer           # fixed-size char grid, rows × cols
          diff(frame, prev_frame) → one write()  # only changed rows, cursor-addressed
          swap(prev, cur); poll(stdin, until t)  # keys: q, +, -, p (pause), g (toggle GPU procs)
exit:     restore terminal (also from the signal handler via a single write()), nvmlShutdown
```

| Module | Responsibility | Key decisions |
|---|---|---|
| `main.c` | args, tick loop, signals | `poll()` on stdin until the next deadline (rounded up to whole ms so it never spins), so key presses redraw immediately without extra wakeups |
| `sample.h/.c` | `struct sample` (plain data, fixed arrays), sampler, view computation | max 256 cpus, 16 thermal zones, 64 GPU processes; pid → name/user cache |
| `proc.c` | `/proc/stat`, `/proc/meminfo`, `/proc/loadavg`, `/proc/pressure/*` | hand-written scanners, no `sscanf`; pure functions over a buffer so tests feed fixtures |
| `cpufreq.c` | policy discovery, `cpuinfo_avg_freq`, MIDR cluster map | `EAGAIN` → `freq_khz = 0`, rendered as `idle`; falls back to `scaling_cur_freq` every 5 s only if `cpuinfo_avg_freq` is absent |
| `thermal.c` | `thermal_zone*/temp`, hwmon `nvme` | zones every 2 s, nvme every 10 s, last values carried between reads |
| `nvml.c` | `dlopen`, symbol table, device 0 queries, process list | own ABI declarations (no `nvml.h` needed); `NOT_SUPPORTED` disables the field for the run so the UI hides it rather than printing 0 |
| `render.c` | TUI frame, row diffing, `--once` text, `--json` | one `char frame[rows][cols]` plus a parallel color byte grid; bars drawn with `|` in color, distinct ASCII glyphs per segment without color, `--unicode` for 1/8-step block glyphs |
| `term.c` | termios raw mode, alt screen, size, restore | restore sequence pre-built in a static buffer so the signal handler only calls `write()` |

Error policy: a missing file or unsupported NVML call hides that metric; it never aborts. The only fatal
errors are an unreadable `/proc/stat` or `/proc/meminfo`, and starting the TUI without a terminal.

Testing: `tests/test_main.c` runs the parsers against fixtures captured from this machine (`/proc/stat`,
`/proc/meminfo`, `/proc/loadavg`, PSI), a synthetic 256-cpu `/proc/stat`, the `cpuinfo_avg_freq` EAGAIN
case, the memory split invariants, and a render of every density mode. `make check` runs `tools/check.py`,
which compares a `--json` sample with `/proc/stat` read around the same window, htop's memory formula,
and `nvidia-smi`, writing the table to `log/check-<timestamp>.txt`.

## What is shown and how

Panels top to bottom. Every panel is independent: when its source is absent the panel is omitted and the
ones below move up.

| Panel | Rows | Content |
|---|---|---|
| **Header** | 1 | `edgetop`, hostname or DMI product, uptime, load 1/5/15, task counts (running/total from `/proc/loadavg`), clock |
| **CPU cores** | ⌈cores ÷ per-row⌉ per cluster | one cell per core: `id[bar pct MHz]`. Cells grouped by cluster with a label (`X925`, `A725`; `CPU` on homogeneous machines), fastest cluster first. Cell width 24 → 3 per row at 80 cols, 5 at 140. Bar splits user (green) and system (red); the percentage is colored by threshold; MHz from `cpuinfo_avg_freq`, `idle` when EAGAIN |
| **CPU total** | 1 | aggregate busy % bar with user/system/iowait split by color, PSI `cpu some avg10`, core count |
| **GPU** | 1–2 | `GB10 [bar util%]`, `membw %`, `SM MHz`, `W`, `P-state`, `°C`. Second row only if a slowdown reason (sw-power-cap, hw-slowdown, sw/hw-thermal, hw-power-brake) is active. Hidden with `--no-gpu` or no driver |
| **Memory** | 2 | row 1: stacked bar `apps|gpu|kernel|cache|free` over `MemTotal`, with `used/total`. row 2: the five numbers, plus `avail` and PSI `memory some avg10`. Swap row appears only if `SwapTotal > 0` |
| **Temperature** | 1 | `cpu <max zone>`, `gpu <NVML>`, `nvme <composite, 10 s>`, then all zone values compactly. Labels from hwmon when present |
| **GPU processes** | up to 5 in the TUI, 10 in `--once` (toggle `g`) | `PID USER MEM NAME` sorted by GPU memory; then a line with the process count, the NVML sum, and the meminfo residual as the cross-check |
| **Footer** | 1 | keys, current interval, and edgetop's own cost: `self 0.01% 1.1M` from `/proc/self/schedstat` (ns-exact) and `statm` so the budget is always visible |

Color: 16-color ANSI only. Thresholds: utilization green < 50 %, yellow < 80 %, red ≥ 80 %; temperature
green < 70 °C, yellow < 85 °C, red ≥ 85 °C. `NO_COLOR` or `--no-color` disables. No background colors,
so it reads on light and dark terminals.

Keys: `q` quit, `+`/`-` interval ×2 / ÷2 (0.25–10 s), `p` pause, `g` toggle process panel, `c` cycle
core cell density (bar+MHz / bar only / compact).

`--json` emits one object (sizes in KiB, absent metrics `null`):
`{ts, uptime_s, load:[1,5,15], tasks:{running,total}, cpu:{total_pct, user_pct, system_pct, iowait_pct, psi_some10, cores:[{id, cluster, pct, mhz}]}, gpu:{name, util_pct, membw_pct, sm_mhz, power_w, temp_c, pstate, clock_event_reasons, procs:[{pid, user, mem_mib, name}]}, mem:{total_kib, used_kib, apps_kib, gpu_kib, kernel_kib, cache_kib, free_kib, avail_kib, gpu_procs_kib, swap_total_kib, swap_free_kib, psi_some10}, temp:{cpu_c, gpu_c, nvme_c, zones_c:[...]}, self:{cpu_pct, rss_kib}}`.
`gpu` is `null` with `--no-gpu` or without a driver.

## Layout (80×24, captured from the running TUI)

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
               9650  corelab       525M  firefox
             252809  root          170M  python3
               3263  corelab        55M  Xorg
               3441  corelab        49M  gnome-shell
           6 procs  nvml sum 96.2G  meminfo resid 98.8G

 q quit  +/- 1s  p pause  g procs  c cells                   self  0.04%    20M
```

The product name (`NVIDIA_DGX_Spark`) is shown when the header has room for it next to the clock.
Memory is in GiB (the survey's 127.6 GB is MemTotal in decimal kB).

## Measured sampling costs (DGX Spark, 2026-10-07)

Per source, hot (back-to-back) and cold (after a 300 ms sleep, the realistic case); `make profile`
reproduces the table. Cold NVML figures vary run to run between ≈ 10 µs and ≈ 350 µs depending on driver
state, and every source is ≈ 3× slower while the GPU is busy.

| Source | Hot | Cold | Note |
|---|---|---|---|
| `/proc/stat` (20 cpus) | 34–45 µs | 111 µs | largest unavoidable item |
| `/proc/meminfo` | 1.5–3 µs | 32 µs | |
| `/proc/loadavg`, PSI ×2, `/proc/uptime` | 4 µs | 40 µs | |
| `cpuinfo_avg_freq` × 20 | 5 µs | 77 µs | `EAGAIN` on idle cores |
| `scaling_cur_freq` × 20 | **1997 µs** | — | rejected; also perturbs every core with an IPI |
| `cpuinfo_cur_freq` | n/a | n/a | root only |
| `thermal_zone*/temp` × 7 | 44–48 µs | 151 µs | 2 s cadence |
| nvme `temp1_input` | **714 µs** | — | 10 s cadence |
| NVML util / power | 0.2 µs | 348 / 289 µs | every tick (core metrics) |
| NVML temp / SM clock / pstate | < 0.4 µs | 78 / 63 / 12 µs | every tick |
| NVML clock-event reasons | 0.2 µs | 73 µs | 3 s cadence |
| NVML compute + graphics process lists | 102 µs | 768 µs | 3 s cadence |
| `nvmlInit` (once) | 5.2 ms | | +15 MB private anonymous memory |

## Accuracy policy

Every number must agree with `/proc/stat` over the same window, htop's memory formula, and `nvidia-smi`
within the tolerances in `tools/check.py` (1 pp CPU, 0.5 GiB memory, 1 °C, 15 MHz, 1.5 W, 1 MiB per GPU
process). Results on 2026-10-07, idle: every metric within tolerance; CPU total off by 0.05 pp, memory by
0.03 GiB, GPU temperature, SM clock, and per-process memory exact, power off by 0.01 W.
Under load (`make loadtest`: CUDA kernel loop at 88–89 % utilization, four cores spinning, 4 GiB allocated):
CPU total off by 0.26 pp, memory by 0.01 GiB, GPU utilization 89 vs 89 %, temperature 59 vs 59 °C, power
43.13 vs 43.13 W, all three GPU processes' memory exact. The screen showed the four spinning cores at 100 %,
GPU at 87 %, apps memory up by 4 GiB, and GPU temperature rising from 48 to 58 °C.

## Source layout

```
src/
  main.c        args, modes (TUI, --once, --json, --watch, --bench), tick loop, keys, signals
  sample.h/.c   data structures, sampler, delta and memory computation, pid cache
  proc.c        /proc/stat, /proc/meminfo, /proc/loadavg, PSI parsers
  cpufreq.c     topology (MIDR clusters), cpuinfo_avg_freq
  thermal.c     thermal zones, NVMe hwmon
  nvml.c        dlopen'd NVML wrapper
  render.h/.c   frame grid, panels, row diffing, text and JSON output
  term.h/.c     raw mode, alternate screen, async-signal-safe restore
  util.h/.c     pread helper, number scanners, size formatting
tests/
  test_main.c   unit tests; fixtures/ captured from the DGX Spark
tools/
  check.py            accuracy cross-check (stdlib only)
  check_footprint.sh  shared-library and RSS budget check
  measure_tui.py      steady-state CPU/RSS in a pseudo-terminal, key and restore test
  snap_tui.py         screen reconstruction at several terminal sizes
  profile_sources.c   hot and cold cost of every source, linked against the edgetop objects
  loadtest.py         CPU/GPU/memory load (load/gpuload.cu via nvcc) with screen, accuracy, and cost checks
```

Make targets: `all` (default), `debug`, `test`, `test-one T=<name>`, `check`, `check-footprint`, `bench`,
`measure`, `snap`, `profile`, `loadtest`, `clean`.
