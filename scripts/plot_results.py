#!/usr/bin/env python3
"""Generate comparison plots from benchmark sweep results.

Reads every benchmarks/results/<workload>__<sweep>_<value>.json report,
groups runs by workload and tuning dimension (memtable / bloom / compaction),
and writes PNG charts into benchmarks/results/plots/:

  * write_throughput_vs_<dim>.png   (write ops/s vs parameter)
  * read_throughput_vs_<dim>.png    (read ops/s vs parameter)
  * p99_latency_vs_<dim>.png        (write + read p99 latency vs parameter)
"""

import json
import os
import re

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "benchmarks", "results")
PLOTS = os.path.join(RESULTS, "plots")

FILENAME_RE = re.compile(
    r"^(?P<workload>write_heavy|read_heavy|mixed)__"
    r"(?P<dim>memtable|bloom|compaction)_(?P<value>\d+[KM]?)$"
)


def parse_value(text):
    if text.endswith("K"):
        return int(text[:-1]) * 1024
    if text.endswith("M"):
        return int(text[:-1]) * 1024 * 1024
    return int(text)


def load_results():
    runs = []
    for name in sorted(os.listdir(RESULTS)):
        if not name.endswith(".json"):
            continue
        match = FILENAME_RE.match(name[:-5])
        if not match:
            continue
        with open(os.path.join(RESULTS, name), encoding="utf-8") as handle:
            data = json.load(handle)
        runs.append(
            {
                "workload": match.group("workload"),
                "dim": match.group("dim"),
                "value": parse_value(match.group("value")),
                "data": data,
            }
        )
    return runs


def label(value):
    if value >= 1024 * 1024:
        return f"{value // (1024 * 1024)}M"
    if value >= 1024:
        return f"{value // 1024}K"
    return str(value)


def main():
    os.makedirs(PLOTS, exist_ok=True)
    runs = load_results()
    if not runs:
        print("no sweep results found in", RESULTS)
        return 1

    workloads = ["write_heavy", "read_heavy", "mixed"]
    dims = ["memtable", "bloom", "compaction"]

    for dim in dims:
        for workload in workloads:
            points = sorted(
                (r for r in runs if r["dim"] == dim and r["workload"] == workload),
                key=lambda r: r["value"],
            )
            if not points:
                continue
            x = [p["value"] for p in points]
            labels = [label(v) for v in x]

            results = [p["data"]["results"] for p in points]
            write_ops = [r["write_ops_per_second"] for r in results]
            read_ops = [r["read_ops_per_second"] for r in results]
            write_p99 = [r["write_latency_us"]["p99"] for r in results]
            read_p99 = [r["read_latency_us"]["p99"] for r in results]

            fig, axes = plt.subplots(2, 2, figsize=(11, 8))
            fig.suptitle(f"{workload}  (sweep: {dim})", fontsize=13)

            axes[0][0].plot(labels, write_ops, marker="o")
            axes[0][0].set_title("write ops/s")
            axes[0][0].grid(True, alpha=0.3)

            axes[0][1].plot(labels, read_ops, marker="o", color="tab:green")
            axes[0][1].set_title("read ops/s")
            axes[0][1].grid(True, alpha=0.3)

            axes[1][0].plot(labels, write_p99, marker="o", color="tab:red")
            axes[1][0].set_title("write p99 latency (us)")
            axes[1][0].grid(True, alpha=0.3)

            axes[1][1].plot(labels, read_p99, marker="o", color="tab:purple")
            axes[1][1].set_title("read p99 latency (us)")
            axes[1][1].grid(True, alpha=0.3)

            for axis in axes.flat:
                axis.set_xlabel(dim)
            fig.tight_layout(rect=(0, 0, 1, 0.95))
            fig.savefig(os.path.join(PLOTS, f"{workload}__{dim}.png"), dpi=110)
            plt.close(fig)

    print(f"plots written to {PLOTS}")


if __name__ == "__main__":
    raise SystemExit(main())
