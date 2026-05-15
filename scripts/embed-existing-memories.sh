#!/usr/bin/env bash
# embed-existing-memories.sh — diagnostic backfill of the additive `embeddings`
# table in ~/.human/memory.db.
#
# US-3.4 (Sprint 3). This is a DIAGNOSTIC / VALIDATION tool. The daemon does
# NOT read the `embeddings` table — it re-embeds memories at startup from the
# `memories` table (see US-3.3). This script exists so users and operators
# can inspect what the local TF-IDF embedder makes of their memories.
#
# Usage:
#   embed-existing-memories.sh [--db PATH] [--helper PATH] [--batch N]
#
# Edge cases handled:
#   AC-3.4.4: empty memory.db (no scoped memories) → exit 0 with message
#   AC-3.4.5: missing memory.db                    → exit 1 with clear error
#   AC-3.4.6: >1000 rows                            → batched + progress
#   Idempotent: existing rows in `embeddings` (matched by memory_key) skipped
#
# Schema (additive, INSERTed if missing):
#   CREATE TABLE IF NOT EXISTS embeddings (
#       memory_key       TEXT PRIMARY KEY,
#       embedding        BLOB NOT NULL,
#       dimensions       INTEGER NOT NULL,
#       embedder_version TEXT NOT NULL,
#       created_at       TEXT NOT NULL
#   );

set -euo pipefail

DB_PATH="${HOME}/.human/memory.db"
HELPER_BIN=""
BATCH_SIZE=100
EMBEDDER_VERSION="tfidf-local-v1"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--db PATH] [--helper PATH] [--batch N]

Options:
  --db PATH       Path to memory.db (default: ~/.human/memory.db)
  --helper PATH   Path to hu_embed_helper binary
                  (default: search build/, build-dev/, and PATH)
  --batch N       Progress report every N rows (default: 100)
  -h, --help      Show this help

Diagnostic tool. The daemon does not consume the output.
EOF
}

log() { printf '[embed-backfill] %s\n' "$*" >&2; }
err() { printf '[embed-backfill] ERROR: %s\n' "$*" >&2; }

# SQL-escape a string for safe inclusion in single-quoted SQL literals.
sql_escape() {
    printf "%s" "$1" | sed "s/'/''/g"
}

# Parse args -----------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
    --db)
        DB_PATH="$2"
        shift 2
        ;;
    --helper)
        HELPER_BIN="$2"
        shift 2
        ;;
    --batch)
        BATCH_SIZE="$2"
        shift 2
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        err "unknown argument: $1"
        usage >&2
        exit 1
        ;;
    esac
done

# Locate the helper binary ---------------------------------------------------
locate_helper() {
    if [[ -n "$HELPER_BIN" ]]; then
        if [[ -x "$HELPER_BIN" ]]; then
            return 0
        fi
        err "specified helper not executable: $HELPER_BIN"
        return 1
    fi
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local repo_root
    repo_root="$(cd "$script_dir/.." && pwd)"

    local candidates=(
        "$repo_root/build/hu_embed_helper"
        "$repo_root/build-dev/hu_embed_helper"
        "$repo_root/build-test/hu_embed_helper"
    )
    for c in "${candidates[@]}"; do
        if [[ -x "$c" ]]; then
            HELPER_BIN="$c"
            return 0
        fi
    done
    if command -v hu_embed_helper >/dev/null 2>&1; then
        HELPER_BIN="$(command -v hu_embed_helper)"
        return 0
    fi
    err "could not find hu_embed_helper. Build it first:"
    err "  cmake --build --preset dev --target hu_embed_helper"
    err "Or pass --helper PATH."
    return 1
}

# Dependency checks ----------------------------------------------------------
if ! command -v sqlite3 >/dev/null 2>&1; then
    err "sqlite3 CLI is required but not installed"
    exit 1
fi

# AC-3.4.5: missing memory.db
if [[ ! -f "$DB_PATH" ]]; then
    err "memory.db not found at: $DB_PATH"
    err "Run h-uman at least once to create the memory store."
    exit 1
fi

if ! locate_helper; then
    exit 1
fi
log "using helper: $HELPER_BIN"
log "using database: $DB_PATH"

# Detect the table + columns that hold scoped memories. Different builds use
# different schemas; we try the most common ones in order. We require at
# minimum a key/id column and a content column.
detect_source() {
    local pair table key_col content_col
    while IFS='|' read -r table key_col content_col; do
        if sqlite3 "$DB_PATH" \
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='$table' LIMIT 1" \
            2>/dev/null | grep -q 1; then
            if sqlite3 "$DB_PATH" "PRAGMA table_info($table)" 2>/dev/null |
                awk -F'|' '{print $2}' | grep -qx "$key_col"; then
                if sqlite3 "$DB_PATH" "PRAGMA table_info($table)" 2>/dev/null |
                    awk -F'|' '{print $2}' | grep -qx "$content_col"; then
                    printf '%s|%s|%s\n' "$table" "$key_col" "$content_col"
                    return 0
                fi
            fi
        fi
    done <<EOF
memories|key|content
memories|id|content
memory_entries|key|content
memory_entries|id|content
scoped_memories|key|content
scoped_memories|id|content
EOF
    return 1
}

