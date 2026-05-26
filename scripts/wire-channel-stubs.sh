#!/usr/bin/env bash
# Wire NULL initializers for reply/react_emoji/send_sticker vtable slots
# to every channel that has a .react field but doesn't yet have the new slots.
# Skip imessage.c (gets real impls in phases C/D/E).
set -euo pipefail

ROOT="/Users/sethford/Projects/h-uman/.claude/worktrees/imessage-action-surface"
cd "$ROOT"

PATCHED=0
for f in src/channels/*.c; do
    name=$(basename "$f")
    [ "$name" = "imessage.c" ] && continue

    # Check if this file has a .react assignment but doesn't yet have .reply
    if grep -q '\.react\s*=' "$f" && ! grep -q '\.reply\s*=' "$f"; then
        echo "Patching $f"
        # Find the line with .react and insert the three new stubs after it.
        # Using sed with a portable format for macOS BSD sed.
        # The pattern: match "\.react = " and on that line, append three new lines.
        sed -i.bak '/\.react[[:space:]]*=/a\
    .reply = NULL,\
    .react_emoji = NULL,\
    .send_sticker = NULL,' "$f"
        rm -f "${f}.bak"
        PATCHED=$((PATCHED + 1))
    fi
done

echo "Patched $PATCHED channels"
