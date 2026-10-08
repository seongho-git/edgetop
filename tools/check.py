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
    """Returns {name: (busy, total)} for 'cpu' and every 'cpuN' line."""
    out = {}
    with open("/proc/stat") as f:
        for line in f:
            if not line.startswith("cpu"):
                break
            name, *vals = line.split()
            v = [int(x) for x in vals[:8]]
            busy = v[0] + v[1] + v[2] + v[5] + v[6] + v[7]
            out[name] = (busy, busy + v[3] + v[4])
    return out


def thermal_zones():
    zones = []
    i = 0
    while os.path.exists(f"/sys/class/thermal/thermal_zone{i}/temp"):
        with open(f"/sys/class/thermal/thermal_zone{i}/temp") as f:
            zones.append(int(f.read()) / 1000)
        i += 1
    return zones


def nvme_temp():
    for d in os.listdir("/sys/class/hwmon"):
        try:
            with open(f"/sys/class/hwmon/{d}/name") as f:
                if f.read().strip() == "nvme":
                    with open(f"/sys/class/hwmon/{d}/temp1_input") as t:
                        return int(t.read()) / 1000
        except OSError:
            pass
    return None


def process_count():
    return sum(1 for d in os.listdir("/proc") if d.isdigit())


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
    q = "utilization.gpu,temperature.gpu,clocks.sm,power.draw,power.draw.instant"
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
    return {"util": vals[0], "temp": vals[1], "sm": vals[2], "power": vals[3],
            "power_instant": vals[4] if len(vals) > 4 else None, "procs": procs}


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

    c0 = cpu_times()
    sample = json.loads(subprocess.run([binary, "--json", "-d", "1"], capture_output=True,
                                       text=True, check=True).stdout)
    c1 = cpu_times()
    zones = thermal_zones()
    nvme = nvme_temp()
    nproc = process_count()
    smi = nvidia_smi()
    m = meminfo()

    def pct(name):
        (b0, t0), (b1, t1) = c0[name], c1[name]
        return 100.0 * (b1 - b0) / (t1 - t0) if t1 > t0 else 0.0

    rows.append(f"{'metric':<24} {'edgetop':>12} {'reference':>12}   result")
    # the reference window is a few ms wider than edgetop's, so allow 1 percentage point
    row("cpu total %", sample["cpu"]["total_pct"], pct("cpu"), 1.0, "pp")
    # per core: USER_HZ=100 gives 1 % steps over 1 s, and the reference window is ~10 % wider than
    # edgetop's, so a core whose load changes within that gap can differ by a few points
    worst = max((abs(c["pct"] - pct(f"cpu{c['id']}")), c["id"]) for c in sample["cpu"]["cores"]
                if c["pct"] is not None)
    row(f"core % (worst: cpu{worst[1]})", next(c["pct"] for c in sample["cpu"]["cores"] if c["id"] == worst[1]),
        pct(f"cpu{worst[1]}"), 5.0, "pp")
    for i, z in enumerate(sample["temp"]["zones"]):
        if i < len(zones) and z["c"] is not None:
            row(f"zone {z['label']} C", z["c"], zones[i], 2.0)
    row("nvme C", sample["temp"]["nvme_c"], nvme, 2.0)
    row("cpu C (max cpu zone)", sample["temp"]["cpu_c"],
        max(zones[i] for i, z in enumerate(sample["temp"]["zones"])
            if z["label"] in ("soc", "c0e", "c0p", "c1e", "c1p")) if zones else None, 2.0)
    row("process count", sample.get("procs") and len(sample["procs"]) and nproc, nproc, 15)

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
        row("gpu power W (avg)", g["power_w"], smi["power"], 1.5)
        row("gpu power W (instant)", g["power_instant_w"], smi["power_instant"], 3.0)
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
