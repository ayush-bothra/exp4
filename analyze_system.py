#!/usr/bin/env python3
"""Join vmstat / iostat / top logs with markers.csv.      usage: python3 analyze_system.py <dir> [device]

Prints and saves one row per measured interval (idle, cpu_simd, io_seq, ...) and plots a timeline.
Each tool's first report is since boot, so it is dropped. A sample stamped t describes (t-interval, t],
and only samples whose whole interval lies inside a marker are used.
"""
import csv
import os
import re
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

d = sys.argv[1] if len(sys.argv) > 1 else "mon"
want_dev = sys.argv[2] if len(sys.argv) > 2 else None


def num(x):
    try:
        return float(x)
    except ValueError:
        return None


# ---- vmstat ---------------------------------------------------------------------------------
vm = []                      # (epoch, {column: value})
cols = None
for line in open(f"{d}/vmstat.log"):
    t = line.split()
    if len(t) < 3:
        continue
    if t[1] == "r" and t[2] == "b":
        cols = t[1:]
    elif cols and len(t) == len(cols) + 1 and all(num(x) is not None for x in t):
        vm.append((float(t[0]), dict(zip(cols, map(float, t[1:])))))
vm = vm[1:]                  # drop since-boot row

# ---- iostat ---------------------------------------------------------------------------------
io = defaultdict(list)       # device -> [(epoch, {column: value})]
dev_cols, report = None, -1
for line in open(f"{d}/iostat.log"):
    t = line.split()
    if len(t) < 2:
        continue
    if t[1] == "avg-cpu:":
        report += 1
    elif t[1] == "Device":
        dev_cols = t[2:]
    elif dev_cols and report >= 1 and len(t) == len(dev_cols) + 2 and num(t[2]) is not None:
        io[t[1]].append((float(t[0]), dict(zip(dev_cols, map(float, t[2:])))))

# ---- top ------------------------------------------------------------------------------------
tp = []                      # (epoch, {"load1":..,"workload_cpu":..})
cur, it = None, -1
for line in open(f"{d}/top.log"):
    t = line.split()
    if len(t) < 2:
        continue
    if t[1] == "top":
        it += 1
        cur = {"load1": float(re.search(r"load average: ([\d.]+)", line).group(1)), "workload_cpu": 0.0}
        if it >= 1:
            tp.append((float(t[0]), cur))
    elif cur is not None and len(t) >= 13 and t[12] == "workload" and num(t[9]) is not None:
        cur["workload_cpu"] = float(t[9])

# ---- markers --------------------------------------------------------------------------------
marks = []
with open(f"{d}/markers.csv") as f:
    for r in csv.DictReader(f):
        marks.append((r["phase"], r["label"], float(r["rho"]), int(r["rep"]),
                      float(r["t_start"]), float(r["t_end"])))
marks.sort(key=lambda m: m[4])
if not vm or not marks:
    sys.exit("no vmstat samples or no markers found")

interval = float(np.median(np.diff([e for e, _ in vm]))) if len(vm) > 1 else 1.0
ncpu = os.cpu_count() or 1


def pick_device():
    if want_dev:
        return want_dev
    lo, hi = marks[0][4], marks[-1][5]
    tot = {k: sum(v["rkB/s"] + v["wkB/s"] for e, v in rows if lo <= e <= hi) for k, rows in io.items()}
    return max(tot, key=tot.get)


dev = pick_device()
print(f"device: {dev}   cpus: {ncpu}   sample interval: {interval:.2f}s   (first report of each tool dropped)")


def inside(series, t0, t1):
    return [v for e, v in series if e - interval >= t0 - 0.05 and e <= t1 + 0.05]


def mean(rows, key):
    xs = [r[key] for r in rows if key in r]
    return float(np.mean(xs)) if xs else float("nan")


