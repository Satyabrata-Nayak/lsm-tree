#!/usr/bin/env python3
"""Print a compact summary table of benchmark sweep results.

Usage: python3 scripts/summarize_results.py [workload]
Workload filter is optional (write_heavy | read_heavy | mixed).
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "benchmarks", "results")

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


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    rows = []
    for name in sorted(os.listdir(RESULTS)):
        if not name.endswith(".json"):
            continue
        match = FILENAME_RE.match(name[:-5])
        if not match:
            continue
        workload = match.group("workload")
        if only and workload != only:
            continue
        with open(os.path.join(RESULTS, name), encoding="utf-8") as handle:
            data = json.load(handle)
        r = data["results"]
        rows.append(
            (
                workload,
                match.group("dim"),
                parse_value(match.group("value")),
                r["total_ops_per_second"],
                r["write_ops_per_second"],
                r["read_ops_per_second"],
                r["read_hit_rate"],
                r["write_latency_us"]["p99"],
                r["read_latency_us"]["p99"],
                r["sstable_count"],
                r["bloom_checks"],
                r["bloom_negative_hits"],
                r["database_size_bytes"],
            )
        )

    header = (
        f"{'workload':<12}{'dim':<11}{'value':<8}{'tot_ops/s':>10}"
        f"{'wr_ops/s':>10}{'rd_ops/s':>10}{'hit%':>7}{'wr_p99':>9}"
        f"{'rd_p99':>9}{'sst':>5}{'bloom_chk':>10}{'bloom_neg':>10}"
        f"{'db_bytes':>12}"
    )
    print(header)
    print("-" * len(header))
    for row in rows:
        value = row[2]
        label = (
            f"{value // (1024 * 1024)}M"
            if value >= 1024 * 1024
            else f"{value // 1024}K"
            if value >= 1024
            else str(value)
        )
        print(
            f"{row[0]:<12}{row[1]:<11}{label:<8}{row[3]:>10.1f}"
            f"{row[4]:>10.1f}{row[5]:>10.1f}{row[6] * 100:>6.1f}%"
            f"{row[7]:>9.1f}{row[8]:>9.1f}{row[9]:>5}{row[10]:>10}"
            f"{row[11]:>10}{row[12]:>12}"
        )


if __name__ == "__main__":
    raise SystemExit(main())