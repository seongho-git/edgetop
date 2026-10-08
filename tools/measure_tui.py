"""Measures the TUI's steady-state cost inside a pseudo-terminal.

Usage: python3 -I tools/measure_tui.py "./edgetop [args]" [seconds] [nokeys]
CPU time comes from /proc/PID/schedstat (ns resolution) after a 2 s warm-up, so startup is excluded.
Unless "nokeys" is given, it then exercises every key and a resize, and checks the terminal is restored.
"""
import fcntl, os, pty, select, signal, struct, sys, termios, time

RESTORE = b"\x1b[0m\x1b[?25h\x1b[?1049l"


def run(args, seconds, keys, size=(24, 80)):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(args[0], args)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", size[0], size[1], 0, 0))
    os.kill(pid, signal.SIGWINCH)
    out = bytearray()

    def pump(t):
        end = time.time() + t
        while time.time() < end:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    out.extend(os.read(fd, 65536))
                except OSError:
                    return

    def cpu_ns():
        with open(f"/proc/{pid}/schedstat") as f:
            return int(f.read().split()[0])

    pump(2.0)
    start, b0 = cpu_ns(), len(out)
    pump(seconds)
    cpu = (cpu_ns() - start) / 1e9
    with open(f"/proc/{pid}/status") as f:
        rss = next(l for l in f if l.startswith("VmRSS")).split()[1]
    written = len(out) - b0
    for k in keys:
        if k == "RESIZE":
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 140, 0, 0))
            os.kill(pid, signal.SIGWINCH)
        else:
            os.write(fd, k.encode())
        pump(0.6)
    os.write(fd, b"q")
    pump(0.5)
    _, status = os.waitpid(pid, 0)
    return cpu, rss, written, os.waitstatus_to_exitcode(status), out.endswith(RESTORE)


def main():
    cmd = sys.argv[1].split() if len(sys.argv) > 1 else ["./edgetop"]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20
    keys = [] if len(sys.argv) > 3 else ["c", "c", "c", "g", "+", "-", "p", "p", "RESIZE"]
    cpu, rss, written, code, restored = run(cmd, secs, keys)
    print(f"{' '.join(cmd)}: {cpu / secs * 100:.4f}% of one core ({cpu / secs * 1e6:.0f} us per second), "
          f"rss {rss} kB, {written / secs:.0f} B/s written, exit {code}, terminal restored {restored}")
    return 0 if code == 0 and restored else 1


if __name__ == "__main__":
    sys.exit(main())
