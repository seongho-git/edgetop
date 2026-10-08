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
- v0.1.0 (2026-10-07) implements the full design; every step in `docs/design.md` is done and verified.

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
   <type>(<scope>): <Summary starting with a capital letter>

   <body paragraph as one unbroken line>

   <another body paragraph as one unbroken line>
   EOF
   ```
   `type` is the kind of change (`init`, `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `chore`);
   `scope` is the unit, the core area, or the file language touched (e.g. `cpu`, `gpu`, `ui`, `c`, `docs`).
   Example from history: `init(docs): Add device survey, design proposal, and CLAUDE.md`.
   Never hard-wrap a sentence across lines. Never run `git commit` or `git push`.
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
- **NVML costs 15 MB of private memory at `nvmlInit`** and nothing reduces it. Own RSS target is ≈ 1 MB
  without GPU, ≈ 20 MB with; do not add anything that grows per tick. `--no-gpu` must skip `dlopen` entirely.
- **Budget: < 1 ms CPU per 1 s tick idle (measured 0.36–0.78 ms over several 20–30 s runs), < 2.5 ms under load (measured 1.76 ms),
  0 heap allocations in the loop, 1 wakeup, 1 `write()`.** Hot-loop numbers (`--bench`) understate real cost
  6–10× because every source is cold after the sleep; judge changes with `make measure` and `make loadtest`,
  not `--bench` alone. Per-source hot/cold costs are in `docs/design.md`; `make profile` re-measures them.
- Slow sources carry their last value between reads (thermal zones 2 s, NVML process list and clock-event
  reasons 3 s, NVMe 10 s). Keep new expensive sources on such a cadence.
- Thermal zones are unlabeled `acpitz`; GPU temperature must come from NVML.
- Two core clusters (Cortex-X925 on cpus 5–9, 15–19; Cortex-A725 on 0–4, 10–14) identified via
  `midr_el1`. Group rows by cluster; fall back to one group when MIDR is unreadable.
- No root. Everything is readable as an unprivileged user; do not add anything that needs `sudo`.

## Build, run, test

No `sudo` or extra packages are needed: gcc, make, libc headers, and the NVML runtime library are enough.

```sh
make                     # build ./edgetop (objects in build/)
make debug               # rebuild with -O0 -g -fsanitize=address,undefined
make test                # unit tests (run from tests/ so fixtures resolve)
make test-one T=meminfo  # one test by name; names are listed at the bottom of tests/test_main.c
make check               # tools/check.py: --json vs /proc/stat, htop's memory formula, nvidia-smi; report in log/
make check-footprint     # ldd must show libc only; RSS budget with and without NVML; prints --bench
make bench               # hot-loop per-tick cost with and without GPU
make measure             # real steady-state cost in a pseudo-terminal (30 s each, with and without GPU)
make snap                # print the TUI screen at 80x24, 140x40, 60x12, and too-small sizes
make profile             # hot/cold cost per source (tools/profile_sources.c against the edgetop objects)
make loadtest            # 40 s of CPU+GPU+memory load with screen, accuracy, and cost checks (GPU needs nvcc)
make clean

./edgetop                # TUI, 1 s refresh; keys q, +/-, p, g, c
./edgetop -d 0.5         # refresh interval in seconds (0.25-10)
./edgetop --once         # one window as text (colors only on a terminal)
./edgetop --json         # one window as JSON; --watch SEC streams NDJSON until SIGINT/SIGTERM
./edgetop --no-gpu       # skip NVML entirely (RSS ~1.5 MB instead of ~20 MB)
```

Verify TUI changes without a human at the terminal: `make snap` reconstructs the screen at several sizes,
`make measure` reports steady-state CPU from `/proc/PID/schedstat` (not `/proc/PID/stat`, whose tick-based
accounting is too coarse), exercises every key and a resize, and fails if the terminal is not restored, and
`make loadtest` repeats the screen, accuracy, and cost checks while the machine is busy.

## Architecture

One process, one thread. `main.c` owns the loop: `sampler_read` fills a `struct sample` from fds opened once
at startup, `compute_view` turns two samples into percentages and the memory split, `render` draws panels
into a fixed `struct frame` (character grid plus color grid), and `frame_encode` emits only rows that differ
from the last frame in a single `write()`. `--once`, `--json`, `--watch`, and `--bench` reuse the same
sampler and view; only the output step differs.

- `proc.c` parsers are pure functions over a buffer so `tests/` can feed fixtures captured from this machine.
- `nvml.c` declares the NVML ABI itself (no `nvml.h`). A field that returns NOT_SUPPORTED is disabled for the
  rest of the run, and the renderer hides absent fields instead of printing zeros.
- `render()` auto-switches to denser core cells when the other panels would not fit; below 60x12 it shows a
  "too small" message rather than exiting.
- Terminal restore must stay async-signal-safe (`term_restore_now` uses only `write` and `tcsetattr`); the
  SIGTSTP/SIGCONT handlers rely on that.
