#!/usr/bin/env bash
# Measure stderr output volume (bytes/lines) for a workload that produces builder output, which
# none of the earlier workloads do. Takes the nix binary as $1 and a label as $2; appends to
# bytes-buildlogs.tsv.
set -u
NIX=$1
LABEL=$2
WORKDIR=/tmp/dlbench
OUT=$WORKDIR/bytes-buildlogs.tsv
mkdir -p "$WORKDIR"

BASH_PATH=/nix/store/1sr8rmx4v0v994lkbzhwc1f0qr1gxxs9-bash-5.3p9
FLAGS="--extra-experimental-features nix-command --no-link"

# A derivation whose builder prints $2 lines of ~60 characters. The nonce keeps the drv path
# unique so every run actually builds instead of hitting the store.
build_expr() { # nonce nlines
  cat <<EOF
derivation {
  name = "logspam-$1";
  system = "x86_64-linux";
  builder = "\${builtins.storePath "$BASH_PATH"}/bin/bash";
  args = [ "-c" "i=0; while [ \\\$i -lt $2 ]; do echo \\"build log line \\\$i of $2: the quick brown fox jumps\\"; i=\\\$((i+1)); done; echo done > \\\$out" ];
}
EOF
}

run() { # workload fmt nonce nlines rep
  local workload=$1 fmt=$2 nonce=$3 nlines=$4 rep=$5
  # Via a file rather than --expr, since builtins.storePath needs impure evaluation.
  build_expr "$nonce" "$nlines" > "$WORKDIR/drv.nix"
  $NIX build $FLAGS --log-format "$fmt" --file "$WORKDIR/drv.nix" \
    >/dev/null 2>"$WORKDIR/b.out"
  local ec=$?
  echo -e "$workload\t$LABEL\t$fmt\t$rep\t$(wc -c < "$WORKDIR/b.out")\t$(wc -l < "$WORKDIR/b.out")\t$ec" | tee -a "$OUT"
  cp "$WORKDIR/b.out" "$WORKDIR/stream-$LABEL-$workload-$fmt-$rep.jsonl"
  rm -f "$WORKDIR/b.out"
}

for rep in 1 2; do
  for fmt in raw-with-logs internal-json diffs diffs-with-logs; do
    # `diffs-with-logs` only exists on the new binaries; skip it where it isn't recognized.
    if [ "$fmt" = diffs-with-logs ] && ! $NIX eval --extra-experimental-features nix-command \
         --log-format diffs-with-logs --expr 1 >/dev/null 2>&1; then
      continue
    fi
    run build20k "$fmt" "$LABEL-$fmt-$rep-a" 20000 $rep
    run build200k "$fmt" "$LABEL-$fmt-$rep-b" 200000 $rep
  done
done
echo "DONE"
