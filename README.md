# edgetop

An htop/nvtop-style terminal monitor for CPU, GPU, memory and temperature that costs almost nothing to
run. Built for the NVIDIA DGX Spark (GB10, unified memory); runs on any Linux host, with or without a GPU.

Layout and conventions follow [htop](https://htop.dev/) (per-core bars, process table, `|` bars) and
[nvtop](https://github.com/Syllo/nvtop) (GPU box with compute and memory lines, per-process GPU
utilization). The graphs are a port of nvtop's plotting algorithm.

![edgetop screenshot](docs/screenshot.png)

<!-- TODO: replace docs/screenshot.png with a capture of the TUI at about 160x50 -->

## Footprint

Measured at a 1 s refresh on the DGX Spark, as a share of one core:

| | CPU | RSS |
|---|---|---|
| edgetop, idle | 0.2–0.3 % (0.05–0.08 % with `--no-procs`) | 21 MB (1.5 MB with `--no-gpu --no-procs`) |
| edgetop, GPU at 87 % and 4 cores busy | 1.3 % | 21 MB |
| htop 3.3.0 | 2.26 % | 5.4 MB |
| nvtop 3.0.2 | 0.80 % | 23 MB |

15 MB of the RSS belongs to the NVIDIA management library; `--no-gpu` skips it. The binary links only
libc, runs as a normal user, wakes about once per second and never creates GPU work.

## Install

Requires gcc and make. No other packages, and no root unless you install system-wide.

```sh
make                              # builds ./edgetop
make install                      # ~/.local/bin as a user, /usr/local/bin as root
make install PREFIX=/opt/edgetop  # anywhere else; make uninstall uses the same PREFIX
```

Without an NVIDIA driver the GPU panels are simply absent.

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
      --unicode      unicode glyphs for bars and graphs (default in a UTF-8 locale)
      --ascii        plain ASCII bars and graphs
      --bars STYLE   horizontal bar style: ascii (default, like htop), line, blocks
      --no-graphs    never show core boxes or the GPU history graph
      --bench N      time N sample+render ticks and exit
  -h, --help         show this help
  -V, --version      show version

keys: q quit, +/- interval, p pause, g process list, s sort (cpu/gpu/rss),
      c core cells (box/full/bar/compact)
```

The screen adapts to the terminal: the process list shrinks first, then the graphs, then the core
panel; CPU, GPU and Mem bars are always present down to 40x3.

Design notes and hardware findings: [`docs/design.md`](docs/design.md),
[`docs/device-survey.md`](docs/device-survey.md). Tests and checks: `make test`, `make check`,
`make check-footprint`, `make measure`, `make loadtest`.
