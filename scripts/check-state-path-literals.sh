#!/usr/bin/env bash
# check-state-path-literals.sh
#
# Absolute gate (floor reached 2026-09-06): no hand-rolled "$HOME/.human/..." or
# "$HOME/Library/Messages/chat.db" path may be formatted outside src/core/paths.c.
# Every state / chat.db path goes through hu_paths_* (include/human/core/paths.h)
# so HU_STATE_DIR and HU_CHATDB are honored everywhere. Ceiling is 0 and this is
# not a decaying ratchet (see .claude/rules/ratchet-decay.md: counters at their
# floor are freeze-only). Rule: .claude/rules/state-paths-through-helper.md
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

# Shapes that assembled such a path before the 2026-09-06 sweep:
#   "%s/.human/..."                 inline literal (single- or multi-line)
#   "%s/Library/Messages/chat.db"   inline chat.db literal
#   "%s/%s", home, HU_X_DIR          macro-joined (".human" hidden in a #define)
#   "%s%s", home, X_SUFFIX           suffix-joined ("/.human/..." in a #define)
literal='%s/\.human|%s/Library/Messages/chat\.db'
hits=$(grep -rnE "$literal" src include --include='*.c' --include='*.h' | grep -v '^src/core/paths.c:' || true)
# Macro-joined shapes: only a macro whose #define carries ".human" is a state
# path ("%s/%s", home, HU_LAUNCHD_DIR joins Library/LaunchAgents and is fine).
joined=$(grep -rnE '"%s/?%s", *(home|hm|hm2|home_dir), *[A-Z_]+' src include --include='*.c' --include='*.h' || true)
while IFS= read -r line; do
  [ -n "$line" ] || continue
  macro=$(printf '%s' "$line" | grep -oE ', *[A-Z_]+' | tail -1 | tr -d ' ,')
  if grep -rqE "#\s*define\s+$macro\b.*\.human" src include --include='*.c' --include='*.h'; then
    hits="${hits}${hits:+
}$line"
  fi
done <<< "$joined"
n=$(printf '%s' "$hits" | grep -c . || true)
echo "hand-rolled state/chat.db path literals outside src/core/paths.c: $n (ceiling 0)"
if [ "$n" -gt 0 ]; then
  echo "FAIL: format the path with hu_paths_state/_or/_dir or hu_paths_chatdb/_or" >&2
  echo "      (include/human/core/paths.h) so HU_STATE_DIR / HU_CHATDB are honored." >&2
  printf '%s\n' "$hits" | sed 's/^/  /' >&2
  exit 1
fi
