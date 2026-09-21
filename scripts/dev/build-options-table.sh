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
# This does NOT simulate CMakeLists.txt's HU_ENABLE_ALL_CHANNELS cascade
# (the `if(HU_ENABLE_ALL_CHANNELS) set(HU_ENABLE_TELEGRAM ON) ... endif()`
# block) — a preset that sets HU_ENABLE_ALL_CHANNELS=ON turns on ~20 more
# channel options at configure time that this table won't list as "ON" for
# that preset, because that logic lives in CMakeLists.txt control flow, not
# in a grep-able option() declaration or a CMakePresets.json cache variable.
# See the note in docs/build-options.md's header.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || dirname "$0"/../..)"

OUT="docs/build-options.md"
WRITE=0
[ "${1:-}" = "--write" ] && WRITE=1

python3 - "$WRITE" <<'PYEOF'
import json, re, sys, datetime

write = sys.argv[1] == "1"

# --- 1. Options: grep -n '^option(HU_ENABLE_' CMakeLists.txt, in file order ---
opt_re = re.compile(r'^option\((HU_ENABLE_[A-Z0-9_]+)\s+"((?:[^"\\]|\\.)*)"\s+(ON|OFF)\)\s*$')
options = []  # list of (name, description, default)
with open("CMakeLists.txt", encoding="utf-8") as f:
    for line in f:
        m = opt_re.match(line.rstrip("\n"))
        if m:
            name, desc, default = m.group(1), m.group(2), m.group(3)
            options.append((name, desc, default))

# cmake_dependent_option(NAME "desc" default depends_on force_value) — none
# exist today (grep -n cmake_dependent_option CMakeLists.txt is empty), but
# parse them too so this script does not silently go stale if one is added.
dep_re = re.compile(
    r'^cmake_dependent_option\((HU_ENABLE_[A-Z0-9_]+)\s+"((?:[^"\\]|\\.)*)"\s+(ON|OFF)\s+"([^"]*)"\s+(ON|OFF)\)\s*$'
)
with open("CMakeLists.txt", encoding="utf-8") as f:
    for line in f:
        m = dep_re.match(line.rstrip("\n"))
        if m:
            name, desc, default, depends, force_off_value = m.groups()
            options.append((name, f"{desc} (depends on: {depends})", default))

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


# --- 3. Emit the table ---
today = datetime.date.today().isoformat()
lines = []
lines.append("---")
lines.append("title: Build Options")
lines.append(
    "description: Every HU_ENABLE_* CMake option, its default, and which presets turn it on."
)
lines.append("---")
lines.append("")
lines.append("# Build Options")
lines.append("")
lines.append(
    "GENERATED FILE. Do not hand-edit — regenerate with:"
)
lines.append("")
lines.append("```")
lines.append("bash scripts/dev/build-options-table.sh --write")
lines.append("```")
lines.append("")
lines.append(f"Last generated: {today}, from `CMakeLists.txt` + `CMakePresets.json` at that commit.")
lines.append("")
lines.append(
    "The \"Presets ON\" column lists every `configurePresets` entry (after resolving "
    "`inherits` chains) whose effective cache value for that option is `ON` — either "
    "because the preset (or a preset it inherits from) sets it explicitly, or because "
    "no preset in the chain overrides it and the option's own default (below) is `ON`."
)
lines.append("")
lines.append(
    "**Not reflected here:** `HU_ENABLE_ALL_CHANNELS=ON` (set by the `test`, `release`, "
    "`fuzz` presets) also flips ~20 more `HU_ENABLE_<channel>` options ON at CMake "
    "configure time, via an `if(HU_ENABLE_ALL_CHANNELS) set(HU_ENABLE_TELEGRAM ON) ... "
    "endif()` block in `CMakeLists.txt` — that is control flow, not a declared default "
    "or a preset cache variable, so a preset that relies on it will show those channel "
    "options as off below even though they compile in. Only options declared with an "
    "unconditional top-level `option(HU_ENABLE_...)` appear in this table; options "
    "declared inside an `if()/else()` for a platform-dependent default (for example "
    "`HU_ENABLE_APPLE_INTELLIGENCE`, `HU_ENABLE_PWA`) are intentionally excluded — see "
    "the script's header comment."
)
lines.append("")
lines.append("| Option | Default | Presets ON | Gates |")
lines.append("|---|---|---|---|")
for name, desc, default in options:
    on_presets = [p for p in order if is_on(resolved[p].get(name, default))]
    presets_str = ", ".join(on_presets) if on_presets else "*(none)*"
    desc_escaped = desc.replace("|", "\\|")
    lines.append(f"| `{name}` | {default} | {presets_str} | {desc_escaped} |")
lines.append("")

output = "\n".join(lines)
if write:
    with open("docs/build-options.md", "w", encoding="utf-8") as f:
        f.write(output)
    print(f"Wrote docs/build-options.md ({len(options)} options, {len(order)} presets).", file=sys.stderr)
else:
    print(output)
PYEOF
