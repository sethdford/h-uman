#!/bin/sh
# scripts/check-function-length-ceiling.sh
#
# Function-length ratchet: the longest function body in src/**/*.c may only
# SHRINK. Measured with clang's AST (compile_commands.json flags), not brace
# counting — a brace-depth scan of src/daemon.c was wrong by 20x because of
# unbalanced braces across #if arms.
#
# Why: the file-size ratchet froze src/daemon.c at ~12,300 lines and thereby
# blessed a single 10,087-line function (hu_service_run, 2026-09-10 review).
# No other gate in the repo could see it. This one can.
#
# Usage:
#   scripts/check-function-length-ceiling.sh              # all of src/
#   scripts/check-function-length-ceiling.sh src/daemon.c # specific file(s)
#   HU_BUILD_DIR=build-test scripts/check-function-length-ceiling.sh
#
# Exit codes: 0 = no function exceeds MAX_FN_BASELINE; 1 = ceiling exceeded;
#             0 with a warning if clang/compile_commands.json are unavailable
#             (the gate is advisory where it cannot measure).
#
# Lower MAX_FN_BASELINE after a refactor shrinks the largest function, in the
# same commit, to lock the gain. Target: 300 lines.

set -eu

MAX_FN_BASELINE=10087   # hu_service_run, src/daemon.c, measured 2026-09-12

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="${HU_BUILD_DIR:-build}"
CC_JSON="$BUILD_DIR/compile_commands.json"

if ! command -v clang >/dev/null 2>&1 || [ ! -f "$CC_JSON" ] || ! command -v python3 >/dev/null 2>&1; then
    echo "check-function-length-ceiling: clang, python3 or $CC_JSON missing — skipping (advisory)"
    exit 0
fi

if [ $# -gt 0 ]; then
    FILES="$*"
else
    FILES=$(find src -name '*.c' -not -path 'src/data/*' | sort)
fi

python3 - "$CC_JSON" "$MAX_FN_BASELINE" $FILES <<'PY'
import json, shlex, subprocess, sys, os
cc_json, baseline, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
cc = json.load(open(cc_json))
by_file = {}
for e in cc:
    f = os.path.relpath(e["file"], os.getcwd()) if os.path.isabs(e["file"]) else e["file"]
    # prefer the human_core object over test-library duplicates
    if f not in by_file or "human_core.dir/" in e.get("output", ""):
        by_file[f] = e
worst = (0, "", "", 0, 0)
missing = 0
for f in files:
    e = by_file.get(f)
    if not e:
        missing += 1
        continue
    args = shlex.split(e["command"]) if "command" in e else list(e["arguments"])
    out = []; skip = False
    for a in args[1:]:
        if skip: skip = False; continue
        if a in ("-o", "-MF", "-MT", "-MQ"): skip = True; continue
        if a == "-c" or a.endswith(".c") or a.startswith("-M"): continue
        out.append(a)
    cmd = ["clang"] + out + ["-fsyntax-only", "-w", "-Xclang", "-ast-dump=json",
                             "-Xclang", "-ast-dump-filter=hu_", e["file"]]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=e["directory"])
    dec = json.JSONDecoder(); s = r.stdout; i = 0
    base = os.path.basename(f)
    while i < len(s):
        j = s.find("{", i)
        if j < 0: break
        try:
            obj, k = dec.raw_decode(s, j)
        except Exception:
            i = j + 1; continue
        i = k
        stack = [obj]
        while stack:
            o = stack.pop()
            if not isinstance(o, dict): continue
            if o.get("kind") == "FunctionDecl" and any(c.get("kind") == "CompoundStmt" for c in o.get("inner", [])):
                loc = o.get("loc", {}); rng = o.get("range", {})
                b = rng.get("begin", {}).get("line") or loc.get("line")
                en = rng.get("end", {}).get("line")
                lf = loc.get("file", "")
                if b and en and (not lf or lf.endswith(base)):
                    n = en - b + 1
                    if n > worst[0]: worst = (n, o["name"], f, b, en)
            stack.extend(o.get("inner", []))
if missing and missing == len(files):
    print(f"check-function-length-ceiling: none of the {missing} file(s) are in {cc_json} — skipping (advisory)")
    sys.exit(0)
n, name, f, b, en = worst
print(f"longest function: {name} = {n} lines ({f}:{b}-{en}) (ceiling {baseline})")
if n > baseline:
    print(f"ERROR: {name} exceeds the function-length ceiling ({n} > {baseline}).", file=sys.stderr)
    print("Split the function (attach()/detach() per subsystem, a tick table) or,", file=sys.stderr)
    print("if you shrank the largest one, lower MAX_FN_BASELINE in this script.", file=sys.stderr)
    sys.exit(1)
PY
