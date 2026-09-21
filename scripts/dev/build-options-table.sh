#!/usr/bin/env bash
# build-options-table.sh
#
# Regenerates docs/build-options.md from two mechanical inputs:
#   1. `grep -n '^option(HU_ENABLE_' CMakeLists.txt` — every top-level
#      HU_ENABLE_* option, its help string, and its cmake-level default.
#      (Options declared inside an if()/else() — e.g. HU_ENABLE_APPLE_
#      INTELLIGENCE, HU_ENABLE_PWA, which pick a platform-dependent default —
#      are indented and so do not match the anchored `^option(` pattern.
#      That is deliberate: this table documents the flat, single-default
#      options a preset can meaningfully turn on or off.)
#   2. CMakePresets.json's `configurePresets`, resolved through `inherits`
#      chains, to say which presets set each option ON.
#
# USAGE
#   bash scripts/dev/build-options-table.sh            # print to stdout
#   bash scripts/dev/build-options-table.sh --write    # regenerate docs/build-options.md
#
# The option() parser is multi-line aware (HU_ENABLE_TOPOLOGY_CHECK wraps its
# help string onto a second line) and refuses to emit a short table: the
# parsed option count must equal `grep -c '^option(HU_ENABLE_'` +
# `grep -c '^cmake_dependent_option(HU_ENABLE_'`, or the script exits 1
# instead of silently dropping a row.
#
# This does NOT simulate CMakeLists.txt's HU_ENABLE_ALL_CHANNELS cascade
# (the `if(HU_ENABLE_ALL_CHANNELS) set(HU_ENABLE_TELEGRAM ON) ... endif()`
# block) — a preset that sets HU_ENABLE_ALL_CHANNELS=ON turns on ~20 more
# channel options at configure time that this table won't list as "ON" for
# that preset, because that logic lives in CMakeLists.txt control flow, not
# in a grep-able option() declaration or a CMakePresets.json cache variable.
# See the note in docs/build-options.md's header.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo "$(dirname "$0")/../..")"

OUT="docs/build-options.md"
WRITE=0
[ "${1:-}" = "--write" ] && WRITE=1

# Reproducibility: key the "as of" line to the last commit that touched
# either input file, not wall-clock time, so re-running on a later day with
# no changes to either file produces byte-identical output.
LAST_SHA=$(git log -1 --format=%h -- CMakeLists.txt CMakePresets.json 2>/dev/null || echo "unknown")

python3 - "$WRITE" "$OUT" "$LAST_SHA" <<'PYEOF'
import json, re, subprocess, sys

write = sys.argv[1] == "1"
out_path = sys.argv[2]
last_sha = sys.argv[3]

# --- 1. Options: grep -n '^option(HU_ENABLE_' CMakeLists.txt ---------------
# Multi-line aware: HU_ENABLE_TOPOLOGY_CHECK wraps its help string onto a
# second line (`option(HU_ENABLE_TOPOLOGY_CHECK\n       "..." ON)`), so a
# single-line regex silently drops it. Scan from each line matching
# `^option(HU_ENABLE_`, growing the joined chunk until the full call parses.
opt_re = re.compile(
    r'^option\((HU_ENABLE_[A-Z0-9_]+)\s+"((?:[^"\\]|\\.)*)"\s+(ON|OFF)\)\s*$'
)
MAX_SPAN = 6  # generous cap on lines a single option() call may span

with open("CMakeLists.txt", encoding="utf-8") as f:
    lines = f.read().split("\n")

options = []  # (name, description, default)
i = 0
n = len(lines)
while i < n:
    if re.match(r'^option\(HU_ENABLE_[A-Z0-9_]+', lines[i]):
        matched = None
        span_used = 1
        for span in range(1, MAX_SPAN + 1):
            chunk = "\n".join(lines[i : i + span])
            m = opt_re.match(chunk)
            if m:
                matched = m
                span_used = span
                break
        if matched:
            options.append((matched.group(1), matched.group(2), matched.group(3)))
            i += span_used
            continue
        else:
            print(
                f"build-options-table.sh: could not parse option() starting at "
                f"CMakeLists.txt line {i + 1} within {MAX_SPAN} lines: {lines[i]!r}",
                file=sys.stderr,
            )
            sys.exit(1)
    i += 1

# cmake_dependent_option(NAME "desc" default depends_on force_value) — none
# exist today, but parse them too so this script does not silently go stale.
dep_re = re.compile(
    r'^cmake_dependent_option\((HU_ENABLE_[A-Z0-9_]+)\s+"((?:[^"\\]|\\.)*)"\s+(ON|OFF)\s+"([^"]*)"\s+(ON|OFF)\)\s*$'
)
dependent_options = []
i = 0
while i < n:
    if re.match(r'^cmake_dependent_option\(HU_ENABLE_[A-Z0-9_]+', lines[i]):
        matched = None
        span_used = 1
        for span in range(1, MAX_SPAN + 1):
            chunk = "\n".join(lines[i : i + span])
            m = dep_re.match(chunk)
            if m:
                matched = m
                span_used = span
                break
        if matched:
            name, desc, default, depends, _force_off_value = matched.groups()
            dependent_options.append((name, f"{desc} (depends on: {depends})", default))
            options.append((name, f"{desc} (depends on: {depends})", default))
            i += span_used
            continue
        else:
            print(
                f"build-options-table.sh: could not parse cmake_dependent_option() "
                f"starting at CMakeLists.txt line {i + 1} within {MAX_SPAN} lines: "
                f"{lines[i]!r}",
                file=sys.stderr,
            )
            sys.exit(1)
    i += 1

