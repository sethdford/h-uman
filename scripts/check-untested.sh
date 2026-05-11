#!/usr/bin/env bash
# check-untested.sh — Find src/*.c files with no corresponding test coverage.
# Exit 0 if all files have coverage, exit 1 if gaps found.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

SKIP_PATTERNS="^factory$|^meta_common$|^main$|^main_wasi$|_common$|^bootstrap$|^config_schema$|^config_getters$|^config_migrate$|^cp_admin$|^cp_chat$|^cp_config$|^cp_memory$|^cp_voice$|^cp_hula$|^cp_mcp$|^cp_turing$|^cp_voice_stream$|^cp_voice_clone$|^thread_pool$|^agent_stream$|^superhuman_predictive$|^embedder_gemini_adapter$|^provider_http$|^data_|^embedded_registry$|^anticipatory$|^conversation_plan$|^info_asymmetry$|^theory_of_mind$|^voice_maturity$|^tokenizer_bpe$|^store_mem$|^sqlite_fts$|^sqlite_lucid$|^hula_compiler_examples$|^style_analyzer$|^timing_analyzer$|^accel$|^bff_memory$|^doc_ingest$|^meeting_transcribe$|^webrtc_dtls$|^mcp_tool_wrapper$|^db_introspect$|^retrieval_planner_llm$|^scheduler_probes$|^cli_evaluation$|^cognition_trust$|^evaluation_dataset_loader$|^evaluation_legacy_bridge$|^memory_v1_backend$|^learner_cpu$|^learner_ggml$|^learner_mlx$"
FOUND=0

while IFS= read -r src; do
    base=$(basename "$src" .c)

    if echo "$base" | grep -qE "$SKIP_PATTERNS"; then
        continue
    fi

    # Match: tests/test_<base>.c exists (filename match — covers cases where
    # the test file is the obvious counterpart but uses domain-specific
    # function names internally, e.g. test_response_guard_retry.c uses
    # `guard_reject_retry_*` helpers).
    if [ -f "tests/test_${base}.c" ]; then
        continue
    fi

    if grep -rqwl "${base}" tests/ >/dev/null 2>&1; then
        continue
    fi
    # Function-prefix patterns:
    #   - hu_<base>_<rest>        (typical helper)
    #   - hu_<base>(              (function whose name IS the base, called as fn(...))
    #   - hu_<base>;              (declaration / assignment to function pointer)
    #   - test_<base>_<rest>      (direct test wrapper)
    #   - TEST.*<base>            (named test case)
    # Without this, modules like src/evaluation/evaluation_dmr.c whose only
    # tested symbol is `hu_evaluation_dmr` (no trailing underscore) are
    # falsely reported as untested.
    if grep -rqE "hu_${base}_|hu_${base}\(|hu_${base};|test_${base}_|TEST.*${base}" tests/ >/dev/null 2>&1; then
        continue
    fi

    echo "  NO TEST: $src"
    FOUND=$((FOUND + 1))
done < <(find src -name '*.c' | sort)

if [ "$FOUND" -eq 0 ]; then
    echo "All source files have test references."
else
    echo ""
    echo "$FOUND source file(s) with no test references found."
fi

if [ "$FOUND" -gt 0 ]; then
    exit 1
fi
