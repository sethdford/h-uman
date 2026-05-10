#!/usr/bin/env bash
# check-layer-topology.sh — enforce the 7-layer dependency-direction rule
# from docs/plans/2026-05-10-memory-v2-roadmap-overview.md §"Cross-cutting
# principles" and docs/standards/engineering/principles.md §"Architecture
# Boundaries".
#
# Rule (P2E): a workstream may never depend on a layer above its own.
# Enforcement: scan every .c file in a layer; reject any #include that
# resolves to a header owned by a higher layer.
#
# Layers (memory v2 roadmap):
#   L0 — v1 storage substrate          (graph + sqlite engines + persona deltas)
#   L1 — Memory facade (W7) + W10      (hu_memory_facade_t, neural memory)
#   L2 — Belief layer (W8)             (hu_belief_t, hyperedges)
#   L3 — World model (W9)              (hu_world_model_t)
#   L4 — Learning loop (W13/W14)       (learners, scheduler, runners)
#   L5 — Response-path (W11/W12)       (self-RAG, planner)
#   L6 — Privacy & Governance (W15)    (audit log, keystore, sandbox)
#   L7 — Evaluation (W16)              (eval suite)
#
# A header at layer N may be #included from any file at layer N or above.
# A .c at layer N may NOT #include a header at any layer > N.
#
# Compatible with macOS /bin/bash 3.2 (no mapfile, no namerefs).
#
# Exit codes:
#   0 — clean
#   1 — at least one cross-layer violation
#   2 — script invocation error
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ──────────────────────────────────────────────────────────────────────────
# Layer membership. The owning layer is keyed by header path; each .c file
# is classified by its sibling header where possible, else by directory.
# Patterns are POSIX extended regex (ERE) matched against repo-relative path.
# Order matters: first match wins, so place more-specific rules first.
# ──────────────────────────────────────────────────────────────────────────

# Header rules: "<layer>|<ERE>"
HEADER_RULES="
7|^include/human/agent/eval_.*\\.h$
6|^include/human/security/(audit|keystore|envelope|dpsgd)[^/]*\\.h$
5|^include/human/agent/(self_rag|planner|corrective_rag|adaptive_rag|response_verifier|claim_verifier)[^/]*\\.h$
5|^include/human/memory/(self_rag|corrective_rag|adaptive_rag)[^/]*\\.h$
5|^include/human/agent/belief_reverify_runner\\.h$
4|^include/human/agent/(scheduler|lora_runner|kv_runner|kv_prewarm_runner|cf_rehearsal_runner|autodream_runner|lora_training_runner)[^/]*\\.h$
4|^include/human/agent/learner.*\\.h$
4|^include/human/ml/.*\\.h$
3|^include/human/agent/world_model[^/]*\\.h$
3|^include/human/agent/goals[^/]*\\.h$
2|^include/human/memory/(belief|hyperedge)[^/]*\\.h$
1|^include/human/memory/memory\\.h$
1|^include/human/memory/neural_memory[^/]*\\.h$
1|^include/human/memory/kv_cache[^/]*\\.h$
0|^include/human/memory/.*\\.h$
0|^include/human/memory\\.h$
"

# Bridge/wiring files: explicitly exempt from the rule because their sole
# purpose is to cross layers (e.g. expose a v1 surface to a v2 layer above
# it, or wire a higher-layer backend into the W7 facade). Each entry MUST
# have a one-line rationale next to it. Adding a file here is a deliberate
# architectural decision that should be reviewed.
BRIDGE_FILES="
src/agent/world_model_bridge.c=W9 wire bridge: only TU where W7+W9+W14+W11 headers are visible together; documented in world_model_bridge.h.
src/agent/belief_reverify_runner.c=W14→W11 bridge: learning-loop runner calls self-RAG verifier on belief relations; sole TU coupling L4 runner to L5 verifier surface.
src/memory/memory_v1_backend.c=W7 v1 backend factory: registers L0/L1/L2 storage backends (graph + hyperedge) into the facade vtable.
"

