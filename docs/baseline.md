# Baseline — untouched upstream verification

Machine & environment baseline for all future comparisons.

## Environment

| Item | Value |
|---|---|
| Machine | Windows 11 laptop, WSL2 |
| WSL distro | Ubuntu-22.04 (kernel `uname -r` via WSL2) |
| Compiler | g++ (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 |
| Make | GNU Make 4.3 |
| Python | 3.10.12 |
| Build flags | `-std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror` |
| Project location | `/mnt/c/Users/nsaty/Desktop/lsm-tree` (Windows NTFS over 9P bridge) |
| Git commit | `4614997` (untouched upstream, plus `.gitignore` only) |

> **Note on writes:** the project lives on `/mnt/c` (NTFS via the 9P protocol).
> fsync-heavy writes are significantly slower here than on a native Linux
> filesystem. Write numbers below are *conservative*; reads are unaffected.
> Copying the tree into the WSL filesystem (`~/lsm-tree`) gives realistic
> write throughput — see the "native-fs rerun" section.

---

## Phase 3 — Correctness suite (`make test`)

```text
g++ -Iinclude -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror  tests/test_lsm.cpp build/lsm.o -o build/test_lsm
./build/test_lsm
4064 assertions passed
```

- Compilation: **success** (no warnings, `-Werror` clean)
- Assertions: **4064 passed**
- Failures: **0**
- Wall time: ~16.4 s (dominated by `fsync` on `/mnt/c`)

## Phase 4 — Crash test (`make crash-test`)

### Default (100 rounds)

```json
{"acknowledged_losses": 0, "acknowledged_writes": 10, "elapsed_seconds": 3.441,
 "max_sstables": 0, "recovered_sequence": 80, "rounds": 100, "seed": 20260728}
```

### 1000 rounds

```json
{"acknowledged_losses": 0, "acknowledged_writes": 10, "elapsed_seconds": 34.793,
 "max_sstables": 0, "recovered_sequence": 764, "rounds": 1000, "seed": 20260728}
```

- **Acknowledged losses: 0** in both runs.
- Crash harness: external SIGKILL at randomized 1–22 ms, 7-byte split writes,
  fresh-process verification against a separately-synced oracle.

## Phase 5 — Benchmark (`make benchmark`)

### Default: 20,000 writes + 20,000 reads

```text
writes=20000 write_ops_per_second=273.241 reads=20000 read_ops_per_second=430209
found=20000 sstables=2 bloom_checks=34301
```

- All 20,000 written keys found (100% read accuracy).
- Write throughput low because every acknowledged write is `fsync`'d onto NTFS
  through the 9P bridge (~273 ops/s). Reads are in-memory/`pread` on page cache
  (~430K ops/s).

### Scale sweep (manual counts)

| Count | writes/s | reads/s | found | sstables | bloom_checks |
|---|---|---|---|---|---|
| 20,000 | 273.2 | 430,209 | 20,000 | 2 | 34,301 |
| 100,000 | 273.5 | 452,653 | 100,000 | 2 | 192,934 |

> All numbers above are our own measurements on this machine. They are not
> comparable to the upstream M2 Pro numbers (different hardware/filesystem).

### Native-filesystem rerun (WSL ext4, `~/lsm-native`)

| Count | writes/s | reads/s | found | sstables |
|---|---|---|---|---|
| 20,000 | 131.9 | 858,936 | 20,000 | 2 |

Observation: on this particular machine the WSL2 ext4 vhdx was *slower* for
fsync-heavy writes than `/mnt/c` (131 vs 273 ops/s) but faster for reads
(859K vs 430K ops/s). Write throughput on both filesystems is dominated by
per-write `fsync`; treat both as "this laptop" numbers rather than absolute
performance claims.

---

## Known upstream boundaries (confirmed)

- Single writer; no concurrent readers.
- Full SSTable entries loaded into RAM (no sparse/block index).
- Size-triggered full-table compaction.
- No background flush/compaction; no snapshots; no block cache.
