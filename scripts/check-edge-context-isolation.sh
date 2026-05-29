#!/usr/bin/env bash
# check-edge-context-isolation.sh
#
# Edge-context guard (DDD bounded-context refactor, Phase 0). A concrete
# channel implementation may include the vtable contract and shared infra, but
# must not take on a NEW dependency on a *different* channel's header. This
# protects the one genuinely clean hexagonal boundary in the codebase
# (providers/channels/tools depend on contracts, never on each other).
#
# Ratchet: the current cross-channel includes are all legitimate same-family
# splits (the imessage channel spans imessage*.c/.h; Meta platforms share
# meta_common.h) — they are grandfathered at BASELINE. The guard fails only if
# a genuinely new cross-channel coupling appears (e.g. discord.c including
# slack.h).
#
# Exempt from the count:
#   - a file including its OWN header (slack.c -> slack.h)
#   - shared infra headers: format, dispatch, contact_signature,
#     channel_embed, behavior_class, reaction_event, meta_common
set -euo pipefail

# Measured 2026-05-29 at the start of Phase 0 (all imessage-family + reuse).
BASELINE=6

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

count_cross() {
  local total=0 f base
  for f in src/channels/*.c; do
    [ -e "$f" ] || continue
    base=$(basename "$f" .c)
    local c
    c=$(grep -oE '#include "human/channels/[a-z_]+\.h"' "$f" 2>/dev/null \
      | sed -E 's#.*human/channels/([a-z_]+)\.h.*#\1#' \
      | while IFS= read -r inc; do
          case "$inc" in
            "$base"|format|dispatch|contact_signature|channel_embed|behavior_class|reaction_event|meta_common) ;;
            *) echo x ;;
          esac
        done | wc -l | tr -d ' ')
    total=$((total + c))
  done
  echo "$total"
}

list_cross() {
  local f base
  for f in src/channels/*.c; do
    [ -e "$f" ] || continue
    base=$(basename "$f" .c)
    { grep -oE '#include "human/channels/[a-z_]+\.h"' "$f" 2>/dev/null || true; } \
      | sed -E 's#.*human/channels/([a-z_]+)\.h.*#\1#' \
      | while IFS= read -r inc; do
          case "$inc" in
            "$base"|format|dispatch|contact_signature|channel_embed|behavior_class|reaction_event|meta_common) ;;
            *) echo "  $f -> $inc.h" ;;
          esac
        done
  done
}

count=$(count_cross)
echo "cross-channel includes (excl self + shared): $count (ceiling $BASELINE)"

if [ "$count" -gt "$BASELINE" ]; then
  echo "FAIL: a new cross-channel dependency appeared. A concrete channel must" >&2
  echo "      not depend on a different channel's header — use the vtable" >&2
  echo "      contract or shared infra. Current cross-channel includes:" >&2
  list_cross >&2
  exit 1
fi

if [ "$count" -lt "$BASELINE" ]; then
  echo "NOTE: cross-channel count dropped — lower BASELINE to $count to lock it." >&2
fi

exit 0
