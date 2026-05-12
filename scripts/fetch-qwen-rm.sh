#!/usr/bin/env bash
# Phase 3 Task 8 will replace this stub with the real download. Until
# then, calling this script just tells the operator what's missing.
#
# Why a stub now (Task 0, L3 fix from the plan): the SHA-256 sidecar
# scripts/fetch-qwen-rm.sh.sha256 is created in the SAME Task 0 commit,
# and a dangling sidecar with no script in-tree confuses `git bisect`
# and the dead-code-finder. Stubbing the script up-front (exit 1 + a
# pointer to the plan section that will fill it in) keeps the pair
# consistent across every Phase 3 task between here and Task 8.
#
# The eventual implementation will mirror scripts/fetch-gemma.sh
# byte-for-byte at the structural level (curl + sha256sum/shasum
# verify + --check-only mode + non-destructive on mismatch). See
# docs/plans/2026-05-11-rl-loop-phase-3-kto-rm.md Task 8 step 1.
set -euo pipefail
echo "fetch-qwen-rm.sh stub — Task 8 fills in the real fetch (Qwen-2.5-0.5B-Instruct Q4_K_M GGUF)" >&2
echo "If you need the model NOW, see docs/plans/2026-05-11-rl-loop-phase-3-kto-rm.md Task 8 step 1." >&2
exit 1
