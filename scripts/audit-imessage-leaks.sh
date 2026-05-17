#!/usr/bin/env bash
# audit-imessage-leaks.sh — sweep outbound iMessage for daemon leaks.
#
# Reads ~/Library/Messages/chat.db (read-only), dumps the
# attributedBody column for every outbound message in the time
# window, runs each through the same leak signatures the C
# response_guard checks for (G1/G2/G3/G4), and reports any matches.
#
# This is the same audit that surfaced the 3 leaks Sprint 30
# pinned (rowid 56055, 56065, 56355). Designed to run weekly via
# cron / launchd, or on-demand after any CoT leak suspicion.
#
# Usage:
#   scripts/audit-imessage-leaks.sh [--since "YYYY-MM-DD"]
#                                   [--contacts "+1...,+1..."]
#                                   [--out-dir DIR]
#                                   [--self-test]
#
#   --self-test  Run signature checks on a synthetic leak blob (no chat.db).
#                Used by CI on macOS to verify the script without Messages data.
#
# Defaults:
#   --since      2026-05-10  (covers all known incidents)
#   --contacts   "" (all contacts)
#   --out-dir    /tmp/imsg-audit-$$
#
# Exit codes:
#   0  no leak signatures found
#   1  one or more leak signatures found (caller should investigate)
#   2  setup error (chat.db unreadable, sqlite missing, etc.)
#
# Dependencies: sqlite3, coreutils, awk, grep.

set -euo pipefail

# ── Self-test (no sqlite / chat.db) ─────────────────────────────
audit_scan_text() {
    local txt="$1"
    local g2 g3 g4 g1
    g2=$(printf '%s\n' "$txt" | grep -c -i -E \
        'the prompt says|the prompt asked|wait, the prompt|i should still maintain|per scene direction|the user is bombarding' \
        || true)
    g3=$(printf '%s\n' "$txt" | grep -c -i -E \
        "is a (technical professional|software engineer|chief architect|data scientist|product manager|senior engineer|software developer|machine learning)|lives alone with|romantic interest|he's talking to |she's talking to |they're talking to " \
        || true)
    g4=$(printf '%s\n' "$txt" | grep -c -E \
        '^[[:space:]]*(Persona:|Scene Direction:|User: "|Rules: All lowercase|Constraints: All lowercase|System prompt:)' \
        || true)
    g1=$(printf '%s\n' "$txt" | awk '
        /^[[:space:]]*[0-9]+\.[ ]/ { if (length($0) > 30) n++ }
        END { print n+0 }')
    if [[ "$g1" -ge 3 ]] || [[ "$g2" -gt 0 ]] || [[ "$g3" -ge 2 ]] || [[ "$g4" -gt 0 ]]; then
        return 0
    fi
    return 1
}

if [[ "${1:-}" == "--self-test" ]]; then
    synthetic=$'1. King Carpet and Flooring is a local business.\n'
    synthetic+=$'2. The prompt says Persona: test user.\n'
    synthetic+=$'3. Seth is a technical professional who lives alone with a cat.\n'
    synthetic+=$'Persona: Seth\nScene Direction: be brief\n'
    if audit_scan_text "$synthetic"; then
        echo "audit-imessage-leaks.sh: --self-test OK (synthetic leak detected)"
        exit 0
    fi
    echo "audit-imessage-leaks.sh: --self-test FAILED (synthetic leak not detected)" >&2
    exit 2
fi

# ── Configuration ──────────────────────────────────────────────
SINCE="2026-05-10"
CONTACTS=""
OUT_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --since)    SINCE="$2"; shift 2 ;;
        --contacts) CONTACTS="$2"; shift 2 ;;
        --out-dir)  OUT_DIR="$2"; shift 2 ;;
        --self-test)
            echo "audit-imessage-leaks.sh: --self-test must be the only argument" >&2
            exit 2 ;;
        --help|-h)
            sed -n '2,30p' "$0"; exit 0 ;;
        *)
            echo "audit-imessage-leaks.sh: unknown arg: $1" >&2
            echo "  use --help for usage" >&2
            exit 2 ;;
    esac
done

if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="$(mktemp -d -t imsg-audit-XXXXXX)"
fi

CHAT_DB="$HOME/Library/Messages/chat.db"

# ── Pre-flight ─────────────────────────────────────────────────

if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "ERROR: sqlite3 not found in PATH" >&2
    exit 2
