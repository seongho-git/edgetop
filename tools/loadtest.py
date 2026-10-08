"""Puts the machine under CPU, GPU, and memory load and checks edgetop while it runs.

Usage: python3 -I tools/loadtest.py [./edgetop] [seconds]
Load: a CUDA kernel loop (needs nvcc; skipped without it), four pinned CPU spinners, 4 GiB of touched
memory. Prints the reference nvidia-smi line, the 80x24 screen, the accuracy check, and edgetop's own
cost measured during the load. Everything temporary goes to log/.
"""
import mmap, os, shutil, subprocess, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from snap_tui import capture, screen  # noqa: E402


def gpu_binary():
    if not shutil.which("nvcc"):
        return None
    out = "log/gpuload"
    src = os.path.join(os.path.dirname(__file__), "load", "gpuload.cu")
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
        subprocess.run(["nvcc", "-O2", "-o", out, src], check=True)
    return out


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./edgetop"
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    os.makedirs("log", exist_ok=True)
    procs = []
    gpu = gpu_binary()
    if gpu:
        procs.append(subprocess.Popen([gpu, str(secs), "0"], stdout=subprocess.DEVNULL))
    else:
        print("nvcc not found: GPU load skipped")
    ncpu = os.cpu_count() or 4
    for cpu in sorted({ncpu - 1, ncpu - 2, ncpu // 2, 0}):
        procs.append(subprocess.Popen(["taskset", "-c", str(cpu), "sh", "-c",
                                       f"end=$(( $(date +%s) + {secs} )); while [ $(date +%s) -lt $end ]; do :; done"]))
    mem = mmap.mmap(-1, 4 << 30)
    for off in range(0, len(mem), 4096):
        mem[off] = 1
    time.sleep(6)

    if shutil.which("nvidia-smi"):
        ref = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,power.draw,temperature.gpu",
                              "--format=csv,noheader"], capture_output=True, text=True).stdout.strip()
        print("nvidia-smi during load:", ref)
    print("=== 80x24 under load ===")
    print(screen(capture([binary], (24, 80), b"", seconds=3.5), (24, 80)))
    print("=== accuracy check under load ===")
    r = subprocess.run([sys.executable, "-I", "tools/check.py", binary], capture_output=True, text=True)
    print(r.stdout.strip(), "\nexit", r.returncode)
    print("=== own cost under load (10 s) ===")
    print(subprocess.run([sys.executable, "-I", "tools/measure_tui.py", binary, "10", "nokeys"],
                         capture_output=True, text=True).stdout.strip())
    for p in procs:
        p.wait()
    mem.close()
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