# --- Guard: parsed count must match the two anchored greps exactly ---------
def grep_count(pattern):
    result = subprocess.run(
        ["grep", "-c", pattern, "CMakeLists.txt"], capture_output=True, text=True
    )
    try:
        return int(result.stdout.strip() or "0")
    except ValueError:
        return 0


expected_options = grep_count("^option(HU_ENABLE_")
expected_dependent = grep_count("^cmake_dependent_option(HU_ENABLE_")
parsed_options = len(options) - len(dependent_options)

if parsed_options != expected_options:
    print(
        f"build-options-table.sh: parsed {parsed_options} option(HU_ENABLE_...) "
        f"declarations but grep -c '^option(HU_ENABLE_' CMakeLists.txt says "
        f"{expected_options} — refusing to emit a table that silently drops or "
        f"invents rows. Fix the parser, not the count.",
        file=sys.stderr,
    )
    sys.exit(1)
if len(dependent_options) != expected_dependent:
    print(
        f"build-options-table.sh: parsed {len(dependent_options)} "
        f"cmake_dependent_option(HU_ENABLE_...) declarations but grep -c "
        f"'^cmake_dependent_option(HU_ENABLE_' CMakeLists.txt says "
        f"{expected_dependent} — refusing to emit a table that silently drops "
        f"or invents rows.",
        file=sys.stderr,
    )
    sys.exit(1)

# --- 2. Presets: CMakePresets.json configurePresets, resolved through inherits ---
with open("CMakePresets.json", encoding="utf-8") as f:
    presets_doc = json.load(f)

by_name = {p["name"]: p for p in presets_doc["configurePresets"]}
order = [p["name"] for p in presets_doc["configurePresets"]]


def resolved_cache_vars(name, _seen=None):
    """Merge cacheVariables along the inherits chain; child overrides parent."""
    _seen = _seen or set()
    if name in _seen:
        return {}
    _seen.add(name)
    preset = by_name[name]
    merged = {}
    inherits = preset.get("inherits")
    if inherits:
        parents = inherits if isinstance(inherits, list) else [inherits]
        for parent in parents:
            merged.update(resolved_cache_vars(parent, _seen))
    merged.update(preset.get("cacheVariables", {}))
    return merged


resolved = {name: resolved_cache_vars(name) for name in order}


def is_on(value):
    if isinstance(value, dict):
        value = value.get("value", "")
    return str(value).strip().upper() == "ON"


# --- 3. Emit the table ------------------------------------------------------
lines_out = []
lines_out.append("---")
lines_out.append("title: Build Options")
lines_out.append(
    "description: Every HU_ENABLE_* CMake option, its default, and which presets turn it on."
)
lines_out.append("---")
lines_out.append("")
lines_out.append("# Build Options")
lines_out.append("")
lines_out.append("GENERATED FILE. Do not hand-edit — regenerate with:")
lines_out.append("")
lines_out.append("```")
lines_out.append("bash scripts/dev/build-options-table.sh --write")
lines_out.append("```")
lines_out.append("")
lines_out.append(
    f"Reflects `CMakeLists.txt` + `CMakePresets.json` as of commit `{last_sha}` "
    "(the most recent commit to touch either file) — keyed to a commit rather "
    "than wall-clock time so re-running this script with no changes to either "
    "file produces byte-identical output."
)
lines_out.append("")
lines_out.append(
    "The \"Presets ON\" column lists every `configurePresets` entry (after resolving "
    "`inherits` chains) whose effective cache value for that option is `ON` — either "
    "because the preset (or a preset it inherits from) sets it explicitly, or because "
    "no preset in the chain overrides it and the option's own default (below) is `ON`."
)
lines_out.append("")
lines_out.append(
    "**Not reflected here:** `HU_ENABLE_ALL_CHANNELS=ON` (set by the `test`, `release`, "
    "`fuzz` presets) also flips ~20 more `HU_ENABLE_<channel>` options ON at CMake "
    "configure time, via an `if(HU_ENABLE_ALL_CHANNELS) set(HU_ENABLE_TELEGRAM ON) ... "
    "endif()` block in `CMakeLists.txt` — that is control flow, not a declared default "
    "or a preset cache variable, so a preset that relies on it will show those channel "
    "options as off below even though they compile in. Only options declared with an "
    "unconditional top-level `option(HU_ENABLE_...)` (or `cmake_dependent_option`) "
    "appear in this table; options declared inside an `if()/else()` for a "
    "platform-dependent default (for example `HU_ENABLE_APPLE_INTELLIGENCE`, "
    "`HU_ENABLE_PWA`) are intentionally excluded — see the script's header comment."
)
lines_out.append("")
lines_out.append("| Option | Default | Presets ON | Gates |")
lines_out.append("|---|---|---|---|")
for name, desc, default in options:
    on_presets = [p for p in order if is_on(resolved[p].get(name, default))]
    presets_str = ", ".join(on_presets) if on_presets else "*(none)*"
    desc_escaped = desc.replace("|", "\\|")
    lines_out.append(f"| `{name}` | {default} | {presets_str} | {desc_escaped} |")
lines_out.append("")

output = "\n".join(lines_out)
if write:
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(output)
    print(
        f"Wrote {out_path} ({len(options)} options, {len(order)} presets, as of {last_sha}).",
        file=sys.stderr,
    )
else:
    print(output)
PYEOF
