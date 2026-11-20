# Cross-library benchmark: findings

Summary of repeated runs on this machine (Intel Core Ultra 5 125H, MSVC 19.51,
4 worker threads). Raw data lives in the `results_*` directories; each
`env.txt` records its own conditions.

Read the **methodology caveats** first — several early conclusions in this
project turned out to be measurement artefacts, and two of them were published
before being retracted. They are recorded here so they are not rediscovered.

## Methodology caveats

1. **Cross-run absolute values are not comparable.** zrlog's p50 measured
   24.7 / 28.7 / 30.7 / 35.4 / 39.4 ns across runs with *identical code*.
   Only rankings within a single run are meaningful. Use ≥9 rounds and take
   the median.
2. **System load matters more than any code change.** A VirtualBox VM running
   in the background moved zrlog throughput from ~377 to ~285 M/s; the
   untouched baseline commit moved identically, proving it environmental.
3. **Single-sample outliers can dominate `avg`.** One 3.7 ms sample in 200k
   raises `avg` by ~19 ns. If `avg` exceeds `p99.9`, treat the run as
   contaminated (this is how quill's queue-reallocation problem was found).
4. **Latency here is the frontend call only** (the code between two `rdtsc`
   pairs around one log statement), not end-to-end log-to-disk latency.

### Binding and warm-up (added after the E-core discovery)

5. **`BENCH_CPUS=2,3,4,5` is WRONG on this machine.** Those are E/LPE cores.
   `GetLogicalProcessorInformationEx` reports the P-cores as `0/1`, `10/11`,
   `12/13`, `14/15` (four physical cores with SMT) and the E/LPE cores as
   `2,3,4,5,6,7,8,9,16,17`. All early four-library comparisons were therefore
   measured on E-cores, which understates absolute performance for every
   library. `run_win.ps1` now auto-detects P-cores by `EfficiencyClass` and
   picks one logical CPU per physical core (`0,10,12,14` here).
6. **A warm-up burn is mandatory for zrlog.** Without it, zrlog's p50 lands in
   either ~20 ns or ~50 ns with no code change; with `BENCH_BURN_MS=100` it
   lands at 19.4-20.0 ns in 11/11 rounds. quill and fmtlog are unaffected.
   Threshold is between 20 and 50 ms. `run_win.ps1 -BurnMs 100` is now the
   default. This is still not fully explained — see "Open questions".

## Stable conclusions

Verified across many runs since the binding/warm-up fix; each held every time.

### Latency (ns, median of 9-11 rounds, P-cores, 100 ms burn-in)

| lib | avg | min | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| quill | 14.8 | 13.4 | **14.7** | 15.4 | **16.7** | **30.0** |
| fmtlog | 17.9 | 15.4 | 16.0 | 16.7 | 82.8 | 96.8 |
| zrlog | 20.4 | 18.0 | 20.0 | 20.7 | 28.0 | 40.1 |
| spdlog | 402.6 | 156.3 | 217.0 | 936.2 | 2330.4 | 6545.2 |

- **p50**: quill < fmtlog < zrlog < spdlog. zrlog is 5.3 ns behind quill and
  4.0 ns behind fmtlog.
- **Tail**: quill < **zrlog** < fmtlog < spdlog. zrlog's p99 is **2.9x better
  than fmtlog's** despite a worse p50 — fmtlog's queue handling degrades under
  bursts.
- **Tail shape**: zrlog p50→p99.9 ratio is 2.0x, quill 2.04x, fmtlog 6.05x.
  zrlog's distribution is as well-behaved as quill's; fmtlog's is not.
- **Run-to-run stability of p50**: zrlog 19.4-20.0 (0.6 ns spread over 11
  rounds) was the tightest of the three. quill and fmtlog each had one round
  at 20.7-22.7 among otherwise-flat runs.

### Throughput (M logs/s, median of 9-11 rounds)

| lib | median | range | notes |
|---|---|---|---|
| zrlog | 296.7 | 266 - 376 | Discard: this is the *attempt* rate, logs are dropped |
| fmtlog | 221.3 | 151 - 232 | Discard (FMTLOG_BLOCK=0): also an attempt rate |
| quill | 131.0 | 121 - 132 | Unbounded queue: every log accepted |
| spdlog | 2.31 | 1.25 - 2.37 | overrun_oldest |

**Throughput is not comparable across libraries** because the drop policies
differ. zrlog and fmtlog both discard when full, so their numbers are upper
bounds on accepted logs, not processing rates. Only quill's number is
"everything was processed". Do not quote "zrlog is 2.3x quill" as a speed
claim.

### Queue capacity does not explain fmtlog's lower throughput

fmtlog's default queue is **1 MB** (`FMTLOG_QUEUE_SIZE = 1<<20`, 131072
8-byte blocks), not unbounded. Raising it to 8 MB to match zrlog's thread
buffer changed nothing measurable:

| fmtlog queue | p50 | p99 | p99.9 | throughput |
|---|---|---|---|---|
| 1 MB (default) | 16.0 | 80.8 | 93.5 | 222.3 |
| 8 MB | 16.0 | 82.8 | 96.8 | 221.3 |

So fmtlog's queue is rarely full; its throughput is a real processing rate,
not a drop artefact. **An earlier hypothesis ("fmtlog drops more because its
queue is smaller") was wrong and is retracted.**

`bench_fmtlog.cpp` now sets the queue to 8 MB via
`-DFMTLOG_BENCH_QUEUE_MB=8` **on the compiler command line**, not in the
source file: `fmtlog.cc` is a separate translation unit that does
`new fmtlog::ThreadBuffer()`, while the bench TU inlines `alloc()` against
`BLK_CNT`. Defining the macro in only one TU is an ODR violation and a heap
overflow.

## What the code differences actually are

### Frontend hot path

zrlog per log statement, after serialisation:

```cpp
if (ZRLOG_LIKELY(config_.notify_consumer_per_log)) notify_consumer();
if (ZRLOG_LIKELY(config_.producer_stats))          note_produced(buffer, true);
```

- `notify_consumer()` reads the shared `idle_wait_flag_` — a cache line the
  consumer writes from another core on every idle cycle.
- `note_produced()` does two increments plus a `& 1023` test.
- Both are gated on `config_` fields read from the singleton each call.

fmtlog has neither. quill has neither. This, plus `alloc`'s cursor handling,
is the most plausible source of zrlog's +4-5 ns p50.

Disabling both was measured: it lowered the fast mode from ~28 to ~20 ns but
did not remove the 20↔50 bimodality. Disabling only `notify_consumer()` made
things **worse** (p99.9 90.8 → 223.0) because the consumer then only wakes on
the 1 ms idle timeout and drains in bursts. **Do not remove the notification**
— it is a net positive; the p50 cost is the price of a smooth tail.

### Queue design

fmtlog's `SPSCVarQueueOPT` allocates in 8-byte blocks (`blk_sz`), so the
producer does no ring arithmetic and no wrap-around branch on the fast path.
zrlog's `alloc` computes `phys_w` / `tail_free` and handles wrap with a
padding entry. At 8 MB this happens once per 8 MB written, so it is not a
measurable cost in these tests.

### Cache-line layout

Measured with a mirror struct (MSVC, `hardware_destructive_interference_size`
= 64):

| field | old offset | new offset |
|---|---|---|
| `write_index_` | 0 | 0 (`alignas(128)`) |
| `cached_read_index_` | 64 | 8 |
| `produce_attempts_` | 128 | 16 |
| `read_index_` | 192 | **128** (`alignas(128)`) |
| `cached_write_index_` | 256 | 136 |
| `local_read_index_` | 320 | 144 |
| `size_` | 384 | 256 |
| `should_deallocate_` | 424 | 320 |
| `sizeof` | 448 | **384** |

The change was motivated by Intel's adjacent-cache-line prefetcher, which
pulls 128-byte *pairs* — so 64-byte separation between `write_index_` and
`read_index_` does not actually prevent cross-core ownership transfers.
fmtlog independently does the same thing (`alignas(128)` on `read_idx`).

**Measured effect: none.** 8 interleaved A/B rounds at 8 MB gave the same p50
median for both layouts, and an instrumented build showed
`slow_alloc = 0` over 440,000 logs — the ring never came close to full, so the
contended path was never taken. The change is kept because it matches Intel's
guidance, matches fmtlog's own choice, and makes the struct 64 bytes smaller —
not because it was shown to help.

## Retracted claims

Recorded so they are not repeated:

1. **"Disabling `notify_consumer()` improves tail latency."** False. Measured
   the opposite: p50 31.4 → 44.1, p99.9 90.8 → 223.0, max 10.2 µs → 46.0 µs.
   The notification keeps the consumer draining smoothly; without it the
   consumer wakes only on the 1 ms timeout and drains in bursts that fight the
   producer for cache lines.
2. **"Slow mode is caused by ring-buffer backpressure."** False. The
   instrumented build recorded `slow_alloc = 0` and `full_fail = 0` across
   440,000 logs. The ring was never full.
3. **"Slow mode is caused by the consumer sharing the producer's core or
   SMT sibling."** Not supported. Pinning the consumer to the producer's SMT
   sibling (CPU 1) was among the *fastest* configurations.
4. **"fmtlog's lower throughput is because its 1 MB queue drops more."**
   False. Raising it to 8 MB changed nothing.

## Open questions

- **Why does a 20-50 ms CPU burn remove zrlog's bimodal latency?** A pure ALU
  burn, with no logging involved, moves zrlog from ~50 ns to ~20 ns and holds
  it there for 11/11 rounds. quill and fmtlog are unaffected. Candidates:
  P-state ramp interacting with frontend code volume; first-touch cost on the
  384-byte `ThreadBuffer` struct; TSC-vs-core-frequency ratio during ramp.
  Not distinguished by any experiment so far. **The bimodality is suppressed,
  not explained, and it reappeared in one later 5-round session
  (p50 34.1 / 32.1 / 27.4) — treat "fixed" as unproven.**
- zrlog's p50 gap to quill (5.3 ns) has not been reduced by any change tried.
  Candidate work: sink the `config_` reads into per-`ThreadBuffer` copies;
  localise the producer cursor; pad the shared statistics counters. None of
  these has been measured yet.

## Per-library notes

- **quill** runs with a tuned configuration (32 MB queue, ordering grace 0).
  Its stock configuration is worse on `avg`/`max` (queue reallocations) but
  its p50 advantage survives: see `results_win_quill_default/env.txt`.
- **fmtlog** has a large p99 (78-111 ns) despite a good p50; raising the queue
  to 8 MB did not change it. Its backend polls every 1 ms
  (`startPollingThread(1000000)`), which is the most likely cause: the
  producer can fill a large part of the queue between polls, and the burst
  drain then contends with the producer.
- **spdlog** is used in async mode with `overrun_oldest`, the closest match to
  zrlog's Discard policy, and is still 2 orders of magnitude slower.
- **NanoLog** is Linux-only (POSIX AIO) and cannot be measured here.

## A note on the archived results

The `results_*` directories span the whole tuning history, including runs that
were later found to be contaminated (background VM, quill queue reallocation,
cross-run drift, E-core binding, missing warm-up). Aggregate ranges over *all*
directories therefore look wider than any single run: zrlog throughput ranges
69-414 M/s across 84 rounds, and its p99 ranges 25-402 ns. Treat the per-run
`env.txt` as authoritative.

Prefer these for a clean comparison:

- `results_win_pcore_burn_8mb/` — 9 rounds, P-cores, 100 ms burn, all four
  libraries, fmtlog at 8 MB. **Current canonical run.**
- `results_win_ecore_235/` — same but bound to E-cores `2,3,4,5`, kept as
  evidence for caveat 5.

## Reproducing

See `README.md` for build and run instructions. The canonical run is:

```
powershell -File bench\run_win.ps1 -Rounds 9 -Threads 4 -Per 2000000
python bench\aggregate.py bench\bin\results
```

`run_win.ps1` auto-detects P-cores and defaults to `-BurnMs 100`. Before
publishing numbers, confirm no VM is running and that `env.txt` shows
`Pinned: 0,10,12,14` (or whatever the local P-cores are) and `BurnMs: 100`.
