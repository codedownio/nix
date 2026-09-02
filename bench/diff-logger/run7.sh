#!/usr/bin/env bash
# Consumer-speed behaviour at exit: a consumer that reads slowly but steadily should get the whole
# log, while one that stops reading altogether should not delay exit. Takes the nix binary as $1
# and a label as $2; appends to consumer-speed.tsv.
set -u
NIX=$1
LABEL=$2
WORKDIR=/tmp/dlbench
OUT=$WORKDIR/consumer-speed.tsv
mkdir -p "$WORKDIR"

BASH_PATH=/nix/store/1sr8rmx4v0v994lkbzhwc1f0qr1gxxs9-bash-5.3p9
FLAGS="--extra-experimental-features nix-command --no-link"
# 150k lines is ~8.7MB, under the 16MB queue cap, so nothing should be dropped for lack of room:
# any loss here is the exit path giving up on the consumer.
NLINES=150000

build_expr() { # nonce
  cat <<EOF
derivation {
  name = "logspam-${1//\//-}";
  system = "x86_64-linux";
  builder = "\${builtins.storePath "$BASH_PATH"}/bin/bash";
  args = [ "-c" "i=0; while [ \\\$i -lt $NLINES ]; do echo \\"build log line \\\$i: the quick brown fox jumps\\"; i=\\\$((i+1)); done; echo done > \\\$out" ];
}
EOF
}

account() { # file
  python3 - "$1" <<'PY'
import json, sys
lines = dropped = 0
for l in open(sys.argv[1]):
    l = l.strip()
    if not l: continue
    d = json.loads(l)
    if isinstance(d, dict): continue
    for op in d:
        if op["path"] == "/logs/-":
            v = op["value"]
            if "dropped" in v: dropped += v["dropped"]
            else: lines += len(v["lines"])
print(f"{lines}\t{dropped}")
PY
}

# Slow but steady: one patch line per 100ms.
slow() {
  local fifo=$WORKDIR/fifo.slow.$$ out=$WORKDIR/slow-$LABEL.jsonl
  rm -f "$fifo" "$out"; mkfifo "$fifo"
  ( while IFS= read -r l; do printf '%s\n' "$l" >> "$out"; sleep 0.1; done < "$fifo" ) &
  local reader=$! t0 code
  build_expr "slow-$LABEL" > "$WORKDIR/drv.nix"
  sleep 0.3
  t0=$(date +%s.%N)
  timeout 300 "$NIX" build $FLAGS --log-format diffs-with-logs --file "$WORKDIR/drv.nix" >/dev/null 2>"$fifo"
  code=$?
  local elapsed=$(echo "$(date +%s.%N) $t0" | awk '{printf "%.2f", $1-$2}')
  wait "$reader" 2>/dev/null
  rm -f "$fifo"
  echo -e "slow\t$LABEL\t$elapsed\t$(account "$out")\t$((NLINES + 3))\t$code" | tee -a "$OUT"
}

# Stopped: reads 1KB, then holds the pipe open without reading.
stopped() {
  local fifo=$WORKDIR/fifo.stopped.$$
  rm -f "$fifo"; mkfifo "$fifo"
  ( exec 3<"$fifo"; head -c 1024 <&3 >/dev/null; sleep 300 ) &
  local reader=$! t0 code
  build_expr "stopped-$LABEL" > "$WORKDIR/drv.nix"
  sleep 0.3
  t0=$(date +%s.%N)
  timeout 300 "$NIX" build $FLAGS --log-format diffs-with-logs --file "$WORKDIR/drv.nix" >/dev/null 2>"$fifo"
  code=$?
  local elapsed=$(echo "$(date +%s.%N) $t0" | awk '{printf "%.2f", $1-$2}')
  kill "$reader" 2>/dev/null; pkill -P "$reader" 2>/dev/null; wait "$reader" 2>/dev/null
  rm -f "$fifo"
  echo -e "stopped\t$LABEL\t$elapsed\t-\t-\t$((NLINES + 3))\t$code" | tee -a "$OUT"
}

slow
stopped
echo "DONE"
