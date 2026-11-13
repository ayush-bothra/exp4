#!/usr/bin/env python3
"""Tables and plots for the lab.   usage: python3 analyze.py [results_dir]

Reads characterization.csv, queue_summary.csv and jobs_*.csv written by ./workload and writes
queue_vs_rho.png, service_hist.png and service_qq.png into the same directory.
"""

import csv
import glob
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

d = sys.argv[1] if len(sys.argv) > 1 else "results"
ORDER = ["simd_seq", "simd_rand", "scalar_seq", "scalar_rand"]


def read(path):
    with open(path) as f:
        return list(csv.DictReader(f))


# ---- Phase A: the lab's metric table ------------------------------------------------------
if os.path.exists(f"{d}/characterization.csv"):
    print(
        f"{'workload':18}{'cpu %':>8}{'load1':>8}{'RSS MB':>8}{'time s':>8}"
        f"{'io frac':>9}{'wchar MB':>10}{'disk MB':>9}"
    )
    for r in read(f"{d}/characterization.csv"):
        print(
            f"{r['workload']:18}{float(r['cpu_pct']):8.1f}{float(r['loadavg1']):8.2f}"
            f"{float(r['max_rss_kb']) / 1024:8.1f}{float(r['wall_s']):8.2f}"
            f"{float(r['io_fraction']):9.2f}{float(r['wchar']) / 1e6:10.1f}"
            f"{float(r['write_bytes']) / 1e6:9.1f}"
        )
else:
    print(
        f"(No '{d}/characterization.csv' found, skipping Phase A table)"
    )  # ---- Phase B: waiting time vs utilization, measured vs M/M/1 and M/G/1 --------------------
if not os.path.exists(f"{d}/queue_summary.csv"):
    sys.exit(
        "(no queue_summary.csv here: characterization-only run, nothing more to plot)"
    )
by_cfg = defaultdict(list)
for r in read(f"{d}/queue_summary.csv"):
    by_cfg[r["config"]].append(r)
cfgs = [c for c in ORDER if c in by_cfg]

fig, axes = plt.subplots(2, 2, figsize=(11, 8), squeeze=False)
for ax, name in zip(axes.flat, cfgs):
    rows = by_cfg[name]
    rho = np.array([float(r["rho"]) for r in rows])
    wq = 1e3 * np.array([float(r["wq_meas"]) for r in rows])
    mean_s = np.mean([float(r["mean_S"]) for r in rows])
    cv2 = np.mean([float(r["cv2_S"]) for r in rows])
    x = np.linspace(0.05, 0.97, 200)
    ax.plot(x, 1e3 * x * mean_s / (1 - x), label="M/M/1")
    ax.plot(
        x, 1e3 * x * mean_s * (1 + cv2) / (2 * (1 - x)), label=f"M/G/1 (C²={cv2:.2f})"
    )
    ax.scatter(rho, wq, color="k", zorder=3, label="measured")
    ax.set_title(f"{name}   E[S]={1e3 * mean_s:.2f} ms")
    ax.set_xlabel("utilization ρ (measured)")
    ax.set_ylabel("mean wait Wq (ms)")
    ax.set_xlim(0, 1)
    ax.set_ylim(0, max(wq.max() * 1.5, 1e-3))
    ax.legend()
fig.tight_layout()
fig.savefig(f"{d}/queue_vs_rho.png", dpi=130)

# ---- Service-time distribution vs exponential ---------------------------------------------
service = {}
for name in cfgs:
    vals = []
    for p in glob.glob(f"{d}/jobs_{name}_rho*_rep*.csv"):
        vals += [float(r["finish"]) - float(r["start"]) for r in read(p)]
    service[name] = np.array(vals)

fig, axes = plt.subplots(2, 2, figsize=(11, 8), squeeze=False)
for ax, name in zip(axes.flat, cfgs):
    s = 1e3 * service[name]
    m = s.mean()
    ax.hist(s, bins=60, density=True, alpha=0.6, label="measured S")
    t = np.linspace(0, s.max(), 300)
    ax.plot(t, np.exp(-t / m) / m, label="exponential, same mean")
    ax.set_title(f"{name}   C²={s.var() / m**2:.2f}")
    ax.set_xlabel("service time (ms)")
    ax.legend()
fig.tight_layout()
fig.savefig(f"{d}/service_hist.png", dpi=130)

fig, axes = plt.subplots(2, 2, figsize=(11, 8), squeeze=False)
for ax, name in zip(axes.flat, cfgs):
    s = np.sort(service[name])
    p = (np.arange(len(s)) + 0.5) / len(s)
    ax.scatter(-np.log(1 - p), s / s.mean(), s=4)
    top = max(-np.log(1 - p).max(), (s / s.mean()).max())
    ax.plot([0, top], [0, top], "r--", label="exponential")
    ax.set_title(name)
    ax.set_xlabel("exponential quantile")
    ax.set_ylabel("S / E[S]")
    ax.legend()
fig.tight_layout()
fig.savefig(f"{d}/service_qq.png", dpi=130)
print(
    "wrote",
    ", ".join(
        os.path.join(d, f)
        for f in ("queue_vs_rho.png", "service_hist.png", "service_qq.png")
    ),
)
