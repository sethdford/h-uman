#!/usr/bin/env bash
# prune-memory-db.sh — one-time prune of ~/.human/memory.db dead-weight tables.
#
# Context (2026-07-19): memory.db reached ~20GB. Two tables are confirmed dead weight:
#   * skill_attempts (~74.8M rows, ~10GB) — negative-row faucet closed by PR #313 + #317;
#     keep the newest ~100k rows for residual analytics value.
#   * opinions (~18M rows, ~6GB) — its only writer (legacy F65 surface) was deleted on main
#     (~914e0f2a) and hu_opinions_upsert has no production caller; all rows are legacy
#     garbage. Emptied entirely. NOTE: `evolved_opinions` is a DIFFERENT table (see
#     src/memory/evolved_opinions.c) and is never touched here.
#
# The daemon MUST be stopped for the duration: VACUUM under a live writer is the risk.
# Post-VACUUM restart is fast — startup quick_check is sentinel-gated.
#
# Usage: scripts/prune-memory-db.sh [--yes] [--keep N]
#   --yes     non-interactive: accept the prune AND the no-backup path if backup
#             is impossible (insufficient disk)
#   --keep N  rows of skill_attempts to keep (default 100000)

set -euo pipefail

DB="$HOME/.human/memory.db"
SERVICE_LABEL="ai.human.service-loop"
PLIST_PATH="$HOME/Library/LaunchAgents/$SERVICE_LABEL.plist"
DAEMON_BIN="$HOME/.local/bin/human-daemon"
KEEP=100000
ASSUME_YES=0

for arg in "$@"; do
    case "$arg" in
        --yes)  ASSUME_YES=1 ;;
        --keep) ;; # value handled below
        --keep=*) KEEP="${arg#--keep=}" ;;
        *) if [[ "${prev:-}" == "--keep" ]]; then KEEP="$arg"; fi ;;
    esac
    prev="$arg"
done

confirm() { # confirm "<question>" — honors --yes
    [[ $ASSUME_YES -eq 1 ]] && return 0
    read -r -p "$1 [y/N] " reply
    [[ "$reply" == "y" || "$reply" == "Y" ]]
}

sql()    { sqlite3 "$DB" "$1"; }
db_size() { stat -f%z "$DB"; }
free_bytes() { df -Pk "$(dirname "$DB")" | awk 'NR==2{print $4*1024}'; }
gb() { awk -v b="$1" 'BEGIN{printf "%.1f GB", b/1e9}'; }

[[ "$(uname)" == "Darwin" ]] || { echo "error: macOS-only (launchd)" >&2; exit 1; }
[[ -f "$DB" ]] || { echo "error: $DB not found" >&2; exit 1; }
[[ -f "$PLIST_PATH" ]] || { echo "error: $PLIST_PATH not found — can't restart daemon after" >&2; exit 1; }
command -v sqlite3 >/dev/null || { echo "error: sqlite3 not on PATH" >&2; exit 1; }

SIZE_BEFORE=$(db_size)
echo "==> memory.db prune"
echo "    db:            $DB ($(gb "$SIZE_BEFORE"))"
echo "    free disk:     $(gb "$(free_bytes)")"
echo "    plan:          skill_attempts → keep newest $KEEP rows; opinions → empty; VACUUM"
confirm "Proceed (stops the $SERVICE_LABEL daemon for the duration)?" || { echo "aborted"; exit 1; }

# ── (b) Backup — or documented skip ──────────────────────────────────────
# A full backup needs >= db-size free on the same volume. At 20GB db / <20GB free
# this is typically impossible; the tables being pruned are confirmed write-orphaned
# (writers removed from the codebase), so the data is unrecoverable-but-worthless.
if (( $(free_bytes) > SIZE_BEFORE + 2000000000 )); then
    BACKUP="$DB.pre-prune.$(date +%Y%m%d%H%M%S)"
    echo "==> backing up to $BACKUP (delete it manually once satisfied)"
    sqlite3 "$DB" ".backup '$BACKUP'"
else
    echo "==> BACKUP SKIPPED: $(gb "$(free_bytes)") free < db size $(gb "$SIZE_BEFORE") + margin."
    echo "    Rationale: both pruned tables have no remaining writer in the codebase;"
    echo "    their contents are legacy garbage (see header). evolved_opinions is untouched."
    confirm "Continue WITHOUT a backup?" || { echo "aborted — free up disk and re-run"; exit 1; }
fi

# ── (a) Stop the daemon ──────────────────────────────────────────────────
echo "==> stopping $SERVICE_LABEL"
launchctl bootout "gui/$UID/$SERVICE_LABEL" 2>/dev/null || true
for _ in $(seq 1 30); do
    pgrep -f "$DAEMON_BIN" >/dev/null 2>&1 || break
    sleep 1