# Source rules: "<layer>|<ERE>"
SOURCE_RULES="
7|^src/eval\\.c$
7|^src/agent/eval_.*\\.c$
6|^src/security/(audit|keystore|envelope|dpsgd)[^/]*\\.c$
5|^src/agent/(self_rag|planner|corrective_rag|adaptive_rag|response_verifier|claim_verifier)[^/]*\\.c$
5|^src/memory/(self_rag|corrective_rag|adaptive_rag)[^/]*\\.c$
5|^src/agent/belief_reverify_runner\\.c$
4|^src/agent/(scheduler|lora_runner|kv_runner|kv_prewarm_runner|cf_rehearsal_runner|autodream_runner|lora_training_runner)[^/]*\\.c$
4|^src/agent/learner.*\\.c$
4|^src/ml/.*\\.c$
3|^src/agent/world_model[^/]*\\.c$
3|^src/agent/goals[^/]*\\.c$
2|^src/memory/(belief|hyperedge)[^/]*\\.c$
1|^src/memory/memory\\.c$
1|^src/memory/memory_v1_backend\\.c$
1|^src/memory/neural_memory[^/]*\\.c$
1|^src/memory/kv_cache[^/]*\\.c$
0|^src/memory/(graph|persona_deltas|deep_memory|emotional_residue|negative_memory|episodes|consolidation|consolidation_engine|trust|forgetting|cross_edges|case_store|quarantine)[^/]*\\.c$
0|^src/memory/engines/.*\\.c$
"

classify_header() {
    _path="$1"
    printf '%s\n' "$HEADER_RULES" | while IFS='|' read -r layer pattern; do
        [ -z "$layer" ] && continue
        if printf '%s' "$_path" | grep -qE "$pattern"; then
            printf '%s' "$layer"
            return
        fi
    done
}

classify_source() {
    _path="$1"
    printf '%s\n' "$SOURCE_RULES" | while IFS='|' read -r layer pattern; do
        [ -z "$layer" ] && continue
        if printf '%s' "$_path" | grep -qE "$pattern"; then
            printf '%s' "$layer"
            return
        fi
    done
}

# Resolve an `#include "human/foo/bar.h"` directive to its repo-relative
# header path. Returns empty string for system / non-project includes.
resolve_include() {
    # Use a distinct name from the scan loop's `target` and default $1 so
    # `set -u` never trips on an empty/missing argument after a `read`.
    _inc="${1-}"
    case "$_inc" in
        human/*) printf 'include/%s' "$_inc" ;;
        *) printf '' ;;
    esac
}

# ──────────────────────────────────────────────────────────────────────────
# Scan
# ──────────────────────────────────────────────────────────────────────────

violations=0
sources_scanned=0

# Use git ls-files to honor .gitignore + skip generated artifacts. Include
# both tracked and untracked-but-not-ignored files so that files staged for
# review (or just dropped into src/) are also enforced — otherwise a new
# violation would slip past the check until the file was committed.
SRC_LIST="$(git ls-files --cached --others --exclude-standard 'src/**/*.c' | sort -u)"

is_bridge_file() {
    _path="$1"
    printf '%s\n' "$BRIDGE_FILES" | while IFS='=' read -r path _reason; do
        [ -z "$path" ] && continue
        if [ "$path" = "$_path" ]; then
            printf 'yes'
            return
        fi
    done
}

for src in $SRC_LIST; do
    [ -f "$src" ] || continue
    src_layer="$(classify_source "$src")"
    [ -z "$src_layer" ] && continue
    sources_scanned=$((sources_scanned + 1))

    if [ "$(is_bridge_file "$src")" = "yes" ]; then
        continue
    fi

    # Extract include directives (quoted form only — system <...> headers are
    # never project layers). One-line awk pass per file.
    awk '
        /^[[:space:]]*#[[:space:]]*include[[:space:]]+"[^"]+"/ {
            match($0, /"[^"]+"/);
            print substr($0, RSTART + 1, RLENGTH - 2);
        }
    ' "$src" | while read -r target; do
        header_path="$(resolve_include "$target")"
        [ -z "$header_path" ] && continue
        hdr_layer="$(classify_header "$header_path")"
        [ -z "$hdr_layer" ] && continue
        if [ "$hdr_layer" -gt "$src_layer" ]; then
            printf 'LAYER VIOLATION: %s (L%s) -> #include "%s" (L%s)\n' \
                "$src" "$src_layer" "$target" "$hdr_layer"
            # Record violation in a temp file; subshell from `while read`
            # cannot mutate the outer counter on bash 3.2.
            echo "x" >> "${TMPDIR:-/tmp}/.htopo.$$"
        fi
    done
done

if [ -f "${TMPDIR:-/tmp}/.htopo.$$" ]; then
    violations="$(wc -l < "${TMPDIR:-/tmp}/.htopo.$$" | tr -d ' ')"
    rm -f "${TMPDIR:-/tmp}/.htopo.$$"
fi

if [ "$violations" -gt 0 ]; then
    printf '\n%s cross-layer #include violation(s) detected.\n' "$violations" >&2
    printf 'See docs/plans/2026-05-10-memory-v2-roadmap-overview.md §"Cross-cutting principles" (P2E rule).\n' >&2
    exit 1
fi

printf 'Layer topology OK: 0 cross-layer violations across %s source files.\n' "$sources_scanned"
exit 0
