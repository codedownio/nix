#!/usr/bin/env bash

source common.sh

TODO_NixOS

clearStore

# A builder that prints a known number of lines, so we can check that all of them survive the
# trip through the logger.
nlines=500
cat > "$TEST_ROOT/logspam.nix" <<EOF
with import ${config_nix};
mkDerivation {
  name = "logspam";
  buildCommand = ''
    i=0
    while [ \$i -lt $nlines ]; do echo "log line \$i"; i=\$((i+1)); done
    echo done > \$out
  '';
}
EOF

# Every line of both formats' output must be valid JSON: the first line is the initial state,
# the rest are RFC 6902 patch arrays.
checkStream() { # file
    jq -e . < "$1" > /dev/null
}

# The `logs` array is always declared, but stays empty without --print-build-logs.
nix build -f "$TEST_ROOT/logspam.nix" --no-link --log-format diffs 2> "$TEST_ROOT/diffs.jsonl"
checkStream "$TEST_ROOT/diffs.jsonl"
[[ $(head -n 1 "$TEST_ROOT/diffs.jsonl" | jq -c '.logs') = '[]' ]]
grepQuietInverse '"/logs/-"' "$TEST_ROOT/diffs.jsonl"

# With --print-build-logs, every line the builder printed comes through, in order, batched into
# entries on that array. The flag works on either side of --log-format: it is applied to the
# logger the format creates, not just to whichever logger happened to be current.
for order in "--log-format diffs -L" "-L --log-format diffs"; do
    clearStore

    # shellcheck disable=SC2086
    nix build -f "$TEST_ROOT/logspam.nix" --no-link $order 2> "$TEST_ROOT/withlogs.jsonl"
    checkStream "$TEST_ROOT/withlogs.jsonl"

    # resBuildLogLine is 101; the post-build hook's output (107) is not part of the count.
    tail -n +2 "$TEST_ROOT/withlogs.jsonl" \
        | jq -r '.[] | select(.path == "/logs/-") | .value | select(.type == 101) | .lines[]' \
        > "$TEST_ROOT/lines"
    [[ $(wc -l < "$TEST_ROOT/lines") = "$nlines" ]]
    seq 0 $((nlines - 1)) | sed 's/^/log line /' | diff - "$TEST_ROOT/lines"
done

clearStore

# A failed build reports its log tail in the error message when we aren't streaming logs,
# and the message reaches the consumer as a /messages entry.
cat > "$TEST_ROOT/logfail.nix" <<EOF
with import ${config_nix};
mkDerivation {
  name = "logfail";
  buildCommand = ''
    echo "a line before the failure"
    exit 3
  '';
}
EOF

expectStderr 1 nix build -f "$TEST_ROOT/logfail.nix" --no-link --log-format diffs \
    > "$TEST_ROOT/fail.jsonl"
tail -n +2 "$TEST_ROOT/fail.jsonl" \
    | jq -r '.[] | select(.path == "/messages/-") | .value.msg' \
    | grepQuiet "a line before the failure"