SRC_INFO="$(detect_source || true)"
if [[ -z "$SRC_INFO" ]]; then
    err "could not find a scoped memories table in $DB_PATH"
    err "expected one of: memories, memory_entries, scoped_memories"
    exit 1
fi
IFS='|' read -r SRC_TABLE SRC_KEY SRC_CONTENT <<<"$SRC_INFO"
log "source: $SRC_TABLE($SRC_KEY, $SRC_CONTENT)"

# Ensure additive embeddings table exists ------------------------------------
sqlite3 "$DB_PATH" <<'EOF'
CREATE TABLE IF NOT EXISTS embeddings (
    memory_key TEXT PRIMARY KEY,
    embedding BLOB NOT NULL,
    dimensions INTEGER NOT NULL,
    embedder_version TEXT NOT NULL,
    created_at TEXT NOT NULL
);
EOF

# Count work -----------------------------------------------------------------
TOTAL_ROWS="$(sqlite3 "$DB_PATH" \
    "SELECT COUNT(*) FROM $SRC_TABLE WHERE $SRC_CONTENT IS NOT NULL AND length($SRC_CONTENT) > 0")"
TOTAL_ROWS="${TOTAL_ROWS:-0}"

# AC-3.4.4: empty memory.db (no scoped memories)
if [[ "$TOTAL_ROWS" -eq 0 ]]; then
    log "nothing to embed: $SRC_TABLE has 0 rows with non-empty $SRC_CONTENT"
    exit 0
fi

log "found $TOTAL_ROWS row(s); existing embeddings will be skipped (idempotent)"

# Iterate --------------------------------------------------------------------
PROCESSED=0
INSERTED=0
SKIPPED=0
FAILED=0

# Dump just the keys (one per line). Keys may not contain newlines in any
# h-uman schema; if they do, that's a separate data-integrity issue.
KEYS_TMP="$(mktemp)"
TEXT_TMP="$(mktemp)"
EMB_TMP="$(mktemp)"
trap 'rm -f "$KEYS_TMP" "$TEXT_TMP" "$EMB_TMP"' EXIT

sqlite3 -noheader "$DB_PATH" \
    "SELECT $SRC_KEY FROM $SRC_TABLE WHERE $SRC_CONTENT IS NOT NULL AND length($SRC_CONTENT) > 0;" \
    >"$KEYS_TMP"

while IFS= read -r key; do
    [[ -z "$key" ]] && continue
    PROCESSED=$((PROCESSED + 1))

    key_esc="$(sql_escape "$key")"

    # Idempotent: skip if already embedded.
    existing="$(sqlite3 "$DB_PATH" \
        "SELECT 1 FROM embeddings WHERE memory_key = '${key_esc}' LIMIT 1" 2>/dev/null || true)"
    if [[ "$existing" == "1" ]]; then
        SKIPPED=$((SKIPPED + 1))
    else
        # Pull content into a file so the helper reads it via stdin.
        if ! sqlite3 -noheader "$DB_PATH" \
            "SELECT $SRC_CONTENT FROM $SRC_TABLE WHERE $SRC_KEY = '${key_esc}' LIMIT 1" \
            >"$TEXT_TMP" 2>/dev/null; then
            FAILED=$((FAILED + 1))
            err "failed to read content for key: $key"
        elif [[ ! -s "$TEXT_TMP" ]]; then
            FAILED=$((FAILED + 1))
            err "empty content for key: $key"
        else
            if "$HELPER_BIN" <"$TEXT_TMP" >"$EMB_TMP" 2>/dev/null; then
                # Store the JSON one-liner as the BLOB. Diagnostic format; a
                # future revision may switch to packed float32 bytes. The
                # dimensions+version columns let consumers validate format.
                emb_b64="$(base64 <"$EMB_TMP" | tr -d '\n')"
                created_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
                sqlite3 "$DB_PATH" <<SQL
INSERT OR IGNORE INTO embeddings(memory_key, embedding, dimensions, embedder_version, created_at)
VALUES (
    '${key_esc}',
    CAST('${emb_b64}' AS BLOB),
    384,
    '${EMBEDDER_VERSION}',
    '${created_at}'
);
SQL
                INSERTED=$((INSERTED + 1))
            else
                FAILED=$((FAILED + 1))
                err "embedder failed for key: $key"
            fi
        fi
    fi

    # AC-3.4.6: progress indicator on batches.
    if ((PROCESSED % BATCH_SIZE == 0)); then
        log "progress: $PROCESSED / $TOTAL_ROWS (inserted=$INSERTED skipped=$SKIPPED failed=$FAILED)"
    fi
done <"$KEYS_TMP"

log "done: processed=$PROCESSED inserted=$INSERTED skipped=$SKIPPED failed=$FAILED"
if [[ "$FAILED" -gt 0 ]]; then
    exit 2
fi
exit 0