done
if pgrep -f "$DAEMON_BIN" >/dev/null 2>&1; then
    echo "error: daemon still running after bootout — refusing to prune under a live writer" >&2
    exit 1
fi

# From here on, any failure should still try to bring the daemon back.
restart_daemon() {
    echo "==> restarting $SERVICE_LABEL"
    launchctl bootstrap "gui/$UID" "$PLIST_PATH" 2>/dev/null || true
    launchctl kickstart -p "gui/$UID/$SERVICE_LABEL" 2>/dev/null || true
}
trap restart_daemon EXIT

# ── (c) Prune ────────────────────────────────────────────────────────────
# Sanity: the two victim tables and the protected table must all exist.
for t in skill_attempts opinions evolved_opinions; do
    [[ "$(sql "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='$t'")" == "1" ]] \
        || { echo "error: expected table '$t' missing — schema drift, aborting" >&2; exit 1; }
done
EVOLVED_BEFORE=$(sql "SELECT count(*) FROM evolved_opinions")

# Cutoff pass: sort only the applied_at column (no index on it; sorting full rows
# would spill ~10GB of temp). Ties at the cutoff keep slightly more than $KEEP.
echo "==> computing skill_attempts cutoff (newest $KEEP by applied_at)…"
CUTOFF=$(sql "SELECT applied_at FROM skill_attempts ORDER BY applied_at DESC LIMIT 1 OFFSET $((KEEP - 1))")
[[ -n "$CUTOFF" ]] || CUTOFF=0   # fewer than KEEP rows total → keep everything

# Copy-swap instead of DELETE: deleting ~74M rows would journal ~10GB of page
# pre-images. Copying ~100k keepers + DROP TABLE journals almost nothing.
SA_DDL=$(sql "SELECT sql FROM sqlite_master WHERE type='table' AND name='skill_attempts'")
echo "==> pruning skill_attempts (cutoff applied_at >= $CUTOFF)…"
sql "
BEGIN IMMEDIATE;
${SA_DDL/skill_attempts/skill_attempts_prune_new};
INSERT INTO skill_attempts_prune_new SELECT * FROM skill_attempts WHERE applied_at >= $CUTOFF;
DROP TABLE skill_attempts;
ALTER TABLE skill_attempts_prune_new RENAME TO skill_attempts;
CREATE INDEX idx_skill_attempts_skill ON skill_attempts(skill_id);
COMMIT;"

echo "==> emptying opinions (writer deleted; DROP + recreate beats a 18M-row DELETE)…"
OP_DDL=$(sql "SELECT sql FROM sqlite_master WHERE type='table' AND name='opinions'")
sql "
BEGIN IMMEDIATE;
DROP TABLE opinions;
$OP_DDL;
CREATE INDEX idx_opinions_topic ON opinions(topic);
COMMIT;"

# ── (d) VACUUM — needs free disk ≈ 2x the LIVE content (not the file size) ──
LIVE=$(sql "SELECT (SELECT page_count FROM pragma_page_count) * (SELECT page_size FROM pragma_page_size) - (SELECT freelist_count FROM pragma_freelist_count) * (SELECT page_size FROM pragma_page_size)")
NEED=$((LIVE * 2))
if (( $(free_bytes) < NEED )); then
    echo "error: VACUUM needs ~$(gb "$NEED") free, have $(gb "$(free_bytes)")." >&2
    echo "       Rows are pruned (db is valid, space is on the freelist); free disk and" >&2
    echo "       re-run 'sqlite3 $DB VACUUM' with the daemon stopped." >&2
    exit 1
fi
echo "==> VACUUM (live content $(gb "$LIVE"); this is the long step)…"
sql "VACUUM;"
echo "==> quick_check…"
QC=$(sql "PRAGMA quick_check;")
[[ "$QC" == "ok" ]] || { echo "error: quick_check failed: $QC" >&2; exit 1; }

# ── (e) Restart — the EXIT trap performs it; verify it took ──────────────
restart_daemon
trap - EXIT
sleep 2
launchctl print "gui/$UID/$SERVICE_LABEL" >/dev/null 2>&1 \
    || { echo "error: daemon did not come back — run scripts/install-human-daemon.sh" >&2; exit 1; }

# ── (f) Verify ───────────────────────────────────────────────────────────
SIZE_AFTER=$(db_size)
echo "==> results"
echo "    size:            $(gb "$SIZE_BEFORE") → $(gb "$SIZE_AFTER")"
echo "    skill_attempts:  $(sql 'SELECT count(*) FROM skill_attempts') rows kept"
echo "    opinions:        $(sql 'SELECT count(*) FROM opinions') rows"
echo "    evolved_opinions: $EVOLVED_BEFORE → $(sql 'SELECT count(*) FROM evolved_opinions') (must be unchanged)"
echo "==> doctor"
"$DAEMON_BIN" doctor 2>&1 | sed 's/^/    /' || echo "    (doctor reported issues — review above)"
echo "==> done"
