# kvstore-cpp

A small, crash-safe, thread-safe key-value storage engine in C++20, in the style of
[Bitcask](https://riak.com/assets/bitcask-intro.pdf): an append-only log on disk plus an
in-memory hash index. About 800 lines of library code, no dependencies (GoogleTest is
fetched for tests).

```cpp
auto db = kv::DB::open("/tmp/mydb");          // recovers whatever is on disk
db->put("user:1", "alice");
std::optional<std::string> v = db->get("user:1");
db->del("user:1");
db->compact();                                 // reclaim space from old versions
```

## On-disk format

A database is a directory of numbered segment files (`0000000001.data`, `0000000002.data`, ...).
Only the highest-numbered one is written to; the others are sealed and immutable.
Each segment is a sequence of records:

```
+-----------+-------------+-------------+----------+-----------+-------------+
| crc32 (4) | key_len (4) | val_len (4) | flags(1) | key bytes | value bytes |
+-----------+-------------+-------------+----------+-----------+-------------+
```

- Integers are little-endian. `crc32` (IEEE polynomial, implemented in `src/crc32.cpp`)
  covers everything after the crc field.
- `flags & 1` marks a tombstone (a delete). Tombstones have an empty value.
- The index maps `key -> (segment id, offset, record size)`, so a read is one `pread`.

## Durability

`Options::sync_mode` picks the tradeoff:

- `kEveryWrite` (default): `put`/`del` return only after `fdatasync`. Concurrent writers
  share fsyncs (group commit): the first waiting writer syncs once on behalf of every record
  appended so far, the rest wait on a condition variable.
- `kBatch`: `fdatasync` every `batch_sync_bytes` (1 MiB default), on `sync()`, on segment
  rollover and on close. A crash can lose the last unsynced writes, but never corrupts
  older data.

If a write or fsync fails, the DB refuses further writes until it is reopened, because the
state of the log tail is unknown at that point (re-trying fsync after a failure is not safe
on Linux).

## Crash recovery

On open, every segment is scanned in order and the index is rebuilt. A record is accepted
only if its header lengths are in range, it fits in the file, and its CRC matches.

- In the **newest** segment, the first bad record is treated as a torn write from a crash.
  The file is truncated there and fsynced. Every acknowledged write is before that point.
- In an **older** segment, a bad record cannot be a torn write (segments are fsynced before
  they are sealed), so `open` throws `CorruptionError` rather than silently losing data from
  the middle of the log.
- `get` re-checks the CRC of the record it reads, so corruption after open is reported
  (`CorruptionError`) instead of returning wrong bytes.

An exclusive `flock` on `LOCK` stops two processes from opening the same directory.

## Compaction

Overwrites and deletes leave dead records behind. `compact()`:

1. Rolls to a new active segment, so every older segment is sealed.
2. Copies the records the index still points to (in file order) into `NNN.compact.tmp`,
   where `NNN` is the newest sealed segment id, then fsyncs it.
3. **Commit point:** renames it to `NNN.compact` and fsyncs the directory.
4. Repoints the index, deletes the old segments, renames `NNN.compact` to `NNN.data`,
   fsyncs the directory.

Tombstones and overwritten values are dropped: every segment older than the active one is
rewritten at once, so nothing older remains for a dropped tombstone to have been hiding.

Crash safety comes from file names, checked on open:

- `*.tmp` present: the compaction had not committed. Delete it; old segments are intact.
- `NNN.compact` present: it had committed. Delete segments `<= NNN` and finish the rename.

Reads and writes continue during compaction. Writes go to the new active segment; when the
index is repointed, a key is only moved if it still points at the record that was copied,
so a concurrent overwrite or delete is never undone.

## Concurrency model

- `index_mu_` (`std::shared_mutex`) guards the index. Readers take it shared just long
  enough to copy a location and a `shared_ptr` to the file, then release it and `pread`
  without any lock. `pread`/`pwrite` take explicit offsets, so threads never share a file
  position.
- `write_mu_` serializes appends. A writer holds it for the `pwrite` and index update only,
  not for the fsync (group commit happens outside it).
- `compact_mu_` allows one compaction at a time.
- Lock order is `compact_mu_ -> write_mu_ -> index_mu_ / sync_mu_`.
- A reader holding a `shared_ptr<File>` keeps the descriptor open even if compaction
  unlinks that segment, so reads never race with file deletion.

## Limitations and tradeoffs

- Every key must fit in memory (the Bitcask tradeoff: one disk read per lookup, but RAM
  grows with key count). No range scans; the index is a hash map.
- Recovery reads every segment, so open time grows with data size. Bitcask's "hint files"
  would fix this; not implemented.
- Compaction rewrites all sealed segments every time, rather than choosing the ones with
  the most garbage. It also needs free disk space for the live data while it runs.
- One writer at a time; write throughput is bounded by a single append stream.
- Max key 64 KiB, max value 64 MiB. Values are returned by copy.

## Build, test, benchmark

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
(cd build && ctest --output-on-failure)

# sanitizers
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DKV_SANITIZE=thread   # ThreadSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DKV_SANITIZE=address  # ASan + UBSan

./build/kvcli /tmp/db put hello world
./build/kvcli /tmp/db get hello
./build/kvcli /tmp/db del hello
./build/kvcli /tmp/db compact
./build/kvbench --threads 4 --ops 200000 --sync batch
```

Everything builds with `-Wall -Wextra -Werror`. CI runs the tests in Release, TSan and
ASan+UBSan builds.

The tests cover round trips, overwrites, deletes, reopen, torn writes at every byte
offset of the last record, a zero-filled tail, flipped bytes (in the tail, in a sealed
segment, and after open), compaction (latest values kept, tombstones and old versions
dropped, size reduced), crashes before and after the compaction commit point, and
multithreaded stress with readers checking that values never go backwards, including
with compaction running concurrently.

## Benchmark results

Measured on an Intel i5-9500T (6 cores, 2.2 GHz), ext4 on NVMe, Release build, 100-byte
values, 13-byte keys, 100,000-key space. Each thread does `--ops` puts then `--ops` random
gets. Latencies are per operation.

| command | put ops/sec | put p50 / p99 | get ops/sec | get p50 / p99 |
|---|---:|---:|---:|---:|
| `kvbench --threads 1 --ops 200000 --sync batch` | 350,628 | 1.6 / 4.6 us | 653,263 | 1.5 / 1.9 us |
| `kvbench --threads 4 --ops 200000 --sync batch` | 284,998 | 2.5 / 26.2 us | 2,040,681 | 1.9 / 2.4 us |
| `kvbench --threads 1 --ops 2000 --sync every` | 184 | 5205 / 8781 us | 681,286 | 1.3 / 1.7 us |
| `kvbench --threads 4 --ops 2000 --sync every` | 454 | 9802 / 14413 us | 2,173,226 | 1.6 / 2.3 us |
| `kvbench --threads 16 --ops 1000 --sync every` | 1,666 | 10347 / 14855 us | 2,572,746 | 1.8 / 2.5 us |

What the numbers show:

- Reads scale with threads (about 3x from 1 to 4 threads) because readers only share a
  lock briefly and then `pread` in parallel. Data fits in the page cache, so these are
  memory-speed reads plus a syscall.
- Batched writes do not scale: there is one writer lock, and with more threads they
  contend for it.
- With an fsync per write, the cost is the ~5 ms `fdatasync` on this disk. Group commit
  lets concurrent writers share it: 16 threads get about 9x the single-thread throughput.

## License

MIT
