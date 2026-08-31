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
