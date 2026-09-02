#!/usr/bin/env bash
# Benchmark the four nix binaries across logger-stressing workloads.
set -u
cd /tmp/dlbench

BASE=/nix/store/080ra2sg23fwg74n4s7l97jixprb9j5p-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
ORIG=/nix/store/kg4c47n0xbs1as4q5p2sj75ch99dqqcm-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
DIRTY=/nix/store/k2qzigk7zi6d9k8n824qraa1r62jpwm1-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
NONBLOCK=/nix/store/lfacnivddh502q0119h9wf7z5i1lnv45-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix

FLAGS="--extra-experimental-features nix-command"

# ~100us of hashing work per trace so the flood spans many 300ms ticks while the
# message list grows to 150k entries.
FLOOD="toString (builtins.foldl' (acc: x: builtins.trace \"spam message number \${toString x}\" (acc + builtins.stringLength (builtins.hashString \"sha256\" (builtins.concatStringsSep \"-\" (builtins.genList (y: toString (x + y)) 512))))) 0 (builtins.genList (x: x) 150000))"

# 500k traces as fast as eval can emit them; exercises the final flush / big-diff path.
BURST="toString (builtins.foldl' (acc: x: builtins.trace \"spam message number \${toString x}\" (acc + x)) 0 (builtins.genList (x: x) 500000))"

COPYPATHS="/nix/store/0a7326j0l4lhqkdwgwv9hx37jfz9i4fr-nix-prefetch-git-26.05 /nix/store/05xfwnl62nipbw1ankbcv2crjhb9a918-python3-3.13.14 /nix/store/18311cxwsdjsc52dhas54wfqqa212q5m-openjdk-21.0.12+8"

RESULTS=/tmp/dlbench/results.tsv
: > "$RESULTS"

timed() { # workload label rep cmd...
  local workload=$1 label=$2 rep=$3; shift 3
  local tf out code
  tf=$(mktemp)
  { TIMEFORMAT='%R %U %S'; time timeout 600 "$@" >/dev/null 2>/dev/null; } 2> "$tf"
  code=$?
  out=$(tail -1 "$tf")
  rm -f "$tf"
  echo -e "$workload\t$label\t$rep\t$out\t$code" | tee -a "$RESULTS"
}

echo "=== flood (150k traces + hash work, stderr > /dev/null) ==="
for rep in 1 2 3; do
  timed flood base/internal-json $rep $BASE eval $FLAGS --log-format internal-json --expr "$FLOOD"
  timed flood base/raw           $rep $BASE eval $FLAGS --log-format raw           --expr "$FLOOD"
  timed flood orig/diffs         $rep $ORIG eval $FLAGS --log-format diffs         --expr "$FLOOD"
  timed flood dirty/diffs        $rep $DIRTY eval $FLAGS --log-format diffs        --expr "$FLOOD"
  timed flood nonblock/diffs     $rep $NONBLOCK eval $FLAGS --log-format diffs     --expr "$FLOOD"
done

echo "=== burst (500k plain traces) ==="
for rep in 1 2 3; do
  timed burst base/internal-json $rep $BASE eval $FLAGS --log-format internal-json --expr "$BURST"
  timed burst orig/diffs         $rep $ORIG eval $FLAGS --log-format diffs         --expr "$BURST"
  timed burst dirty/diffs        $rep $DIRTY eval $FLAGS --log-format diffs        --expr "$BURST"
  timed burst nonblock/diffs     $rep $NONBLOCK eval $FLAGS --log-format diffs     --expr "$BURST"
done

echo "=== copy (214-path closure to file:// cache) ==="
for rep in 1 2; do
  for cfg in "base/internal-json:$BASE:internal-json" "orig/diffs:$ORIG:diffs" "dirty/diffs:$DIRTY:diffs" "nonblock/diffs:$NONBLOCK:diffs"; do
    label=${cfg%%:*}; rest=${cfg#*:}; bin=${rest%%:*}; fmt=${rest#*:}
    rm -rf /tmp/dlbench/cache
    timed copy "$label" $rep $bin copy $FLAGS --log-format $fmt --no-check-sigs --to "file:///tmp/dlbench/cache?compression=none" $COPYPATHS
  done
done
rm -rf /tmp/dlbench/cache

echo "=== stall (consumer reads 1KB then stops; timeout 25s; exit 0 = survived, 124 = hung) ==="
stall() { # label bin fmt
  local label=$1 bin=$2 fmt=$3
  local fifo=/tmp/dlbench/fifo.$$
  mkfifo "$fifo"
  ( exec 3<"$fifo"; head -c 1024 <&3 >/dev/null; sleep 90 ) &
  local reader=$!
  local t0 t1 code
  t0=$(date +%s.%N)
  timeout 25 "$bin" eval $FLAGS --log-format "$fmt" --expr "$BURST" >/dev/null 2>"$fifo"
  code=$?
  t1=$(date +%s.%N)
  kill "$reader" 2>/dev/null
  pkill -P "$reader" 2>/dev/null
  wait "$reader" 2>/dev/null
  rm -f "$fifo"
  echo -e "stall\t$label\t1\t$(echo "$t1 $t0" | awk '{printf "%.2f", $1-$2}')\t-\t-\t$code" | tee -a "$RESULTS"
}
stall base/internal-json $BASE internal-json
stall orig/diffs $ORIG diffs
stall dirty/diffs $DIRTY diffs
stall nonblock/diffs $NONBLOCK diffs

echo "=== stream validity (every stderr line parses as JSON) ==="
for cfg in "orig:$ORIG" "nonblock:$NONBLOCK"; do
  label=${cfg%%:*}; bin=${cfg#*:}
  $bin eval $FLAGS --log-format diffs --expr "$BURST" >/dev/null 2>/tmp/dlbench/stream-$label.txt
  bad=$(awk 'NF' /tmp/dlbench/stream-$label.txt | while IFS= read -r line; do echo "$line" | jq -e . >/dev/null 2>&1 || echo BAD; done | wc -l)
  echo "$label: $(wc -l < /tmp/dlbench/stream-$label.txt) lines, $bad unparseable"
done

echo "DONE"
