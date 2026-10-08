"""Cross-checks one edgetop sample against nvidia-smi, free, and /proc/stat.

Usage: python3 -I tools/check.py ./edgetop
Writes a report to log/check-<timestamp>.txt and exits non-zero on any mismatch.
"""
import json
import os
import shutil
import subprocess
import sys
import time


def cpu_times():
    with open("/proc/stat") as f:
        v = [int(x) for x in f.readline().split()[1:9]]
    busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7]
    return busy, busy + v[3] + v[4]


def meminfo():
    out = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":")
            out[k] = int(v.split()[0])
    return out


def nvidia_smi():
    if not shutil.which("nvidia-smi"):
        return None
    q = "utilization.gpu,temperature.gpu,clocks.sm,power.draw"
    r = subprocess.run(["nvidia-smi", f"--query-gpu={q}", "--format=csv,noheader,nounits"],
                       capture_output=True, text=True)
    if r.returncode:
        return None
    vals = [x.strip() for x in r.stdout.splitlines()[0].split(",")]
    apps = subprocess.run(["nvidia-smi", "--query-compute-apps=pid,used_memory",
                           "--format=csv,noheader,nounits"], capture_output=True, text=True)
    procs = {}
    for line in apps.stdout.splitlines():
        pid, mem = [x.strip() for x in line.split(",")]
        procs[int(pid)] = int(mem)
    return {"util": vals[0], "temp": vals[1], "sm": vals[2], "power": vals[3], "procs": procs}


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./edgetop"
    rows, failed = [], False

    def row(name, ours, ref, tol, unit=""):
        nonlocal failed
        if ours is None or ref in (None, "[N/A]", "N/A"):
            rows.append(f"{name:<24} {str(ours):>12} {str(ref):>12}   skip")
            return
        diff = abs(float(ours) - float(ref))
        ok = diff <= tol
        failed |= not ok
        rows.append(f"{name:<24} {float(ours):>12.2f} {float(ref):>12.2f}   "
                    f"{'ok' if ok else 'MISMATCH'} (|d|={diff:.2f}{unit}, tol {tol}{unit})")

    b0, t0 = cpu_times()
    sample = json.loads(subprocess.run([binary, "--json", "-d", "1"], capture_output=True,
                                       text=True, check=True).stdout)
    b1, t1 = cpu_times()
    smi = nvidia_smi()
    m = meminfo()

    rows.append(f"{'metric':<24} {'edgetop':>12} {'reference':>12}   result")
    # the reference window is a few ms wider than edgetop's, so allow 1 percentage point
    row("cpu total %", sample["cpu"]["total_pct"], 100.0 * (b1 - b0) / (t1 - t0), 1.0, "pp")

    # htop's definition: used = total - free - buffers - (cached + sreclaimable - shmem)
    used_htop = m["MemTotal"] - m["MemFree"] - m["Buffers"] - (m["Cached"] + m["SReclaimable"] - m["Shmem"])
    row("mem total GiB", sample["mem"]["total_kib"] / 2**20, m["MemTotal"] / 2**20, 0.01)
    row("mem used GiB (htop)", sample["mem"]["used_kib"] / 2**20, used_htop / 2**20, 0.5)
    row("mem avail GiB", sample["mem"]["avail_kib"] / 2**20, m["MemAvailable"] / 2**20, 0.5)

    g = sample.get("gpu")
    if g and smi:
        row("gpu util %", g["util_pct"], smi["util"], 5, "pp")
        row("gpu temp C", g["temp_c"], smi["temp"], 1)
        row("gpu sm MHz", g["sm_mhz"], smi["sm"], 15)
        row("gpu power W", g["power_w"], smi["power"], 1.5)
        ours = {p["pid"]: p["mem_mib"] for p in g["procs"]}
        for pid, mib in sorted(smi["procs"].items()):
            row(f"gpu proc {pid} MiB", ours.get(pid), mib, 1)
        resid = sample["mem"]["gpu_kib"] / 2**20
        procs = sample["mem"]["gpu_procs_kib"] / 2**20
        rows.append(f"{'gpu meminfo residual':<24} {resid:>12.2f} {procs:>12.2f}   "
                    f"info (nvml process sum; gap {resid - procs:.2f} GiB is driver-owned)")
    else:
        rows.append("gpu                      skipped (no GPU in sample or no nvidia-smi)")

    rows.append(f"{'self rss KiB':<24} {sample['self']['rss_kib']:>12}")
    report = "\n".join(rows) + "\n"
    os.makedirs("log", exist_ok=True)
    path = time.strftime("log/check-%Y%m%d-%H%M%S.txt")
    with open(path, "w") as f:
        f.write(report)
    print(report + f"report: {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
