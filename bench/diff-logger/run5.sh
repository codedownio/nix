#!/usr/bin/env bash
# Activity-heavy workload (a store copy), used to measure the byte cost of recording each result
# type separately rather than in one clobbered `fields` slot. Takes the nix binary as $1 and a
# label as $2; appends to bytes-results.tsv.
set -u
NIX=$1
LABEL=$2
WORKDIR=/tmp/dlbench
OUT=$WORKDIR/bytes-results.tsv
mkdir -p "$WORKDIR"

COPYPATHS="/nix/store/05xfwnl62nipbw1ankbcv2crjhb9a918-python3-3.13.14"
FLAGS="--extra-experimental-features nix-command"

for rep in 1 2; do
  for fmt in internal-json diffs; do
    rm -rf "$WORKDIR/cache"
    $NIX copy $FLAGS --log-format "$fmt" --no-check-sigs \
      --to "file://$WORKDIR/cache?compression=none" $COPYPATHS >/dev/null 2>"$WORKDIR/c.out"
    ec=$?
    echo -e "copy\t$LABEL\t$fmt\t$rep\t$(wc -c < "$WORKDIR/c.out")\t$(wc -l < "$WORKDIR/c.out")\t$ec" | tee -a "$OUT"
    cp "$WORKDIR/c.out" "$WORKDIR/copystream-$LABEL-$fmt-$rep.jsonl"
  done
done
rm -rf "$WORKDIR/cache" "$WORKDIR/c.out"
echo "DONE"
