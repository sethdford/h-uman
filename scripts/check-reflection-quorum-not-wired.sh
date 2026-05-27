#!/usr/bin/env bash
# Phase 1 contract: hu_reflection_pattern_has_quorum is TELEMETRY ONLY.
#
# It must not be used to gate mutations against hu_personal_model_t
# until Phase 2 lands. This script fails CI if any src/ file outside
# src/reflection/ mentions has_quorum near personal_model symbols.
#
# Why this matters: belief updates (mutating learned facts based on
# enough corroborating observations) are exactly the kind of thing
# that needs the Phase 2 spec — including a rollback path, a
# "user contradicted the belief" surface, and an eval harness. Wiring
# quorum→personal_model without those guardrails would let the agent
# silently change what it thinks Seth believes based on three runs
# of a model that might all be wrong in the same direction.
#
# Phase 2 plan: lifting this gate requires explicit removal of the
# check + a Phase 2 spec at docs/plans/<date>-reflection-belief-update/.
# See docs/plans/2026-05-26-reflection-loop/design.md "Phase 2 plan".

set -euo pipefail

# Find all files outside src/reflection/ that reference has_quorum
HITS=$(grep -rln 'hu_reflection_pattern_has_quorum' src/ 2>/dev/null \
       | grep -v '^src/reflection/' || true)

VIOLATIONS=""
for f in $HITS; do
    if grep -q 'hu_personal_model_' "$f"; then
        VIOLATIONS+=" $f"
    fi
done

if [ -n "$VIOLATIONS" ]; then
    echo "ERROR: Phase 1 contract violated."
    echo
    echo "The following files use hu_reflection_pattern_has_quorum AND"
    echo "reference hu_personal_model_ symbols — this means quorum may be"
    echo "gating a belief mutation, which is reserved for Phase 2."
    echo
    echo "Files:$VIOLATIONS"
    echo
    echo "Phase 2 lift: remove this gate AND add a Phase 2 spec at"
    echo "docs/plans/<date>-reflection-belief-update/{requirements,design,tasks}.md"
    exit 1
fi

echo "OK: no Phase 1 quorum-mutation violations."
