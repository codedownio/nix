#!/usr/bin/env bash
# Round 3 timing: what streaming builder output costs in wall/CPU time, and whether the exit path
# is still bounded when the consumer stops reading. Takes the baseline nix as $1 and the
# build-logs nix as $2. Results: results3.tsv.
set -u
BASE=$1
NEW=$2
WORKDIR=/tmp/dlbench
RESULTS=$WORKDIR/results3.tsv
mkdir -p "$WORKDIR"
: > "$RESULTS"

FLAGS="--extra-experimental-features nix-command --no-link"
BASH_PATH=/nix/store/1sr8rmx4v0v994lkbzhwc1f0qr1gxxs9-bash-5.3p9

# Message-heavy workloads carried over from round 1, to check the paths this branch didn't touch.
FLOOD="toString (builtins.foldl' (acc: x: builtins.trace \"spam message number \${toString x}\" (acc + builtins.stringLength (builtins.hashString \"sha256\" (builtins.concatStringsSep \"-\" (builtins.genList (y: toString (x + y)) 512))))) 0 (builtins.genList (x: x) 150000))"

# The builder prints $2 lines of ~58 characters; the nonce keeps the drv unique so each rep
# actually builds.
build_expr() { # nonce nlines
  # Slashes in the label would make an invalid derivation name.
  local nonce=${1//\//-}
  cat <<EOF
derivation {
  name = "logspam-$nonce";
  system = "x86_64-linux";
  builder = "\${builtins.storePath "$BASH_PATH"}/bin/bash";
  args = [ "-c" "i=0; while [ \\\$i -lt $2 ]; do echo \\"build log line \\\$i of $2: the quick brown fox jumps\\"; i=\\\$((i+1)); done; echo done > \\\$out" ];
}
EOF
}

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

echo "=== build200k (200k builder lines, stderr > /dev/null) ==="
for rep in 1 2 3; do
  for cfg in "base/diffs:$BASE:diffs" "new/diffs:$NEW:diffs" "new/diffs-with-logs:$NEW:diffs-with-logs" \
             "new/raw-with-logs:$NEW:raw-with-logs" "new/internal-json:$NEW:internal-json"; do
    label=${cfg%%:*}; rest=${cfg#*:}; bin=${rest%%:*}; fmt=${rest#*:}
    build_expr "t6-$label-$rep" 200000 > "$WORKDIR/drv.nix"
    timed build200k "$label" $rep $bin build $FLAGS --log-format "$fmt" --file "$WORKDIR/drv.nix"
  done
done

echo "=== flood (150k traces + hash work; untouched by this branch, drift check) ==="
for rep in 1 2 3; do
  timed flood base/diffs $rep $BASE eval --extra-experimental-features nix-command --log-format diffs --expr "$FLOOD"
  timed flood new/diffs  $rep $NEW  eval --extra-experimental-features nix-command --log-format diffs --expr "$FLOOD"
done

# The builder itself takes tens of seconds at these sizes, so the pass criterion isn't a fixed
# deadline like round 1's: it's exit 0, with elapsed no more than a few seconds above the
# unstalled wall time above.
echo "=== stall (consumer reads 1KB then stops; timeout 90s; exit 0 = survived, 124 = hung) ==="
stall() { # label bin fmt nlines
  local label=$1 bin=$2 fmt=$3 nlines=$4
  local fifo=$WORKDIR/fifo.$$
  rm -f "$fifo"; mkfifo "$fifo"
  ( exec 3<"$fifo"; head -c 1024 <&3 >/dev/null; sleep 120 ) &
  local reader=$!
  build_expr "stall-$label-$nlines" "$nlines" > "$WORKDIR/drv.nix"
  local t0 t1 code
  t0=$(date +%s.%N)
  timeout 90 "$bin" build $FLAGS --log-format "$fmt" --file "$WORKDIR/drv.nix" >/dev/null 2>"$fifo"
  code=$?
  t1=$(date +%s.%N)
  kill "$reader" 2>/dev/null
  pkill -P "$reader" 2>/dev/null
  wait "$reader" 2>/dev/null
  rm -f "$fifo"
  echo -e "stall-$nlines\t$label\t1\t$(echo "$t1 $t0" | awk '{printf "%.2f", $1-$2}')\t-\t-\t$code" | tee -a "$RESULTS"
}
# 200k for a like-for-like comparison against the timed runs above; 400k is ~23MB of builder
# output, past the 16MB queue cap, so it also exercises dropping against a consumer that never
# drains at all.
for n in 200000 400000; do
  stall base/diffs "$BASE" diffs $n
  stall new/diffs "$NEW" diffs $n
  stall new/diffs-with-logs "$NEW" diffs-with-logs $n
done

echo "DONE"
