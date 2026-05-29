#!/usr/bin/env bash
# check-modeled-person-layering.sh
#
# Modeled-Person layering guard (DDD bounded-context refactor). The persona /
# cognition / behavior directories are ONE bounded context with a unidirectional
# per-turn flow:
#
#     persona/    expression  (identity, voice, style, boundaries)
#        │
#        ▼
#     cognition/  perception  (emotion, trust, attachment, presence)
#        │
#        ▼
#     behavior/   decision    (relational acts, intensity, modulation)
#
# The layers communicate ONLY through the aggregate roots (human/persona.h,
# human/memory/personal_model.h) + shared core/data infra. The other three
# context boundaries each have an enforcing ratchet; this one did not — the
# layering was documented (docs/standards/engineering/bounded-contexts.md) but
# unprotected. This guard freezes it.
#
# Forbidden include directions (each frozen at its measured baseline):
#   cognition/ -> human/behavior/   (sibling cross-talk)
#   behavior/  -> human/cognition/  (sibling cross-talk)
#   persona/   -> human/cognition/  (the root must not depend on perception)
#   persona/   -> human/behavior/   (the root must not depend on decision)
#
# Allowed (downward, to the aggregate roots / infra — NOT counted here):
#   cognition/ , behavior/  -> human/persona.h , human/memory/personal_model.h
#   any layer               -> human/core/* , human/data/*
#
# Scans BOTH the layer sources (src/<layer>/*.c) and its public headers
# (include/human/<layer>/*.h) — a header pulling another layer's header is the
# same coupling. Fails only on GROWTH past the baseline.
set -euo pipefail

# Measured 2026-05-29 — sibling/perception directions clean. Lower a baseline to
# lock any future drop; the guard only fails on growth.
COG_TO_BEH_BASELINE=0
BEH_TO_COG_BASELINE=0
PER_TO_COG_BASELINE=0
# GRANDFATHERED at 6 during the 2026-05-29 reconcile: main's B-series prosocial
# work (persona/celebration.c, warm_response.c + their headers) depends on
# behavior/ TYPE headers (prosocial_moment hu_pmoment_kind_t, safety
# hu_behavior_risk_t, win_detect hu_win_kind_t). These predate this ratchet on
# main. DEBT: retire by moving those shared enums to a shared layer
# (human/core/* or an aggregate root) so persona needn't depend on behavior/;
# then lower this back to 0. Until then, fail only on GROWTH past 6.
PER_TO_BEH_BASELINE=6

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

# count_cross <layer> <target>: number of `#include "human/<target>/<x>.h"`
# across the layer's sources + public headers.
count_cross() {
  local layer="$1" target="$2"
  { grep -rhoE "#include \"human/${target}/[a-z_]+\.h\"" \
      "src/${layer}" "include/human/${layer}" 2>/dev/null || true; } \
    | wc -l | tr -d ' '
}

list_cross() {
  local layer="$1" target="$2"
  { grep -rnE "#include \"human/${target}/[a-z_]+\.h\"" \
      "src/${layer}" "include/human/${layer}" 2>/dev/null || true; } \
    | sed 's#^#  #'
}

fail=0
check() {  # <layer> <target> <baseline> <human-readable reason>
  local layer="$1" target="$2" baseline="$3" reason="$4" count
  count=$(count_cross "$layer" "$target")
  echo "${layer}/ -> human/${target}/ includes: ${count} (ceiling ${baseline})"
  if [ "$count" -gt "$baseline" ]; then
    echo "FAIL: ${reason}" >&2
    echo "      ${layer}/ must reach ${target} only through the aggregate roots" >&2
    echo "      (human/persona.h, human/memory/personal_model.h), never by" >&2
    echo "      including a ${target}/ header directly. Offending includes:" >&2
    list_cross "$layer" "$target" >&2
    fail=1
  elif [ "$count" -lt "$baseline" ]; then
    echo "NOTE: ${layer}->${target} dropped to ${count} — lower the baseline to lock it." >&2
  fi
}

check cognition behavior "$COG_TO_BEH_BASELINE" "cognition/ took a NEW dependency on behavior/ (sibling cross-talk)"
check behavior  cognition "$BEH_TO_COG_BASELINE" "behavior/ took a NEW dependency on cognition/ (sibling cross-talk)"
check persona   cognition "$PER_TO_COG_BASELINE" "persona/ (expression root) took a NEW dependency on cognition/"
check persona   behavior  "$PER_TO_BEH_BASELINE" "persona/ (expression root) took a NEW dependency on behavior/"

[ "$fail" -eq 0 ] || exit 1
exit 0
