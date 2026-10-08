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
| CPU per 1 s tick, idle, process list on (default), 80x24 | < 3.5 ms (< 0.35 % of one core) | 2.1–2.9 ms (0.21–0.29 %) across runs; the 2 s `/proc` scan of ≈ 560 processes is most of it |
| CPU per 1 s tick, idle, `--no-procs` | < 1 ms (< 0.1 %) | 0.36–0.78 ms (0.04–0.08 %); 0.44 ms pinned to an X925 core, 0.82 ms pinned to an A725 core |
| CPU per 1 s tick, idle, 160x50 with core boxes and the GPU box | < 4 ms | 2.1 ms (0.21 %); the larger frame adds rendering and ≈ 6.5 KB/s of output |
| CPU per 1 s tick, under load (GPU 75–87 %, cores spinning), process list on | < 15 ms (< 1.5 %) | 8.7–12.8 ms (0.9–1.3 %), of which ≈ 3 ms is `GetProcessUtilization` and ≈ 1 ms the power-cap counter; 1.76 ms (0.18 %) without the process list. Every source is ≈ 3× slower on the loaded machine, `/proc/stat` included, so the cause is not NVML contention |
| CPU per 1 s tick, idle, `--no-procs` | < 1 ms (< 0.1 %) | 0.36–0.78 ms (0.04–0.08 %); 0.44 ms pinned to an X925 core, 0.82 ms pinned to an A725 core |
| Wakeups per second | ≈ 1 | 1.3 (incl. startup/exit); timer slack raised to 5 ms so the kernel can merge the wakeup with other timers |
| Interference with a pinned CPU benchmark | none measurable | 36608 vs 36608 loop iterations in 8 s, with and without edgetop |
| Interference with GPU idle power / utilization | none measurable | 11.46 W vs 11.48 W, 0 % vs 0 % over 20 nvidia-smi samples |
| CPU per tick, hot loop (`--bench`) | < 100 µs | 53 µs without GPU, 56 µs with GPU (render ≈ 6 µs) |
| Own RSS, `--no-gpu --no-procs` | < 2 MB | 1.3–1.5 MB |
| Own RSS, `--no-gpu` | < 3 MB | 1.7–1.9 MB (the process lists add ≈ 0.4 MB once scanned; the static table is 1.4 MB but only touched pages count; history ring 130 KB) |
| Own RSS, with NVML and the process list | < 22 MB (≈ 15 MB is NVML's, fixed) | 20.5–21.3 MB |
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
| thermal zones | every tick | 7 ACPI `_TMP` evaluations, ≈ 150 µs cold; read every tick on purpose, temperature accuracy was requested over this cost |
| `/proc` scan for the process list | 2 s | ≈ 4 ms per scan for 560 processes (≈ 12 ms under load); skipped when the panel is off (`--no-procs`, `g`) or has no rows |
| GPU process list + per-process utilization | 3 s | two process-list calls ≈ 770 µs cold; `GetProcessUtilization` 6–11 ms while GPU processes exist. Clock-event reasons (≈ 6 µs cold) and instantaneous power (≈ 11 µs cold) are read every tick so the status row is live; the violation counter joins this cadence only while the GPU is busy |
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
| CPU temperature | `thermal_zone0..6`, classified by ACPI path | the firmware names them `TSOC`, `TS<cluster>E/P`, `TGPU`, `TUNC`; "cpu" = hottest SoC/core zone (direct on-die sensors), second row lists `soc`, `X925 c0/c1`, `A725 c0/c1`, `gpu(acpi)`, `uncore`. Mapping verified by loading each cluster separately (survey §Thermal). Machines without names fall back to the hottest zone, marked `?` |
| GPU temperature | NVML `GetTemperature` | the GPU's own die sensor; the ACPI `TGPU` zone runs 2–5 °C apart and is shown as `gpu(acpi)` |

### Should

| Metric | Source | Notes |
|---|---|---|
| Per-core frequency (MHz) | `policyN/cpuinfo_avg_freq` | AMU-based, 5 µs for 20 cores, already ≤ max; `EAGAIN` means the core is idle. **Never poll `scaling_cur_freq` per tick** (≈ 100 µs/core, IPIs every core, values exceed max) |
| Core type / cluster grouping | `midr_el1` part number | X925 vs A725 rows; generic fallback = one group |
| Load average, task counts | `/proc/loadavg` | |
| PSI cpu / memory | `/proc/pressure/*` | `some avg10`, highlights contention that util % hides |
| GPU SM clock, P-state | NVML `GetClockInfo(SM)`, `GetPerformanceState` | |
| GPU power (W) | NVML field `POWER_INSTANT` (186) on the bar line (≈ 10 µs); `GetPowerUsage` average on the status line | no power limit is exposed on GB10 (all limit calls NOT_SUPPORTED), so `cap n/a`. `GetTotalEnergyConsumption` would give an exact window average but costs 2.4–2.7 ms per call and agreed with the driver average within 0.12 W, so it is not used |
| GPU power-cap share | NVML `GetViolationStatus(POWER)` delta ÷ elapsed, on the 3 s cadence | 1–2 ms per call, and the counter also advances while the driver holds idle clocks down (≈ 50 % at P8), so it is sampled only while utilization > 0 |
| GPU clock-event reasons | NVML `GetCurrentClocksEventReasons` every tick | "SW power cap" is flagged at idle too; it counts as throttling only while busy |
| GPU memory-controller busy % | NVML `GetUtilizationRates.memory` | label it "membw", not "mem" |
| Process list (all processes) | `/proc/PID/stat` for every pid every 2 s; cpu% from utime+stime deltas (percent of one core, htop semantics); name from `/proc/PID/cmdline` once per new pid (keeps `setproctitle` names), user from the owner of `/proc/PID`; GPU memory and GPU% joined from NVML by pid | sorted by cpu, gpu (utilization, then memory), or rss (`s` key); top 64 kept, the screen shows what fits |
| GPU process list | NVML compute + graphics `Get*RunningProcesses_v3`, merged by pid | refreshed every 3 s; feeds the GPUMEM column and the nvml-sum cross-check |
| Per-process GPU utilization | `nvmlDeviceGetProcessUtilization` with the last seen timestamp; SM% per pid, max over the returned samples | supported on GB10 (verified: 67–70 % for a kernel loop while the device showed 87 %); 6–11 ms per call even when idle, so it shares the 3 s cadence and is skipped unless the process panel is visible and device utilization is above 0 (no process can have SM time otherwise); NOT_FOUND means no GPU work since the last call and resets to 0 |
| Utilization history | ring of 512 per-tick percentages per core, GPU compute, system memory used | drives the core box graphs (24 ticks) and the GPU box (one tick per column, up to the screen width) |
| NVMe temperature | `hwmon` with `name=nvme`, `temp1_input` | ≈ 700 µs per read (NVMe admin command): sample every 10 s, not every tick |

### Nice to have (later, off by default)

Disk throughput (`/proc/diskstats`), network throughput (`/sys/class/net/*/statistics`), per-core
config file for panel order. (`NO_COLOR` / `--no-color` and history graphs shipped since.)

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
| `sample.h/.c` | `struct sample` (plain data, fixed arrays), sampler, view computation | max 256 cpus, 16 thermal zones, 64 GPU processes, 4096 processes per scan; the process table is static storage referenced by pointer so its pages stay non-resident until the first scan |
| `proc.c` | `/proc/stat`, `/proc/meminfo`, `/proc/loadavg`, `/proc/pressure/*` | hand-written scanners, no `sscanf`; pure functions over a buffer so tests feed fixtures |
| `cpufreq.c` | policy discovery, `cpuinfo_avg_freq`, MIDR cluster map | `EAGAIN` → `freq_khz = 0`, rendered as `idle`; falls back to `scaling_cur_freq` every 5 s only if `cpuinfo_avg_freq` is absent |
| `thermal.c` | `thermal_zone*/temp` with ACPI-path classification, hwmon `nvme` | zones every tick, nvme every 10 s with the last value carried |
| `procs.c` | `/proc` scan, `/proc/PID/stat` parser, pid merge, top-N | fields counted from the last `)` (comm may contain spaces); tpgid and nice are signed; two 4096-entry lists swapped each scan, merged by pid for deltas; identity (name, uid) read once per new pid |
| `nvml.c` | `dlopen`, symbol table, device 0 queries, process list | own ABI declarations (no `nvml.h` needed); `NOT_SUPPORTED` disables the field for the run so the UI hides it rather than printing 0 |
| `render.c` | layout planner, TUI frame, core and GPU line graphs, row diffing, `--once` text, `--json` | one `char frame[rows][cols]` plus a parallel color byte grid; horizontal bars default to `|` as in htop/nvtop (one glyph per segment without color); `--bars line` gives ━ in half-cell steps with a dim ─ track, `--bars blocks` 1/8-step left blocks; graphs and box edges use box-drawing glyphs in unicode mode (braille was rejected as too dotted); frame cells are 16-bit for glyph-code headroom; GPU memory is magenta via 256-color index 164 (hue of 201 without its neon brightness) because palette magenta renders purple in most terminals |
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
| **CPU cores** | ⌈cores ÷ per-row⌉ per cluster (×4 for boxes) | one cell per core: `id[bar pct MHz]`. Cells grouped by cluster with a label (`X925`, `A725`; `CPU` on homogeneous machines), fastest cluster first. Cell width 24 → 3 per row at 80 cols, 5 at 140; the box form is a 26-wide, 6- or 4-row box with the id, %, MHz on its top edge and a 24-tick line graph inside (nvtop's algorithm, one series) whose whole line is colored by the current load with the usual 50/80 % thresholds. Bar splits user (green) and system (red); the percentage is colored by threshold; MHz from `cpuinfo_avg_freq`, `idle` when EAGAIN |
| **CPU total** | 1 | aggregate busy % bar with user/system/iowait split by color, PSI `cpu some avg10`, core count; at 110+ columns also the usr/sys/io percentages as text |
| **GPU** | 2 | row 1: `GB10 [bar util%]`, `membw %`, `SM MHz`, instantaneous `W`, `P-state`, `°C`. row 2 (always present): `throttle:` `idle` (GPU not busy) / `none` (busy at full clocks) / `sw-power-cap` (yellow, busy) / red slowdown reasons (hw-slowdown, sw-thermal, hw-thermal, hw-power-brake); `power: avg N W` (the driver's averaged reading); `cap n/a` (GB10 exposes no limit); `capped N%` share of the last 3 s spent power-capped, sampled and shown only while busy. Hidden with `--no-gpu` or no driver |
| **Memory** | 2 | row 1: stacked bar `apps|gpu|kernel|cache|free` over `MemTotal`, with `used/total`. row 2: the five numbers, plus `avail` and PSI `memory some avg10`. Swap row appears only if `SwapTotal > 0` |
| **Temperature** | 2 (1 without zones) | row 1: `cpu` (hottest CPU sensor, `?` when only an unlabeled max is available), `gpu` (NVML), `nvme`. row 2: every zone by name, clustered sensors as `X925 c0/c1`, `A725 c0/c1`, plus `gpu(acpi)` and `uncore`; unlabeled machines get the old `zones 63 49 …` row |
| **GPU box** | 16, 12 or 8 (only when the screen is large) | nvtop-style box titled with the GPU name, current compute % and memory % (GiB); inside, two continuous polylines (─ │ ╭ ╮ ╰ ╯, one tick per column) over a 10- or 6-row area: compute utilization (green) and system memory in use ÷ MemTotal (yellow), the GPU/MEM pair nvtop plots. nvtop's algorithm: values rounded to rows (0 % bottom, 100 % top), ─ for flat, ┌ ┐ └ ┘ + │ for a change, two columns per tick with one series moving per column and the other continued flat, ┬ ┴ ┼ where a vertical run crosses the other line; `100%`, `50%`, `0%` labels at the left |
| **Procs** | whatever is left, at least 4 (header + 2 + summary) | `PID USER CPU% GPU% RSS GPUMEM NAME COMMAND` (`GPU%`/`GPUMEM` only with a GPU; COMMAND is the space-joined argv, dim, cut at the screen edge), sorted by the `s` key (cpu, gpu, rss); running processes in bold; summary line with the process count, the sort key, the GPU process count, the NVML sum and the meminfo residual. `--once` shows up to 15 rows, `--json` 20 |
| **Footer** | 1 | keys, current interval, and edgetop's own cost: `self 0.01% 1.1M` from `/proc/self/schedstat` (ns-exact) and `statm` so the budget is always visible |

Color: 16-color ANSI only. Thresholds: utilization green < 50 %, yellow < 80 %, red ≥ 80 %; temperature
green < 70 °C, yellow < 85 °C, red ≥ 85 °C. `NO_COLOR` or `--no-color` disables. No background colors,
so it reads on light and dark terminals.

Keys: `q` quit, `+`/`-` interval ×2 / ÷2 (0.25–10 s), `p` pause, `g` toggle process panel, `s` cycle the
sort key, `c` cycle core cell density (box / bar+MHz / bar only / compact).

### Layout planner

`plan_layout()` starts from the richest layout and removes one element per step until the rows fit:

1. core boxes 6 → 4 rows (4-row → 2-row graph; width 26 plus a gap)
2. core boxes → full cells
3. GPU history box 16 → 12 → 8 rows (14 → 10 → 6-row graph); the GPU graph outranks core graphs
4. GPU history box off
5. core cells: full → bar → compact
6. the process list (it otherwise takes every remaining row, minimum 4)
7. the core panel
8. the second Temp row, then footer, then header, then Temp entirely, then the Mem detail row, and as the
   very last step the GPU status line, so three rows still show CPU, GPU and Mem

CPU, GPU and Mem bars are never removed; below 40 columns or 3 rows a one-line message is shown. Graph
features (steps 1–4) are only kept while the process list would still have at least 8 rows, so they appear
on large terminals and never squeeze the list. Measured: 160x50 → the 16-row GPU box and 4-row core boxes;
100x40 → the 16-row GPU box above full cells; 80x24 → neither. Measured on 80 columns with the two-row GPU panel: 24 rows → 5 processes,
8 rows → header, CPU, GPU (2), Mem (2), Temp (1), footer; 5 → CPU, GPU (2), Mem (2); 4 → CPU, GPU (2),
Mem; 3 → CPU, GPU, Mem. At 100x40 and 160x50 the
the core boxes and the GPU box are on. `main.c` runs the same planner before sampling and skips the `/proc`
scan when the list would be hidden.

Glyphs: block characters are used by default when the locale is UTF-8 (`LC_ALL`/`LC_CTYPE`/`LANG`);
`--ascii` forces plain characters, `--unicode` forces unicode glyphs. Horizontal bars use `|` by default
(the htop/nvtop look); full-height block glyphs fill the whole cell and look taller than the `[` `]`
around them, so they are opt-in via `--bars blocks`, as is the text-height line style via `--bars line`.

`--json` emits one object (sizes in KiB, absent metrics `null`):
`{ts, uptime_s, load:[1,5,15], tasks:{running,total}, cpu:{total_pct, user_pct, system_pct, iowait_pct, psi_some10, cores:[{id, cluster, pct, mhz}]}, gpu:{name, util_pct, membw_pct, sm_mhz, power_w, power_instant_w, power_capped_pct, temp_c, pstate, clock_event_reasons, procs:[{pid, mem_mib, sm_pct}]}, mem:{total_kib, used_kib, apps_kib, gpu_kib, kernel_kib, cache_kib, free_kib, avail_kib, gpu_procs_kib, swap_total_kib, swap_free_kib, psi_some10}, temp:{cpu_c, gpu_c, nvme_c, cpu_source:"labeled"|"max_zone", zones:[{label, c}]}, procs:[{pid, user, cpu_pct, gpu_pct, rss_kib, gpu_mib, name, cmd}] (top 20 by the sort key), self:{cpu_pct, rss_kib}}`.
`gpu` is `null` with `--no-gpu` or without a driver.

## Layout (80×24, captured from the running TUI)

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
        321363  seongho      0.5%    188M          claude
          3263  corelab      0.5%     41M     55M  Xorg
       561 procs, sort cpu  |  4 on gpu: nvml 662M, meminfo resid 2.9G
 q quit  +/- 1s  p pause  g procs  s sort  c cells           self  0.05%    20M
```

The product name (`NVIDIA_DGX_Spark`) is shown when the header has room for it next to the clock.
Memory is in GiB (the survey's 127.6 GB is MemTotal in decimal kB). This capture was taken after the
resident sglang server had been stopped, hence the small GPU share and the P8 idle state.

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
| NVML `POWER_INSTANT` field / clock-event reasons | 0.5 / 2 µs | 11 / 7 µs | every tick |
| NVML `TotalEnergyConsumption` | **2.7 ms** | 2.4 ms | not used |
| NVML `ViolationStatus(POWER)` | **1.0 ms** | 2.3 ms | 3 s cadence, busy only |
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

Thermal zone identity was verified by loading one cluster at a time and sampling every zone before, during,
and after (zone names from `/sys/class/thermal/thermal_zoneN/device/path`):

```sh
for cpu in 10 11 12 13 14; do taskset -c $cpu sh -c 'end=$(( $(date +%s) + 25 )); while [ $(date +%s) -lt $end ]; do :; done' & done
sleep 22; for i in 0 1 2 3 4 5 6; do printf "%s=%s " $(basename $(cat /sys/class/thermal/thermal_zone$i/device/path)) $(( $(cat /sys/class/thermal/thermal_zone$i/temp) / 1000 )); done; echo; wait
```

Only `TS1E` (and `TSOC`, which follows the maximum) moved for cpus 10–14; only `TS0P` moved substantially
for cpus 5–9 (44 → 58 °C). Both runs are tabulated in `device-survey.md` §Thermal.

## Source layout

```
src/
  main.c        args, modes (TUI, --once, --json, --watch, --bench), tick loop, keys, signals
  sample.h/.c   data structures, sampler, delta and memory computation, pid cache
  proc.c        /proc/stat, /proc/meminfo, /proc/loadavg, PSI parsers
  cpufreq.c     topology (MIDR clusters), cpuinfo_avg_freq
  thermal.c     thermal zones (ACPI-path classification), NVMe hwmon
  procs.c       /proc scan, /proc/PID/stat parser, cpu% deltas, top-N
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
