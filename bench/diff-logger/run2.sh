#!/usr/bin/env bash
# Benchmark the incremental diff logger, with dirty-flag re-run as a drift anchor,
# plus stream-equivalence checks against the original logger.
set -u
cd /tmp/dlbench

ORIG=/nix/store/kg4c47n0xbs1as4q5p2sj75ch99dqqcm-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
DIRTY=/nix/store/k2qzigk7zi6d9k8n824qraa1r62jpwm1-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
INCR=$1

FLAGS="--extra-experimental-features nix-command"

FLOOD="toString (builtins.foldl' (acc: x: builtins.trace \"spam message number \${toString x}\" (acc + builtins.stringLength (builtins.hashString \"sha256\" (builtins.concatStringsSep \"-\" (builtins.genList (y: toString (x + y)) 512))))) 0 (builtins.genList (x: x) 150000))"
BURST="toString (builtins.foldl' (acc: x: builtins.trace \"spam message number \${toString x}\" (acc + x)) 0 (builtins.genList (x: x) 500000))"
SMALL="toString (builtins.foldl' (acc: x: builtins.trace \"equivalence check msg \${toString x}\" (acc + x)) 0 (builtins.genList (x: x) 3000))"
COPYPATHS="/nix/store/0a7326j0l4lhqkdwgwv9hx37jfz9i4fr-nix-prefetch-git-26.05 /nix/store/05xfwnl62nipbw1ankbcv2crjhb9a918-python3-3.13.14 /nix/store/18311cxwsdjsc52dhas54wfqqa212q5m-openjdk-21.0.12+8"

RESULTS=/tmp/dlbench/results2.tsv
: > "$RESULTS"

timed() {
  local workload=$1 label=$2 rep=$3; shift 3
  local tf out code
  tf=$(mktemp)
  { TIMEFORMAT='%R %U %S'; time timeout 600 "$@" >/dev/null 2>/dev/null; } 2> "$tf"
  code=$?
  out=$(tail -1 "$tf")
  rm -f "$tf"
  echo -e "$workload\t$label\t$rep\t$out\t$code" | tee -a "$RESULTS"
}

echo "=== flood ==="
for rep in 1 2 3; do
  timed flood dirty/diffs $rep $DIRTY eval $FLAGS --log-format diffs --expr "$FLOOD"
  timed flood incr/diffs  $rep $INCR eval $FLAGS --log-format diffs  --expr "$FLOOD"
done

echo "=== burst ==="
for rep in 1 2 3; do
  timed burst dirty/diffs $rep $DIRTY eval $FLAGS --log-format diffs --expr "$BURST"
  timed burst incr/diffs  $rep $INCR eval $FLAGS --log-format diffs  --expr "$BURST"
done

echo "=== copy ==="
for rep in 1 2; do
  for cfg in "dirty/diffs:$DIRTY" "incr/diffs:$INCR"; do
    label=${cfg%%:*}; bin=${cfg#*:}
    rm -rf /tmp/dlbench/cache
    timed copy "$label" $rep $bin copy $FLAGS --log-format diffs --no-check-sigs --to "file:///tmp/dlbench/cache?compression=none" $COPYPATHS
  done
done

echo "=== stall ==="
stall() {
  local label=$1 bin=$2
  local fifo=/tmp/dlbench/fifo.$$
  mkfifo "$fifo"
  ( exec 3<"$fifo"; head -c 1024 <&3 >/dev/null; sleep 90 ) &
  local reader=$!
  local t0 t1 code
  t0=$(date +%s.%N)
  timeout 25 "$bin" eval $FLAGS --log-format diffs --expr "$BURST" >/dev/null 2>"$fifo"
  code=$?
  t1=$(date +%s.%N)
  kill "$reader" 2>/dev/null; pkill -P "$reader" 2>/dev/null; wait "$reader" 2>/dev/null
  rm -f "$fifo"
  echo -e "stall\t$label\t1\t$(echo "$t1 $t0" | awk '{printf "%.2f", $1-$2}')\t-\t-\t$code" | tee -a "$RESULTS"
}
stall incr/diffs $INCR

echo "=== equivalence (same workload through orig and incr; reconstruct and compare) ==="
$ORIG eval $FLAGS --log-format diffs --expr "$SMALL" >/dev/null 2>/tmp/dlbench/eq-orig.txt
$INCR eval $FLAGS --log-format diffs --expr "$SMALL" >/dev/null 2>/tmp/dlbench/eq-incr.txt
python3 /tmp/dlbench/apply.py /tmp/dlbench/eq-orig.txt > /tmp/dlbench/eq-orig-final.json || echo "ORIG APPLY FAILED"
python3 /tmp/dlbench/apply.py /tmp/dlbench/eq-incr.txt > /tmp/dlbench/eq-incr-final.json || echo "INCR APPLY FAILED"
if cmp -s /tmp/dlbench/eq-orig-final.json /tmp/dlbench/eq-incr-final.json; then
  echo "EQUIVALENCE: identical final states ($(wc -c < /tmp/dlbench/eq-incr-final.json) bytes)"
else
  echo "EQUIVALENCE: MISMATCH"
fi

echo "=== copy-stream applies cleanly (activity path coverage) ==="
rm -rf /tmp/dlbench/cache
$INCR copy $FLAGS --log-format diffs --no-check-sigs --to "file:///tmp/dlbench/cache?compression=none" $COPYPATHS >/dev/null 2>/tmp/dlbench/eq-copy.txt
rm -rf /tmp/dlbench/cache
python3 /tmp/dlbench/apply.py /tmp/dlbench/eq-copy.txt > /tmp/dlbench/eq-copy-final.json && echo "COPY STREAM: applies cleanly, $(wc -l < /tmp/dlbench/eq-copy.txt) lines, final state $(wc -c < /tmp/dlbench/eq-copy-final.json) bytes" || echo "COPY STREAM: APPLY FAILED"

echo "DONE"
