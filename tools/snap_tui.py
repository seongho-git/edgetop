"""Runs the TUI in a pseudo-terminal and prints the final screen for several sizes.

Usage: python3 -I tools/snap_tui.py [./edgetop]
Only cursor-addressed writes and clears are emulated, which is all edgetop emits; colors are dropped.
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time

CASES = [
    ("80x24", (24, 80), [], b""),
    ("80x24 bar cells, procs hidden", (24, 80), [], b"cg"),
    ("140x40 unicode", (40, 140), ["--unicode"], b""),
    ("60x12", (12, 60), [], b""),
    ("50x10 too small", (10, 50), [], b""),
]


def capture(args, size, keys, seconds=2.5):
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

    pump(seconds)
    for k in keys:
        os.write(fd, bytes([k]))
        pump(0.4)
    os.write(fd, b"q")
    pump(0.4)
    os.waitpid(pid, 0)
    return out


def screen(data, size):
    rows, cols = size
    grid = [[" "] * cols for _ in range(rows)]
    r = c = 0
    last = None
    text = data.decode("utf-8", "replace")
    i = 0
    while i < len(text):
        m = re.match(r"\x1b\[([0-9;?]*)([A-Za-z])", text[i:])
        if m:
            arg, cmd = m.groups()
            if cmd == "H":
                p = [int(x) for x in arg.split(";")] if arg else [1, 1]
                r, c = p[0] - 1, p[1] - 1
            elif cmd == "J":
                grid = [[" "] * cols for _ in range(rows)]
            elif cmd == "l" and arg == "?1049":
                last = "\n".join("".join(x).rstrip() for x in grid)
            i += m.end()
            continue
        if 0 <= r < rows and 0 <= c < cols:
            grid[r][c] = text[i]
        c += 1
        i += 1
    return last


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./edgetop"
    for name, size, extra, keys in CASES:
        print(f"===== {name} =====")
        print(screen(capture([binary, *extra], size, keys), size))


if __name__ == "__main__":
    main()
