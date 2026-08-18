# Concurrent Readers

MiniLSM supports a single writer alongside multiple concurrent readers.

`LSMTree` uses a `std::shared_mutex` for database state:

- `get`, `scan`, and `stats` use a shared lock, so any number of readers can
  access the immutable SSTables and MemTable concurrently.
- `put`, `erase`, `flush`, and `compact` use the exclusive lock. Each mutation
  is therefore atomic with respect to readers and the existing single-writer
  WAL/recovery model remains unchanged.
- Read statistics use a separate mutex, avoiding data races between readers
  without serializing the read path on the database-state lock.

This is deliberately a correctness-first design. A synchronous writer blocks
new readers while its WAL append or flush is in progress; background flushes,
snapshots, and lock-free version publication remain out of scope.

## Verification and measurement

The correctness suite runs four readers against one writer that performs 600
updates, automatic flushes, and compactions. Readers repeatedly perform point
lookups and range scans over stable keys.

Run the scaling benchmark on a machine with a native WSL filesystem for the
most representative numbers:

```bash
make reader-bench
```

It creates one 20,000-key SSTable, verifies every read, and reports total
throughput and p50/p95/p99 latency for 1, 2, 4, 8, and 16 readers.
