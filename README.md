# TSFS — Telemetry-Streaming FUSE Filesystem

A FUSE-based filesystem that explores how far you can push a logging path
before the OS gets in the way: producer-consumer buffering, mmap-based
zero-copy writes, and telemetry-specific compression.

Built incrementally, with every performance number below actually measured
on real hardware — none of it is estimated.

## Architecture

```
Application
   |
   |  write() / mmap()
   v
+-----------------------------+
| TSFS (libfuse, C++)         |
|  getattr / readdir / open   |  <- Step 1: kernel <-> userspace interface
|  create / truncate          |
+-----------------------------+
   |
   v
+-----------------------------+
| RingBuffer (mutex + condvar)|  <- Step 2: producer-consumer decoupling
|  bounded, backpressured     |
+-----------------------------+
   |
   v  (background flusher thread)
+-----------------------------+
| In-memory file storage      |
|  (page-cache backed, so     |  <- Step 3: mmap zero-copy write path
|   mmap() works transparently|
|   without a custom callback)|
+-----------------------------+
   |
   v  (offline compaction, not live path)
+-----------------------------+
| XOR-delta compression       |  <- Step 4: telemetry-specific compression
+-----------------------------+
```

## Step 1 — Foundation (getattr / readdir / open)
Implements the minimum FUSE contract: the kernel asks "what is this path"
(`getattr`), "what's inside this directory" (`readdir`), and "can I open
this" (`open`), plus `create`/`write`/`read`/`truncate` to make it usable.
No real disk I/O — files live in an in-memory `std::map`.

## Step 2 — Producer-Consumer Log Engine
`write()` no longer touches file storage directly. It builds a `LogPacket`
and pushes it into a bounded, thread-safe ring buffer (mutex + 2 condition
variables, MPSC), then returns immediately. One background thread drains
the queue and applies the bytes to storage.

**Design choice:** bounded with backpressure, not unbounded. A slow flush
path can't cause unbounded memory growth — producers block instead.

**Measured (test_ringbuffer.cpp, 8 producer threads, 400,000 packets):**
- Packets pushed/popped: 400,000 / 400,000 — zero loss
- Checksum: exact match — zero corruption
- Throughput: **588,401 packets/sec** *(measured on Grishmank's laptop; sandbox measured 1,052,508/sec — report your own hardware's number)*

**Measured (stress_test.sh, 8 concurrent OS processes appending to one file):**
- 1600/1600 lines — zero lost writes under real concurrent load

## Step 3 — Zero-Copy Write Path (mmap)
Key correction from the original plan: `fuse_operations` has no `mmap`
callback. What actually happens: because `open()` doesn't set the
`direct_io` flag, the kernel's own page cache handles the mapping.
The application writes straight into page-cache-backed memory with
**zero `write()` syscalls**; the driver's `write()` is only invoked later,
in batches, during kernel writeback.

**Measured (1,000,000 double-precision samples, same data, same machine):**
| Path | Real time | User | Sys |
|---|---|---|---|
| `mmap` (zero syscalls) | **0.029s** | 0.007s | 0.009s |
| `write()` (1M syscalls) | **10.513s** | 0.278s | 3.618s |

**~362x wall-clock difference.** Note `real` (10.5s) for the `write()` path
far exceeds `user+sys` (~3.9s) — that gap is time blocked on FUSE's
per-call kernel/userspace round trip through `/dev/fuse`, which is the
actual cost `mmap` eliminates.

## Step 4 — Telemetry-Specific Compression (XOR-delta)
Offline compaction step (not the live write path — see design note below),
inspired by Facebook's Gorilla time-series compression: XOR consecutive
samples' IEEE-754 bit patterns, strip whole leading zero bytes.
**Lossless** — decode reproduces the exact original bits, verified by
bit-exact comparison, not just "looks close."

**Measured (100,000 samples):**
| Dataset | Raw | Encoded | Ratio |
|---|---|---|---|
| Synthetic sensor random-walk | 800,000 B | 693,656 B | 1.15x |
| Real captured telemetry.bin | 800,000 B | 702,000 B | 1.14x |

**Why the ratio is modest, honestly:** this implementation strips zero
bytes, not zero bits. A double's sign+exponent is only 12 bits (1.5
bytes), so byte-granularity truncation reliably saves about 1 byte per
sample. A full bit-packed implementation (variable-length bit fields, as
Gorilla actually does it) would improve this meaningfully, at the cost of
a bit-level reader/writer — a documented next step, not claimed as done.

**Design note on scope:** this compaction runs over a completed log
segment, not live mmap'd pages. Delta/XOR encoding needs values in strict
sequential order; live writes can dirty any page in any order. Real
systems (Prometheus, Gorilla, LSM-trees generally) solve this the same
way — a hot uncompressed buffer for live writes, a background compactor
for cold storage.


## Repo layout
- `tsfs.cpp` — the FUSE driver (Steps 1–3)
- `ring_buffer.h` / `test_ringbuffer.cpp` — Step 2, standalone concurrency proof
- `mmap_writer.cpp` / `write_baseline.cpp` — Step 3, the before/after comparison
- `delta_codec.h` / `delta_test.cpp` — Step 4, compression + correctness proof
- `stress_test.sh` — end-to-end multi-process concurrency test

