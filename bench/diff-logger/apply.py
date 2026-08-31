#!/usr/bin/env python3
"""Apply a diff-logger stream (line 1: full state; rest: RFC 6902 patch arrays) and print the final state."""
import json, sys

def resolve(doc, tokens):
    for t in tokens:
        doc = doc[int(t) if isinstance(doc, list) else t.replace("~1", "/").replace("~0", "~")]
    return doc

def apply_op(state, op):
    tokens = op["path"].split("/")[1:]
    parent = resolve(state, tokens[:-1])
    last = tokens[-1] if not isinstance(parent, list) else None
    kind = op["op"]
    if isinstance(parent, list):
        idx = len(parent) if tokens[-1] == "-" else int(tokens[-1])
        if kind == "add":
            parent.insert(idx, op["value"])
        elif kind == "replace":
            parent[idx] = op["value"]
        elif kind == "remove":
            del parent[idx]
        else:
            raise ValueError(f"unsupported op {kind}")
    else:
        key = last.replace("~1", "/").replace("~0", "~")
        if kind == "add":
            parent[key] = op["value"]
        elif kind == "replace":
            if key not in parent:
                raise KeyError(f"replace on missing key {op['path']}")
            parent[key] = op["value"]
        elif kind == "remove":
            del parent[key]
        else:
            raise ValueError(f"unsupported op {kind}")

def main(path):
    with open(path) as f:
        lines = [l for l in f if l.strip()]
    state = json.loads(lines[0])
    for n, line in enumerate(lines[1:], start=2):
        for op in json.loads(line):
            try:
                apply_op(state, op)
            except Exception as e:
                print(f"ERROR line {n}: {e} on {json.dumps(op)[:200]}", file=sys.stderr)
                sys.exit(1)
    json.dump(state, sys.stdout, sort_keys=True)

main(sys.argv[1])
