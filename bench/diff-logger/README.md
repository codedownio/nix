# DiffLogger benchmarks

Comparison of the `--log-format diffs` logger across four versions of this fork, plus stock nix
as a baseline. Run 2026-08-31 on a single Linux x86_64 machine (NixOS, kernel 6.12), all binaries
built as `.#nix-cli-static` (static musl) so toolchain and dependencies are held constant.

## Binaries

| label | commit | branch | store path (as run) |
|---|---|---|---|
| base | 2c73b59da | upstream 2.35-maintenance (merge-base; substituted from cache.nixos.org, bit-identical to the official binary) | `080ra2sg...` |
| orig | c862694be | 2.35.2-diff-logger-filtered | `kg4c47n0...` |
| dirty | 49f9c5e2d | 2.35.2-diff-logger-dirty-flag | `k2qzigk7...` |
| nonblock | 94f7c7fd5 | 2.35.2-diff-logger-nonblocking | `lfacnivd...` |
| incr | d87bde07b | 2.35.2-diff-logger-incremental | `qnrs42b7...` |

`base` has no `diffs` format, so it runs `internal-json` (and `raw`) as the closest equivalents.

## Scripts

- `run.sh` — round 1: base/orig/dirty/nonblock over all workloads, plus the stall test and a
  stream-validity check (every line parses as JSON). Results: `results.tsv`.
- `run2.sh` — round 2 (takes the incr binary path as `$1`): incr with dirty re-run alongside as a
  drift anchor, plus stream-equivalence checks. Results: `results2.tsv`.
- `run3.sh` — output-volume measurement (stderr bytes/lines per version per workload).
  Results: `bytes.tsv`.
- `apply.py` — minimal RFC 6902 applier (add/replace/remove, `-` array append); reconstructs the
  final state from a captured stream so two loggers' streams can be compared semantically.

The scripts hardcode the store paths above and work under `/tmp/dlbench`; to rerun, rebuild the
binaries at the listed commits and update the paths. Timing is bash `time` (wall/user/sys),
3 reps with configs interleaved within each rep, medians reported. Timed runs send stderr to
/dev/null so consumer speed can't contaminate results.

## Workloads

- **flood** — 150k `builtins.trace` messages, each with ~100µs of `hashString` work so the run
  spans ~30 ticks (~9s) while the message list grows. Stresses per-tick cost against a large
  accumulated state. (Plain traces evaluate too fast — 500k in 1.5s is only ~5 ticks — hence the
  padding work.)
- **burst** — 500k plain traces; nearly everything lands in the final flush.
- **copy** — a 214-path closure (`nix copy` to a `file://` cache, compression=none); realistic
  activity/result traffic.
- **stall** — a fifo consumer reads 1KB of stderr then holds the pipe open without reading;
  `timeout 25` around a 500k-trace eval. Tests hang behavior, not speed.

## Results

Flood, median wall seconds:

| version | wall | vs base internal-json |
|---|---|---|
| base internal-json | 8.55 | — |
| base raw | 8.66 | +1% |
| orig | 11.60 | +36% |
| dirty | 10.12 | +18% |
| nonblock | 9.54 | +12% (only version with wall < user CPU: conversion overlaps eval) |
| incr | 8.70 | +2% |

Burst: base 0.75s, orig 2.58s, dirty 2.03s, nonblock 1.62s, incr 0.98s.
Copy: IO-bound, all versions within noise (~1.2–3.3s).

Stall:

| version | outcome |
|---|---|
| base internal-json | hung; ignored SIGTERM at 25s (blocked in `write(2)`, signal never delivered to that thread), died only at ~92s when the pipe closed |
| orig | hung, killed by timeout at 25s |
| dirty | hung, killed by timeout at 25s |
| nonblock | completed, exit 0 in 3.2s |
| incr | completed, exit 0 in 2.9s |

Output volume (stderr bytes):

| workload | base internal-json | orig | incr |
|---|---|---|---|
| flood | 10.99 MB / 150k lines | 15.56 MB / ~31 lines | 15.56 MB / ~30 lines |
| burst | 36.89 MB / 500k lines | 52.39 MB / 2–3 lines | 52.14 MB / 2–3 lines |
| copy | 112.25 MB / 1.25M lines | 121 KB / 5 lines | 124 KB / 6 lines |

The incremental logger's whole-activity `replace` ops cost ~1–2% extra bytes on activity-heavy
workloads versus `json::diff`'s leaf-path ops; message workloads are byte-identical. The diffs
format itself trades ~42% more bytes than internal-json under message floods for ~900x fewer
bytes on activity-heavy workloads, since it samples state at 300ms instead of streaming every
result event.

## Equivalence

Reconstructing the final state from a captured stream (`apply.py`) for the same workload through
orig and incr yields byte-identical states, modulo a pre-existing bug where `NixMessage.level`
serialized uninitialized memory (fixed in 569350c08 on the incremental branch, after the
benchmarked commit). A 214-path copy stream — which exercises the incremental logger's
whole-activity `replace` ops — applies cleanly.

# Round 3: builder output (2.35.2-diff-logger-build-logs)

Run 2026-09-01, same machine. Unlike rounds 1 and 2 these are meson dev builds (`nix develop`,
`ninja -C build`) rather than `.#nix-cli-static`, so the timings aren't comparable to the tables
above — only the byte counts are measured here.

