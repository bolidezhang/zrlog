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
```

- `notify_consumer()` reads the shared `idle_wait_flag_` — a cache line the
  consumer writes from another core on every idle cycle.
- It is gated on a `config_` field read from the singleton each call.
- Historical: this build also had `note_produced(buffer, ...)` gated on
  `config_.producer_stats` (two increments plus a `& 1023` test). Both the
  `producer_stats` switch and all produce/consume counters were **removed** on
  the `opt/frontend-latency` branch to shorten the frontend path.

fmtlog has neither. quill has neither. The `notify_consumer()` check, plus
`alloc`'s cursor handling, is the most plausible source of zrlog's +4-5 ns p50.

Disabling notification and producer stats together was measured (on the old
build): it lowered the fast mode from ~28 to ~20 ns but did not remove the
20↔50 bimodality. Disabling only `notify_consumer()` made things **worse**
(p99.9 90.8 → 223.0) because the consumer then only wakes on the 1 ms idle
timeout and drains in bursts. **Do not remove the notification** — it is a net
positive; the p50 cost is the price of a smooth tail.

### Queue design

fmtlog's `SPSCVarQueueOPT` allocates in 8-byte blocks (`blk_sz`), so the
producer does no ring arithmetic and no wrap-around branch on the fast path.
zrlog's `alloc` computes `phys_w` / `tail_free` and handles wrap with a
padding entry. At 8 MB this happens once per 8 MB written, so it is not a
measurable cost in these tests.

### Cache-line layout

Measured with a mirror struct (MSVC, `hardware_destructive_interference_size`
= 64). This is a **historical snapshot**: `produce_attempts_` /
`produce_accepted_` were later removed on `opt/frontend-latency` together with
all produce/consume counters, so current offsets and `sizeof` differ.

| field | old offset | new offset |
|---|---|---|
| `write_index_` | 0 | 0 (`alignas(128)`) |
| `cached_read_index_` | 64 | 8 |
| `produce_attempts_` (removed) | 128 | 16 |
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

### Local producer write cursor (measured: no effect)

`opt/frontend-latency` changed `ThreadBuffer::alloc()`/`commit()` to keep a
producer-private `local_write_index_` and publish once via release store in
`commit()`, removing two relaxed loads of the shared `write_index_`. A/B with
9 interleaved rounds in each order (18 samples total, P-cores, 100 ms burn):

| run | base p50 | opt p50 | base thr | opt thr |
|---|---|---|---|---|
| forward | 20.0 | 20.0 | 324.5 | 354.6 |
| reversed | 22.0 | 21.4 | 329.1 | 326.1 |

p50/p99/p99.9 are indistinguishable; the throughput difference in the forward
run is ordering/environment noise (the reversed run removes it). Correctness
was checked with a dedicated wraparound-integrity test (50k sequence-numbered
logs through a 1 KB buffer with Block policy, every line validated).

The loads were cheap because they hit the producer's L1 and the benchmark
disables `notify_consumer_per_log`, so this optimisation has **no measurable
effect** in this harness. Kept because it is semantically safe and removes
redundant shared-atomic reads, but it does not close the gap to quill.

## Frontend ablation series (opt/frontend-latency, assembly-driven)

MSVC `/FAcs` probes of `ZRLOG_INFO` revealed the real structure of the gap:
`get_thread_buffer()` was a *called* function with a 72-byte frame and 6
register saves even on the TLS-hit path; `push_log_entry` was a real call with
arguments spilled to stack; `NanoLogger::instance()` costs 6 loads of TLS
epoch state per call. quill's `log_statement` inlines everything except two
small helpers. The series of single-variable A/B runs (9 forward + 9 reverse
each, P-cores, 100 ms burn):

| experiment | p50 | verdict |
|---|---|---|
| baseline (post-stats-removal) | 20.0 | — |
| split TLS buffer lookup: inline `get_thread_buffer`, noinline `create_thread_buffer` | **18.0** | **kept — the one clear win** |
| split `alloc` hot/cold | 18.7 | regressed, reverted |
| forceinline `push_log_entry` | 17.7 (noise-level) | reverted |
| forceinline `alloc` | 18.7, p99 +6 | regressed, reverted |
| zero-guard singleton via global pointer | no change | reverted |
| compile-time policy (`ZRLOG_COMPILE_TIME_POLICY`, off by default) | 17.7 in ctp mode | kept as opt-in; ~0.3-0.7 ns |
| call-site constants in `FmtProvider` (30 paired samples) | p50 diff median 0.00 | indistinguishable, reverted |
| noinline Block/Retry slow path; Discard stays inline (30 paired samples) | p50 diff 0.00; p99.9 diff **-3.30** | kept: better tail, no p50/throughput cost |
| `WriteReservation` value returned from `alloc` (30 paired samples) | p50 diff 0.00; p99.9 diff +1.35 | reverted |
| **hoist `rdtsc` before `alloc`** (30 paired samples) | **p50 diff -1.30; p99 diff -3.30; p99.9 diff -6.70** (25/30 negative) | **kept: OPT FASTER; latency-throughput trade-off** |

Key findings:

- **Function-frame cost, not atomics, dominated the gap.** Splitting the TLS
  lookup alone moved p50 from 20.0 to 18.0 and lifted throughput medians by
  ~15% (367→468 M/s in one pairing). The win is MSVC-specific in mechanism
  (frame setup for cold paths) but likely general in kind.
- **`alloc` must NOT be forceinlined or split.** Both directions regressed
  p50 and p99. The independent function boundary appears to be good for
  I-cache/layout; quill also keeps its reserve step as a separate concept.
- **The singleton guard and `push_log_entry` call frame are not worth
  removing.** Direct-pointer singleton removed 6 TLS loads from assembly but
  measured nothing; forceinline of the push removed the call and argument
  spills but measured noise. Both reverted.
- **Compile-time policy is a real but small lever** (~0.3-0.7 ns p50 in ctp
  mode plus visibly better p99.9 in some pairings). It ships behind
  `ZRLOG_COMPILE_TIME_POLICY`, refuses `init()` when the runtime Config
  disagrees with the baked-in policy, and defaults to plain runtime-config
  behaviour.
- **Moving call-site file/func/line/level into `FmtProvider` changed nothing.**
  A 15-forward + 15-reverse paired run gave p50 difference median 0.00 ns and
  throughput difference +9.5 M/s (only 18/30 rounds positive). The assembly
  explains why: MSVC had already folded the fixed arguments and aligned entry
  size into immediate values. Reverted.
- **Splitting only the Block/Retry full-buffer slow paths is a tail win.**
  Keeping Discard inline is essential: the first version routed Discard through
  the noinline helper too and lost 36.7 M/s median paired throughput. With
  Discard inline, the 30-sample paired differences were p50 0.00, avg -0.20,
  p99 -0.60, p99.9 **-3.30 ns** (21 negative vs 8 positive), throughput +6.87
  M/s median. Kept.

Same-session comparison after the series (medians of 7 rounds, P-cores,
burn 100 ms). This run includes the kept Block/Retry slow-path split:

| lib | avg | min | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|---|---|
| quill | 14.9 | 13.4 | 14.7 | 15.4 | 16.0 | 30.7 |
| zrlog | 18.2 | 16.0 | **17.4** | 18.0 | 30.0 | **39.4** |
| fmtlog | 18.4 | 15.4 | 16.0 | 17.4 | 78.1 | 90.1 |
| spdlog | 450.4 | 157.6 | 245.1 | 1007.6 | 2212.9 | 5768.6 |

zrlog's p50 gap to quill shrank from 5.3 to **2.7 ns**. The remaining gap lives
in the call-site `log_id` state and the 16-vs-32-byte header paths that were
not reachable by pure function-boundary surgery. All ablation binaries and
per-round JSON are in `bench/bin/` (gitignored).

### Hoisting `rdtsc` before `alloc` — the one change that broke the resolution floor

This is the only experiment in the entire series whose paired-difference median
exceeded the 0.6-0.95 ns IQR. The mechanism is visible in the assembly: before
the change, `rdtsc` was issued *after* `alloc` returned (serialised on the
critical path). After hoisting it to just before the call, the 12-16 cycle TSC
read latency overlaps with the call setup (5 instructions) and the `alloc`
function body (~10-15 cycles):

```asm
000ec  rdtsc                 ; issued early
000ee  shl  rdx, 32          ; ┐
000f2  mov  rcx, rbx         ; │ call setup: rdtsc latency hidden here
000f5  or   rax, rdx         ; │
000f8  mov  edx, 32          ; │
000fd  mov  rsi, rax         ; ┘ tsc saved to rsi
00100  call  alloc            ; ← latency overlaps with alloc body
```

30 paired samples (15 forward + 15 reverse, P-cores, 100 ms burn):

| metric | paired diff median | negative/zero/positive | verdict |
|---|---|---|---|
| p50 | **-1.30 ns** | 25/3/2 | **OPT FASTER** |
| p99 | **-3.30 ns** | 25/2/3 | |
| p99.9 | **-6.70 ns** | 25/2/3 | |
| throughput | **-142 M/s** | **30/0/0** | ⚠️ trade-off |

The latency win is real and consistent across every percentile. The throughput
cost is also real and systematic: base 374 M/s (range 359-717) vs opt 231 M/s
(range 214-328) — the ranges do not overlap. The mechanism is a classic
latency-throughput trade-off: `rdtsc` is a partially-serializing instruction
that prevents speculative execution of subsequent instructions on Intel. When
issued early (at the top of the hot path), it stalls the OoO engine before the
`alloc` call can be dispatched, hurting the 4-thread full-speed throughput loop.
When issued late (after `alloc` returns), it is serialised on the critical path
but does not block speculative execution of the alloc itself.

**The change is kept** because the project goal is frontend latency, not
throughput. If throughput matters more in a deployment, revert this change (one
line) or use `ZRLOG_COMPILE_TIME_POLICY` to test both configurations. The
throughput number is an *attempt rate* (Discard policy) and not a processing
rate — see the "Throughput is not comparable" section above.

### Where the remaining 2.7 ns actually goes (MSVC `/FAcs` of `log_one`)

The kept slow-path split leaves this hot sequence, in order:

1. Singleton guard: `gs:88` TEB read, `_tls_index` load, `_Init_thread_epoch`
   load + compare + branch (~6 instructions, all L1).
2. `active_level_` byte load + `cmp` + branch.
3. Call-site `log_id` dword load + `test` + branch.
4. TLS `__tls_guard` byte check + branch, then `thread_buffer_` load.
5. `alloc(len)` — an out-of-line call (kept; forceinline and hot/cold split
   both regressed).
6. `rdtsc`, 4 header stores, 2 argument stores, `local_write_index_` RMW,
   release store of `write_index_`.
7. `config_.notify_consumer_per_log` byte load + branch (benchmark sets it
   false, so the branch is taken to the exit).

quill's equivalent is: `_thread_context` load, one `prepare_write_reserve_cached`
(no wrap arithmetic — the queue is a flat ring with `& _mask`), four qword
header stores, argument encode, one release store, then a `_message_flush_threshold`
load. The structural differences that remain are:

- **zrlog has two call-site-visible branches quill does not**: the `log_id`
  registration check and the singleton guard. Both were measured as
  individually removable at no gain (zero-guard singleton, static-key
  metadata), so neither alone is the cost.
- **quill's queue has no wrap branch on the write path**; zrlog's `alloc`
  checks `phys_w + len > size_`. At 8 MB this fires once per 8 MB, so it is
  a predicted-not-taken branch, not a measurable cost.
- **zrlog writes 16 bytes of header + 12 bytes of args = 32 B per record;
  quill writes 32 B of header + args.** zrlog's record is smaller, so this
  favours zrlog on bandwidth, not against it.

At this point the p50 delta is ~4 TSC ticks (0.67 ns/tick). The measured
resolution of this harness is 1 tick, and the round-to-round IQR of the paired
difference is 0.6-0.95 ns.

**After hoisting `rdtsc` (the one change that broke the resolution floor), the
p50 gap drops to ~1.4 ns** (17.4 - 1.3 = ~16.1 vs quill's 14.7). The remaining
~1.4 ns is now below the 1-tick resolution floor in individual paired runs and
is not attributable to any single instruction sequence that has been isolated.

### The store-to-load chain that looks like it should matter but does not

`commit()` compiles to a memory RMW plus a dependent reload:

```
add  QWORD PTR [rdi+8], 32      ; local_write_index_ += len
mov  rax, QWORD PTR [rdi+8]     ; dependent reload (store-to-load forwarding)
mov  QWORD PTR [rdi], rax       ; release store of write_index_
```

quill avoids this shape: `prepare_write_reserve_cached` returns `writer_pos` **by
value inside `WriteReservation`**, and `finish_and_commit_write_reservation`
stores the register value, so no reload exists.

Porting the same shape to zrlog was measured and **did not help**. The assembly
explains why: a 16-byte struct is returned via sret, so MSVC materialises it on
the stack (`lea rdx, res$3[rsp]`), and the dependent reload simply moves from
`[rdi+8]` to `[rsp+48]`. 30 paired samples: p50 difference median 0.00
(mean -0.44, 14 negative / 9 zero / 7 positive), p99.9 **+1.35 ns** worse,
throughput +3.3 M/s (noise). Reverted.

The lesson is that on this machine the forwarding latency is hidden under the
`alloc` call and the surrounding serialisation stores; it is not on the critical
path. Any future attempt to remove it must first eliminate the sret (e.g. return
the two values in a register pair via a `struct` of two 64-bit scalars marked
`[[nodiscard]]`, or split into `alloc_ptr()` + `alloc_pos()` inlined together),
otherwise it only relocates the dependency.

## Open questions

- **Why does a 20-50 ms CPU burn remove zrlog's bimodal latency?** A pure ALU
  burn, with no logging involved, moves zrlog from ~50 ns to ~20 ns and holds
  it there for 11/11 rounds. quill and fmtlog are unaffected. Candidates:
  P-state ramp interacting with frontend code volume; first-touch cost on the
  384-byte `ThreadBuffer` struct; TSC-vs-core-frequency ratio during ramp.
  Not distinguished by any experiment so far. **The bimodality is suppressed,
  not explained, and it reappeared in one later 5-round session
  (p50 34.1 / 32.1 / 27.4) — treat "fixed" as unproven.**
- zrlog's p50 gap to quill is now ~3.3 ns (was 5.3). Confirmed dead ends:
  `config_`-to-TLS snapshot (A/D/F, regressed), producer-cursor localisation
  (no effect), zero-guard singleton (no effect despite removing 6 TLS loads),
  forceinline of `push_log_entry`/`alloc` (no effect or regressed), and a
  key-indexed call-site metadata prototype (below). The one confirmed win was
  splitting the TLS buffer lookup into an inlined check plus noinline creator.
  The remaining untested candidate is quill's 32-byte self-describing header
  versus zrlog's 16-byte header + separate `LogMeta` lookup.

### Call-site `static constexpr` metadata prototype (reverted)

A prototype replaced the per-call-site `static std::atomic<uint32_t> log_id`
with a `static constexpr char` key whose address identifies the call site;
`NanoLogger` kept an `unordered_map<const void*, uint32_t>` from key to meta id
and every log statement did an (unsynchronised) `find()` before writing the id
into the unchanged 16-byte header. Correctness was fine (ctest 4/4), and the
assembly removed the atomic load — but the hash lookup is far more expensive:

| (18 samples, 9 forward + 9 reverse) | p50 | p99.9 | throughput |
|---|---|---|---|
| baseline (atomic log_id) | 17.4 | 39.1 | 626.8 M/s |
| key-indexed prototype | 26.0 | 50.7 | 377.2 M/s |

The +8.6 ns p50 and 40% throughput regression is decisive. Unlike quill,
zrlog's backend dereferences a `LogMeta` by id at decode time; removing the
call-site atomic alone replaces one L1 atomic load with a hash traversal on
**every** log, while quill's `MacroMetadata` is already baked into the
compiled decoder and never looked up. Closing the remaining 3.3 ns therefore
needs the full self-describing-header change, not just the call-site key.

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