table = []
for phase, label, rho, rep, t0, t1 in marks:
    v, s, p = inside(vm, t0, t1), inside(io[dev], t0, t1), inside(tp, t0, t1)
    row = {
        "phase": phase, "label": label, "rho": rho, "rep": rep, "exec_s": t1 - t0, "samples": len(v),
        "cpu_user": mean(v, "us"), "cpu_sys": mean(v, "sy"), "cpu_idle": mean(v, "id"), "cpu_iowait": mean(v, "wa"),
        "busy_cores": (mean(v, "us") + mean(v, "sy")) / 100 * ncpu,
        "runnable": mean(v, "r"), "blocked": mean(v, "b"),
        "disk_util": mean(s, "%util"), "disk_idle": 100 - mean(s, "%util"), "disk_util_max": max([x["%util"] for x in s], default=float("nan")),
        "w_MBps": mean(s, "wkB/s") / 1024, "r_MBps": mean(s, "rkB/s") / 1024,
        "w_await_ms": mean(s, "w_await"), "r_await_ms": mean(s, "r_await"), "aqu_sz": mean(s, "aqu-sz"),
        "load1": mean(p, "load1"), "workload_cpu_top": mean(p, "workload_cpu"),
    }
    table.append(row)

with open(f"{d}/system_phase_table.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(table[0]))
    w.writeheader()
    w.writerows(table)

hdr = ("label", "rho", "exec_s", "samples", "cpu_idle", "cpu_iowait", "busy_cores", "disk_util", "disk_idle", "w_MBps", "w_await_ms", "aqu_sz", "load1")
print("".join(f"{h:>12}" for h in hdr))
for r in table:
    if r["phase"] == "queue" and r["rep"] != 0:
        continue     # keep the printed table short; the CSV has everything
    print("".join(f"{r[h]:>12.2f}" if isinstance(r[h], float) else f"{r[h]:>12}" for h in hdr))
if any(r["samples"] < 3 for r in table):
    print("warning: some intervals have fewer than 3 samples; run them longer (char_secs=) for stable means")

# ---- timeline -------------------------------------------------------------------------------
t_lo, t_hi = marks[0][4] - 5 * interval, marks[-1][5] + 5 * interval
x = lambda rows: np.array([e for e, _ in rows if t_lo <= e <= t_hi]) - t_lo - interval / 2  # plot at interval midpoint
y = lambda rows, k: np.array([v[k] for e, v in rows if t_lo <= e <= t_hi])

fig, ax = plt.subplots(4, 1, figsize=(12, 10), sharex=True)
ax[0].plot(x(vm), y(vm, "us") + y(vm, "sy"), label="user+sys")
ax[0].plot(x(vm), y(vm, "wa"), label="iowait")
ax[0].plot(x(vm), y(vm, "id"), label="idle")
ax[0].set_ylabel("CPU % (all cores)")
ax[1].plot(x(vm), y(vm, "r"), label="runnable (r)")
ax[1].plot(x(vm), y(vm, "b"), label="blocked (b)")
ax[1].set_ylabel("processes")
ax[2].plot(x(io[dev]), y(io[dev], "%util"), label=f"{dev} %util")
ax[2].set_ylabel("disk % util")
ax[3].plot(x(io[dev]), y(io[dev], "wkB/s") / 1024, label="write MB/s")
ax[3].set_ylabel("write MB/s")
twin = ax[3].twinx()
twin.plot(x(io[dev]), y(io[dev], "aqu-sz"), color="tab:red", label="aqu-sz")
twin.set_ylabel("avg queue size", color="tab:red")
ax[3].set_xlabel("seconds")
for a in ax:
    a.legend(loc="upper right")
    for phase, label, rho, rep, t0, t1 in marks:
        if phase == "char" or rep == 0:
            a.axvspan(t0 - t_lo, t1 - t_lo, alpha=0.12, color="gray")
for phase, label, rho, rep, t0, t1 in marks:
    if phase == "char":
        ax[0].text((t0 + t1) / 2 - t_lo, 103, label, rotation=45, fontsize=7, ha="left")
ax[0].set_ylim(0, 115)
fig.tight_layout()
fig.savefig(f"{d}/system_timeline.png", dpi=130)
print(f"wrote {d}/system_phase_table.csv and {d}/system_timeline.png")
