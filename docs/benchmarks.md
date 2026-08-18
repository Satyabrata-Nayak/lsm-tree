# Benchmark Framework & Results (Phase 8)

This document describes the benchmark framework added in Phase 8 and summarizes
the controlled experiment sweep.

## Framework

```
benchmarks/
├── benchmark_driver.cpp      # configurable workload runner (JSON output)
├── workloads/
│   ├── write_heavy.conf      # 90% writes, 100k key space, no preload
│   ├── read_heavy.conf       # 10% writes, 100k key space, 10k preload
│   └── mixed.conf            # 50/50, 100k key space, 5k preload
└── results/                  # per-run JSON reports + plots/
```

* `make bench` — build the driver.
* `make bench-sweep` — run the full 33-run sweep (see `scripts/run_experiments.sh`).
* `scripts/plot_results.py` — render PNG charts from the JSON reports.
* `scripts/summarize_results.py` — print a compact table of all runs.

### Metrics collected per run

| Metric | Meaning |
| --- | --- |
| `total_ops_per_second` | end-to-end throughput (all ops) |
| `write_ops_per_second` / `read_ops_per_second` | per-type throughput |
| `write_latency_us` / `read_latency_us` | p50 / p95 / p99 latency |
| `read_hit_rate` | fraction of reads served from the memtable |
| `sstable_count` | number of on-disk tables at the end of the run |
| `bloom_checks` | bloom filter probes during reads |
| `bloom_negative_hits` | probes answered "definitely absent" by the bloom filter |
| `database_size_bytes` | total on-disk size |

### Experiment grid (33 runs)

* MemTable size: 64 KB, 256 KB, 1 MB, 4 MB
* Bloom filter: 4, 8, 10, 14 bits/key
* Compaction trigger: 2, 4, 8

Each run: 5000 operations, seeded RNG (seed 20260728), uniform key draw from a
100k key space, `sync_writes=true`. Runs execute on the WSL filesystem bridge
(NTFS via 9P), so fsync-bound write latency is dominated by the bridge.

## Findings

### Bloom filter bits/key (read_heavy)

More bits per key monotonically reduces false positives:

| bits/key | bloom checks | bloom negatives | read p99 (us) |
| --- | --- | --- | --- |
| 4 | 11419 | 9249 | 8.1 |
| 8 | 11419 | 10747 | 7.8 |
| 10 | 11419 | 10930 | 7.6 |
| 14 | 11419 | 10999 | 6.5 |

With 10k preloaded keys in a 100k key space, ~96% of reads miss; the bloom
filter rejects most of them without touching disk. 10 bits/key is a good
default (diminishing returns beyond it).

### Compaction trigger

Higher trigger keeps more SSTables around, which increases per-read work:

| trigger | sstables | bloom checks | read p99 (us) |
| --- | --- | --- | --- |
| 2 | 1 | 4462 | 6.9 |
| 4 | 1 | 11419 | 7.5 |
| 8 | 5 | 18653 | 7.3 |

Trigger 4 balances write amplification against read amplification for this
workload mix.

### MemTable size

Larger memtables flush less often, so fewer SSTables exist and fewer bloom
probes happen per read:

| memtable | sstables | bloom checks | write p99 (us) |
| --- | --- | --- | --- |
| 64 KB | 1 | 11419 | 5543 |
| 256 KB | 1 | 4417 | 6375 |
| 1 MB | 1 | 4431 | 20851 |
| 4 MB | 0 | 0 | 7712 |

256 KB–1 MB is the sweet spot on this hardware; 4 MB keeps everything in memory
for short runs (no flush until close).

### Workload character

* **write_heavy** (~250–520 ops/s): fsync-bound; throughput is dominated by the
  slow 9P filesystem bridge, not by LSM internals.
* **read_heavy** (~2400 ops/s): reads are ~1–10 us; the bloom filter is the
  main lever for read cost.
* **mixed** (~500 ops/s): balanced; write cost dominates.

## Reproducing

```bash
make bench-sweep            # full 33-run sweep (~15 min)
python3 scripts/plot_results.py
python3 scripts/summarize_results.py
```

Plots are written to `benchmarks/results/plots/`.