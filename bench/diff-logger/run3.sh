#!/usr/bin/env bash
# Measure stderr output volume (bytes/lines) per logger version per workload.
set -u
cd /tmp/dlbench

BASE=/nix/store/080ra2sg23fwg74n4s7l97jixprb9j5p-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
ORIG=/nix/store/kg4c47n0xbs1as4q5p2sj75ch99dqqcm-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
DIRTY=/nix/store/k2qzigk7zi6d9k8n824qraa1r62jpwm1-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
NONBLOCK=/nix/store/lfacnivddh502q0119h9wf7z5i1lnv45-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix
INCR=/nix/store/qnrs42b7cj9hw7xz872mijxynhgnb11b-nix-static-x86_64-unknown-linux-musl-2.35.2/bin/nix

FLAGS="--extra-experimental-features nix-command"

FLOOD="toString (builtins.foldl' (acc: x: builtins.trace \"spam message number \${toString x}\" (acc + builtins.stringLength (builtins.hashString \"sha256\" (builtins.concatStringsSep \"-\" (builtins.genList (y: toString (x + y)) 512))))) 0 (builtins.genList (x: x) 150000))"
BURST="toString (builtins.foldl' (acc: x: builtins.trace \"spam message number \${toString x}\" (acc + x)) 0 (builtins.genList (x: x) 500000))"
COPYPATHS="/nix/store/0a7326j0l4lhqkdwgwv9hx37jfz9i4fr-nix-prefetch-git-26.05 /nix/store/05xfwnl62nipbw1ankbcv2crjhb9a918-python3-3.13.14 /nix/store/18311cxwsdjsc52dhas54wfqqa212q5m-openjdk-21.0.12+8"

OUT=/tmp/dlbench/bytes.tsv
: > "$OUT"

measure() { # workload label rep file
  local workload=$1 label=$2 rep=$3 f=$4
  echo -e "$workload\t$label\t$rep\t$(wc -c < "$f")\t$(wc -l < "$f")" | tee -a "$OUT"
  rm -f "$f"
}

for rep in 1 2; do
  for cfg in "base/internal-json:$BASE:internal-json" "orig/diffs:$ORIG:diffs" "dirty/diffs:$DIRTY:diffs" "nonblock/diffs:$NONBLOCK:diffs" "incr/diffs:$INCR:diffs"; do
    label=${cfg%%:*}; rest=${cfg#*:}; bin=${rest%%:*}; fmt=${rest#*:}

    $bin eval $FLAGS --log-format $fmt --expr "$FLOOD" >/dev/null 2>/tmp/dlbench/b.out
    measure flood "$label" $rep /tmp/dlbench/b.out

    $bin eval $FLAGS --log-format $fmt --expr "$BURST" >/dev/null 2>/tmp/dlbench/b.out
    measure burst "$label" $rep /tmp/dlbench/b.out

    rm -rf /tmp/dlbench/cache
    $bin copy $FLAGS --log-format $fmt --no-check-sigs --to "file:///tmp/dlbench/cache?compression=none" $COPYPATHS >/dev/null 2>/tmp/dlbench/b.out
    measure copy "$label" $rep /tmp/dlbench/b.out
  done
done
rm -rf /tmp/dlbench/cache
echo "DONE"