None of the round 1/2 workloads run a builder, so none of them produce a single `resBuildLogLine`.
Two new workloads:

- `run4.sh` — a derivation whose builder prints N lines of ~58 characters (`build20k`,
  `build200k`), plus the post-build hook's own output (`resPostBuildLogLine`). Results:
  `bytes-buildlogs.tsv`.
- `run5.sh` — `nix copy` of a python3 closure to a `file://` cache. Activity-heavy, used to price
  the per-result-type `results` map. Results: `bytes-results.tsv`.

## Build logs

`build200k` = 200k lines, 11.69 MB of raw builder output.

| format | bytes | lines |
|---|---|---|
| raw-with-logs | 11.69 MB | 200,006 |
| internal-json | 26.09 MB | 200,051 |
| diffs (before this branch) | 3.2 KB | 4–6 |
| diffs (this branch, logs off) | 2.6 KB | 3 |
| diffs-with-logs, one op per line | 29.49 MB | 103 |
| diffs-with-logs, batched | 12.10 MB | 102 |

The old 3.2 KB is not a saving: it's 200k log lines being thrown away, with one surviving line
left in the activity's `fields`. With logs on, batching consecutive lines from the same activity
into one `add /logs/-` entry is what makes the format competitive — one op per line spends ~90
bytes of wrapper on a ~58 byte line (2.5x the raw output, 13% worse than internal-json), while
batching amortizes the wrapper over ~2000 lines and lands 3.5% above raw output and 2.2x below
internal-json.

`build20k` shows the same ratios: 1.13 MB raw, 2.57 MB internal-json, 2.91 MB per-line,
1.17 MB batched.

## Cost of the `results` map

Stream bytes vary run to run with how many 300ms ticks elapse, so this compares the reconstructed
final state (`apply.py`), which is tick-independent.

| variant | state bytes | vs before |
|---|---|---|
| before this branch | 11,242 | — |
| `results` added, `fields` still clobbered (compat) | 12,295 | +9.4% |
| `results` added, `fields` left as the start fields | 13,979 | +24.3% |

The clean-break variant is the larger of the two because it stops discarding data: the start
fields of an `actCopyPath` are a store path plus two store URIs, which the old code overwrote
with the four integers of the next progress result.

## Backpressure

400k lines through a consumer reading one patch line per 300ms, against the 16 MB queue cap:
44,000 lines delivered, 356,003 reported via `{"dropped": N}` entries, totalling exactly the
400,003 lines the builder produced. The count only balances with the drain at the end of `stop()`
(958d6c8cc); before that, everything still queued when the 5s exit deadline expired was discarded
without a marker.

## Timing and stall (round 3)

`run6.sh`, same machine, 2026-09-02. Both binaries are meson `--buildtype=release` builds — the
first attempt used the dev shell's default `-O0`, where flood took 140s instead of 9s and the
numbers were meaningless. These are dynamic release builds rather than `.#nix-cli-static`, so
they aren't comparable to rounds 1 and 2 in absolute terms; base and new are built identically,
so the comparison between them holds. Medians of 3.

`build200k`, 200k builder lines:

| config | wall | user | sys |
|---|---|---|---|
| base/diffs | 1.628 | 0.067 | 0.166 |
| new/diffs (logs off) | 1.698 | 0.068 | 0.172 |
| new/diffs-with-logs | 1.518 | 0.132 | 0.151 |
| new/raw-with-logs | 1.349 | 0.117 | 0.161 |
| new/internal-json | 1.532 | 0.237 | 0.146 |

Wall time is dominated by the builder's own bash loop and the daemon round trips: the range
within a single config is ±25% (new/diffs spans 1.37–2.24s), so it says nothing here. Client CPU
is the signal. Streaming 200k lines costs 0.065s of user CPU over the same run with logs off —
about 0.33µs per line — which lands between `raw-with-logs` (0.117) and `internal-json` (0.237).
With logs off, 0.068 vs the baseline's 0.067 is unchanged, as expected: the new code paths are
behind the `printBuildLogs` check.

`flood` (150k traces + hash work) is untouched by this branch and was rerun as a drift check:
7.726 base vs 7.916 new, with the runs overlapping (7.48–9.02 vs 7.59–8.40). No regression
visible.

Stall — the consumer reads 1KB and then holds the pipe open without reading, `timeout 90`:

| workload | config | elapsed | exit |
|---|---|---|---|
| 200k lines | base/diffs | 2.67 | 0 |
| 200k lines | new/diffs | 1.91 | 0 |
| 200k lines | new/diffs-with-logs | 4.59 | 0 |
| 400k lines | base/diffs | 3.54 | 0 |
| 400k lines | new/diffs | 3.03 | 0 |
| 400k lines | new/diffs-with-logs | 5.25 | 0 |

Nothing hangs, so the property `94f7c7fd5` established survives. Streaming logs does cost about
3s more at exit against a consumer that has stopped reading entirely: `stop()` keeps retrying the
drain until its 5s deadline, and a dead consumer means no retry can make progress. That deadline
is a single constant in `stop()` if the tradeoff should go the other way.

Output line length is worth knowing for consumers that split stdin with a bounded line length:
at `maxLogLinesPerFlush = 2000` and ~58 byte log lines, the longest line in a `build200k`
`diffs-with-logs` stream is 123 KB. A builder emitting 1 KB lines would push that into the
megabytes; it scales with the cap, which is also a single constant.
