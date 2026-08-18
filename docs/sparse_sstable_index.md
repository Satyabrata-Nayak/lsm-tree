# Phase 9 — Sparse SSTable Index and Block Reads

## Design

SSTables now use format version 2.  An opened table retains only its Bloom
filter, a sparse index, file descriptor, and scalar metadata.  It does not
retain every key/value pair.

```text
Header → Bloom filter → [first key, data-block offset]... → data blocks → CRC32
```

Data blocks target 4 KiB and contain consecutive sorted records.  The sparse
index has one entry per block.  A point lookup first checks the Bloom filter;
on a possible match it binary-searches the sparse index, `pread`s one block,
and binary-searches the records in that block.

Range scans read only the blocks that overlap the requested interval.  Table
merging reads blocks sequentially, so compaction retains the original
newest-sequence-wins behavior without restoring a full in-memory table.

## Integrity and recovery

The whole-table CRC is verified when a table is opened.  The loader also
validates sparse-index ordering and offsets, every data record, global key
ordering, entry count, and maximum sequence number.  Validation is transient:
the records are discarded before the table is returned.  This preserves the
existing corruption-rejection behavior while keeping the steady-state memory
footprint sparse.

The SSTable magic/version changed from `SST1`/1 to `SST2`/2.  Existing version
1 files are intentionally rejected rather than silently interpreted with the
new layout.  A database must be rebuilt or migrated before upgrading from the
old development format.

## Observable metrics

`Stats` and the benchmark JSON reports include:

- `sstable_reads`: on-demand data-block reads after a table is opened;
- `sstable_bytes_read`: bytes fetched by those reads;
- `table_metadata_bytes`: Bloom filters plus sparse indexes retained in RAM.

Use `make bench` for a configurable workload run, or `make bench-sweep` for
the controlled grid. `scripts/compare_v1_v2.py` can compare matching JSON
reports when supplied with a separately captured version-1 result directory.

## Verification

The correctness suite covers 500 ordered keys spread across multiple blocks,
point hits and misses at block boundaries, a multi-block range scan, a later
tombstone, reopen/reload, and proof that retained metadata is smaller than its
SSTable.  The normal crash campaign is also run against the version-2 format.
