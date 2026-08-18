#!/usr/bin/env python3
"""Compare v1 (build/v1_results) and v2 (benchmarks/results) sweep results.

Usage: python3 scripts/compare_v1_v2.py [--dir-v1 build/v1_results]
"""
import argparse
import json
import os

METRICS = [
    ("total_ops_per_second", "total_ops/s"),
    ("read_ops_per_second", "read_ops/s"),
    ("read_p99_us", "read_p99_us"),
    ("sstable_count", "sstables"),
    ("bloom_checks", "bloom_checks"),
    ("bloom_negative_hits", "bloom_neg"),
    ("sstable_reads", "sstable_reads"),
    ("sstable_bytes_read", "sstable_bytes_read"),
    ("table_metadata_bytes", "metadata_bytes"),
    ("database_size_bytes", "db_bytes"),
]


def load(path):
    with open(path) as handle:
        return json.load(handle)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir-v1", default="build/v1_results")
    parser.add_argument("--dir-v2", default="benchmarks/results")
    parser.add_argument("--detail", metavar="RUN", help="print raw v1/v2 values for one run")
    args = parser.parse_args()

    if args.detail:
        v1 = load(os.path.join(args.dir_v1, args.detail))["results"]
        v2 = load(os.path.join(args.dir_v2, args.detail))["results"]
        for key, label in METRICS:
            print(f"{label:<22} v1={v1.get(key, 0):>14}  v2={v2.get(key, 0):>14}")
        return

    v1_files = sorted(
        name for name in os.listdir(args.dir_v1) if name.endswith(".json")
    )
    v2_files = sorted(
        name for name in os.listdir(args.dir_v2) if name.endswith(".json")
    )
    common = sorted(set(v1_files) & set(v2_files))
    print(f"v1 files: {len(v1_files)}  v2 files: {len(v2_files)}  common: {len(common)}")

    header = f"{'run':<52}" + "".join(f"{label:>14}" for _, label in METRICS)
    print(header)
    for name in common:
        v1 = load(os.path.join(args.dir_v1, name))["results"]
        v2 = load(os.path.join(args.dir_v2, name))["results"]
        row = f"{name:<52}"
        for key, _ in METRICS:
            old = v1.get(key, 0)
            new = v2.get(key, 0)
            if old == 0 and new == 0:
                row += f"{'0':>14}"
            elif old == 0:
                row += f"{'new':>14}"
            else:
                delta = (new - old) / old * 100.0
                row += f"{delta:>+13.1f}%"
        print(row)


if __name__ == "__main__":
    main()