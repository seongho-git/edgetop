# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

**edgetop** — an htop-style terminal monitor for CPU, GPU, memory, and temperature. The defining
requirement is that it must not register on the machine it watches: no measurable CPU load, no GPU work,
and a memory footprint that is noise. Installing toolchains or libraries is fine; the runtime footprint is
what is constrained. Target hardware is an NVIDIA DGX Spark (GB10, aarch64, unified memory); the binary
must also run on a generic Linux host without a GPU.

- Roles: **design by Fable, implementation by Opus 5.5.** Design decisions live in `docs/design.md`;
  if implementation needs to deviate, update that document first.
- Language is **C11** (decided against C++ after measuring; see `docs/design.md` "C vs C++"). Build with
  `-std=c11 -O2 -Wall -Wextra -Wconversion -Wshadow -Werror -D_POSIX_C_SOURCE=200809L`, link `-ldl` only.
  `make check-footprint` must keep failing if any other shared library appears in `ldd edgetop`.
- Hardware facts (NVML support matrix, memory accounting, sensor layout) are in `docs/device-survey.md`.
  Read it before touching `nvml.c`, `proc.c`, or `thermal.c`.
- As of 2026-10-07 the repository contains docs only. The commands below are the planned interface;
  replace them with the real ones as soon as they exist.

## Working rules

1. **All temporary files and logs go in `./log/` only** — debug output, profiles, sensor dumps, scratch
   scripts, test intermediates. Never `/tmp`, never the repo root. `log/` is gitignored.
2. **Code, comments, docs, and commit messages are in English.** Comments only where the code cannot
   speak for itself (a kernel quirk, a non-obvious formula, a driver limitation); keep them short.
3. **Claude stages, the user commits.** After each implementation step: clean up (rule 4), run
   `git add` on the files that belong to the step, then propose the commit as one runnable heredoc command
   (no repeated `-m`; quotes and apostrophes in the body are safe):
   ```sh
   git commit -F - <<'EOF'
   <type>(<scope>): <summary>

   <body paragraph as one unbroken line>

   <another body paragraph as one unbroken line>
   EOF
   ```
   `type` is one of `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `chore`. Never hard-wrap a sentence
   across lines. Never run `git commit` or `git push`.
4. **Clean up before proposing a commit.** Delete `log/` files that the next step does not need, remove
   debug prints, commented-out experiments, and dead code. State in one line what was left in `log/`.
5. When a reading looks wrong, do not guess — compare against `nvidia-smi -q`, `htop`, or the raw `/proc`
   file at the same moment and record the comparison in `log/`.

## Hardware constraints that shape the code

- **GPU memory is not available from NVML or nvidia-smi** (unified memory; `nvmlDeviceGetMemoryInfo` returns
  NOT_SUPPORTED). Derive it as the `/proc/meminfo` residual and cross-check with the NVML per-process sum.
  Formula and verification are in `docs/device-survey.md`.
- **Never shell out to `nvidia-smi` in the sampling loop** (≈ 20 ms, 20 MB per call). `dlopen`
  `libnvidia-ml.so.1` and call NVML directly (microseconds). Every NVML call must tolerate rc 3
  (NOT_SUPPORTED): fan, memory clock, power limits, and memory temperature are unsupported on GB10.
- **Per-core frequency comes from `cpuinfo_avg_freq`** (5 µs for 20 cores; `EAGAIN` means idle). Never read
  `scaling_cur_freq` in the loop: ≈ 100 µs per core, an IPI to every core, and values above max.
  `cpuinfo_cur_freq` is root-only.
- **Slow sensors get a slow cadence.** NVMe temperature costs ≈ 700 µs per read (10 s cadence); ACPI thermal
  zones ≈ 6 µs each (every tick or 2 s).
- **NVML costs 15 MB of private memory at `nvmlInit`** and nothing reduces it. Own RSS target is ≈ 1 MB
  without GPU, ≈ 20 MB with; do not add anything that grows per tick. `--no-gpu` must skip `dlopen` entirely.
- **Budget per 1 s tick: < 150 µs CPU, 0 heap allocations, 1 wakeup, 1 `write()`.** Measured source costs
  are in `docs/design.md`; re-measure with `log/bench_*.c` before accepting a slower source.
- Thermal zones are unlabeled `acpitz`; GPU temperature must come from NVML.
- Two core clusters (Cortex-X925 on cpus 5–9, 15–19; Cortex-A725 on 0–4, 10–14) identified via
  `midr_el1`. Group rows by cluster; fall back to one group when MIDR is unreadable.
- No root. Everything is readable as an unprivileged user; do not add anything that needs `sudo`.

## Build, run, test (planned)

```sh
make                     # build ./edgetop
make debug               # -O0 -g -fsanitize=address,undefined
make check-footprint     # ldd must list only libc/libdl; --once VmRSS must be under budget
./edgetop                # TUI, 1 s refresh, q quits, +/- changes interval
./edgetop -d 0.5         # refresh interval in seconds
./edgetop --once         # one plain-text sample (used by make check)
./edgetop --json         # one JSON sample; --watch N streams NDJSON
make test                # unit tests: parsers against tests/fixtures/
make test-one T=<name>   # single test binary
make check               # --once vs nvidia-smi/htop/free, output in log/check-*.txt
make clean
```

## Implementation steps

Each step ends with cleanup, `git add`, and a proposed `git commit` command.

1. `chore`: Makefile, `src/` skeleton, `--once` text output, `/proc/stat` and `/proc/meminfo` parsers with tests.
2. `feat(cpu)`: per-core utilization and frequency, cluster detection, loadavg, PSI.
3. `feat(gpu)`: NVML dlopen wrapper, util/clock/power/temp/process list, NOT_SUPPORTED handling, no-GPU fallback.
4. `feat(mem)`: unified-memory decomposition (apps/gpu/kernel/cache/avail) with NVML cross-check.
5. `feat(ui)`: termios/ANSI renderer, row diffing, layout, keys, SIGWINCH, signal-safe restore, `--json`.
6. `perf`: measure own CPU and RSS (record in `log/`), reduce syscalls, confirm budget.
7. `docs`: README; replace the "planned" sections here with the real commands.