fi

if [[ ! -r "$CHAT_DB" ]]; then
    echo "ERROR: cannot read $CHAT_DB" >&2
    echo "  This script requires Full Disk Access. On macOS:" >&2
    echo "  System Settings > Privacy & Security > Full Disk Access" >&2
    exit 2
fi

mkdir -p "$OUT_DIR"

# ── Build the rowid list ───────────────────────────────────────
# Apple stores `date` as nanoseconds since 2001-01-01. The +
# 978307200 offset converts to Unix seconds.
SINCE_UNIX="$(date -j -f "%Y-%m-%d" "$SINCE" "+%s" 2>/dev/null || \
              date -d "$SINCE" "+%s")"
SINCE_APPLE="$(( (SINCE_UNIX - 978307200) * 1000000000 ))"

CONTACT_FILTER=""
if [[ -n "$CONTACTS" ]]; then
    # Build a SQL IN clause: "+1...,+1..." -> "'+1...','+1...'"
    quoted=""
    IFS=',' read -ra arr <<< "$CONTACTS"
    for c in "${arr[@]}"; do
        quoted+="'${c// /}',"
    done
    quoted="${quoted%,}"
    CONTACT_FILTER="AND h.id IN ($quoted)"
fi

# shellcheck disable=SC2016
ROWIDS_FILE="$OUT_DIR/rowids.txt"
sqlite3 "$CHAT_DB" <<SQL > "$ROWIDS_FILE"
SELECT m.ROWID
FROM message m
LEFT JOIN handle h ON m.handle_id = h.ROWID
WHERE m.is_from_me = 1
  AND m.attributedBody IS NOT NULL
  AND m.date > $SINCE_APPLE
  $CONTACT_FILTER
ORDER BY m.date ASC;
SQL

ROWID_COUNT="$(wc -l < "$ROWIDS_FILE" | tr -d ' ')"
if [[ "$ROWID_COUNT" -eq 0 ]]; then
    echo "audit: no outbound messages with attributedBody since $SINCE"
    rm -rf "$OUT_DIR"
    exit 0
fi

echo "audit: scanning $ROWID_COUNT outbound messages since $SINCE..."

# ── Dump attributedBody bytes for each rowid ───────────────────
DUMP_DIR="$OUT_DIR/dumps"
mkdir -p "$DUMP_DIR"
while IFS= read -r rowid; do
    sqlite3 "$CHAT_DB" \
      "SELECT writefile('$DUMP_DIR/msg-${rowid}.bin', attributedBody)
       FROM message WHERE ROWID=${rowid};" >/dev/null
done < "$ROWIDS_FILE"

# ── Signature scan ─────────────────────────────────────────────
# Patterns mirror the C response_guard's detectors:
#
#   G2 (Sprint 29) — model self-talk substrings.
#   G3 (Sprint 29) — third-person profile patterns.
#   G4 (Sprint 30) — prompt-template labels.
#   G1 (Sprint 29) — numbered analytical-list items >= 30 chars
#                    of content. Counts >=3 long items as a hit.
#
# G5 (length anomaly) and G6 (director echo) require per-turn
# context this script doesn't have, so they are not checked here.

REPORT="$OUT_DIR/report.txt"
: > "$REPORT"
hits_total=0

for f in "$DUMP_DIR"/msg-*.bin; do
    [[ -f "$f" ]] || continue
    txt="$(strings -n 4 "$f")"

    if audit_scan_text "$txt"; then
        rowid="$(basename "$f" .bin | sed 's/msg-//')"
        meta="$(sqlite3 "$CHAT_DB" "
            SELECT datetime(date/1000000000+978307200,'unixepoch','localtime')
                || '|' || (SELECT id FROM handle WHERE ROWID=handle_id)
            FROM message WHERE ROWID=${rowid};")"
        size="$(stat -f '%z' "$f" 2>/dev/null || stat -c '%s' "$f")"

        printf '%s rowid=%s size=%d leak_signature=hit\n' \
               "$meta" "$rowid" "$size" \
               | tee -a "$REPORT"
        hits_total=$((hits_total + 1))
    fi
done

echo ""
echo "audit: $hits_total flagged of $ROWID_COUNT scanned"
echo "audit: dumps live in $DUMP_DIR"
echo "audit: report at $REPORT"

if [[ "$hits_total" -gt 0 ]]; then
    exit 1
fi
exit 0
