#!/usr/bin/env bash
# Catch code that REPORTS SUCCESS WHILE DOING NOTHING.
#
# This is the single most expensive bug shape this codebase produces, because
# every instance passes its build, exits 0, and logs success. Five landed in one
# week (2026-07-27..08-02):
#
#   1. mlx-lm-lora ORPO ran the forward pass outside nn.value_and_grad, so every
#      gradient was zero. Two adapters trained to completion as perfect no-ops.
#   2. The proactive check-in discarded channel->vtable->send()'s hu_error_t, so
#      a failed send logged "proactive check-in sent" AND recorded send-recency,
#      trained the bandit, and charged the governor. 12 log lines, 0 deliveries.
#   3. `INSERT OR IGNORE INTO opinions` had no UNIQUE constraint to violate, so
#      OR IGNORE never fired. 9,533,051 rows over 2,962 distinct pairs.
#   4. evaluate_orpo reported a bit-identical val loss across 9 checkpoints.
#   5. embedder_local.c returns a hash projection of word hashes and calls it an
#      embedding.
#
# Only measuring the ARTIFACT caught any of them -- weights, chat.db, row counts.
# Two of the five are mechanically detectable at commit time. This checks those.
#
# LIMITATION (found by testing the guard against its own subject): a constraint
# split across adjacent C string literals -- e.g. "CREATE UNIQUE INDEX ... "
# "ON opinions(...)" -- is invisible to a line-oriented grep, so such a table
# still reports as unconstrained. The baseline absorbs that; do not read a
# baselined table as proof it lacks a constraint.
#
# Usage: check-silent-success.sh [files...]   (defaults to staged files)
set -uo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)" || exit 0

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
  mapfile -t FILES < <(git diff --cached --name-only --diff-filter=ACM 2>/dev/null | grep -E '\.c$' || true)
fi
[ ${#FILES[@]} -eq 0 ] && exit 0

# Grandfathered tables: an OR IGNORE with no unique constraint is a RISK, not a
# proven bug -- it only duplicates if the same row is re-inserted. Measured
# 2026-08-02 against the live memory.db:
#
#   current_events       280,525 / 120,814 distinct = 2.3x  <- REAL, ~160k excess
#   behavioral_feedback   16,312 /  16,308 distinct = 1.0x
#   frontier_state            12 /      12 distinct = 1.0x
#
# So the check flags six and only one is actually duplicating. Baseline the known
# ones and fail on NEW ones, matching sqlite-includer-ratchet / clone-ratchet /
# file-size-ceiling. current_events is a live follow-up, not a false positive:
# it wants the same UNIQUE-index treatment `opinions` got.
BASELINE_TABLES=" ab_tests avoidance_patterns behavioral_feedback boundaries canvas_versions canvases causal_links comfort_patterns contact_baselines contact_identities contact_knowledge core_memory current_events dpo_auto_extractions dpo_pair_hashes episodic_patterns frontier_state general_lessons hyperedge_members kv lancedb_memories memory_edges oauth_tokens opinions reaction_lookup reciprocity_scores reflection_surfacings shared_references skill_profiles social_graph strategy_weights style_fingerprints temporal_patterns tier_memory tom_user_expectations tool_prefs topic_baselines training_data_extractions "

# Check 2 baselines by FILE, not by count. A global count is wrong for a staged
# subset: one NEW file with a single discard passes if 1 < the tree-wide total,
# which is how this guard first let its own test case through. Per-file means a
# new offender always fails, and fixing a listed file lets it leave the list.
DISCARD_BASELINE_FILES=" src/agent/inspiration.c src/app/main_wasi.c src/context/context_engine_rag.c src/daemon/daemon_followup_sched.c src/daemon/daemon_proactive.c "

fail=0

# ---------------------------------------------------------------- CHECK 1 ----
# `INSERT OR IGNORE|REPLACE INTO <table>` with no UNIQUE constraint anywhere in
# the tree for that table. OR IGNORE only suppresses CONSTRAINT violations; with
# nothing to violate it is an unconditional insert wearing a dedup costume.
for f in "${FILES[@]}"; do
  [ -f "$f" ] || continue
  while IFS= read -r tbl; do
    [ -z "$tbl" ] && continue
    case "$BASELINE_TABLES" in *" $tbl "*) continue ;; esac
    # The constraint must be ON THIS TABLE. An earlier draft of this check also
    # accepted a bare /PRIMARY KEY \(/ anywhere under src/, which matches
    # something in every build -- so the check passed unconditionally and caught
    # nothing. That is the very bug shape this script exists to detect, and it
    # was found only by running the guard against a reconstruction of the bug.
    # Never accept evidence that is not scoped to the subject.
    #
    # A single-column `id INTEGER PRIMARY KEY AUTOINCREMENT` does NOT count: it
    # is a rowid alias, unique per row by construction, so it can never make
    # OR IGNORE fire on the content columns. `opinions` had exactly that.
    if ! grep -rqE "CREATE UNIQUE INDEX[^;\"]*\bON[[:space:]]+$tbl[[:space:]]*\(" \
           --include='*.c' --include='*.h' src/ 2>/dev/null \
       && ! grep -rzoE "CREATE TABLE[^;\"]*\b$tbl\b[^;]*?(UNIQUE|PRIMARY KEY[[:space:]]*\([^)]*,)" \
           --include='*.c' --include='*.h' src/ 2>/dev/null | grep -q .; then
      echo "FAIL[silent-success]: $f does 'INSERT OR IGNORE/REPLACE INTO $tbl'"
      echo "  but no UNIQUE constraint on '$tbl' was found under src/."
      echo "  OR IGNORE suppresses CONSTRAINT violations only -- with nothing to"
      echo "  violate it inserts unconditionally. This is the bug that grew"
      echo "  'opinions' to 9.5M rows over 2,962 distinct pairs."
      echo "  Fix: add CREATE UNIQUE INDEX on the natural key (COALESCE nullable"
      echo "  columns -- SQLite treats NULLs as distinct in a UNIQUE index)."
      fail=1
    fi
  done < <(grep -ohE "INSERT[[:space:]]+OR[[:space:]]+(IGNORE|REPLACE)[[:space:]]+INTO[[:space:]]+[A-Za-z_][A-Za-z0-9_]*" "$f" 2>/dev/null \
           | awk '{print $NF}' | sort -u)
done

# ---------------------------------------------------------------- CHECK 2 ----
# A vtable send/store/write whose hu_error_t return is discarded on a line that
# is not an assignment, not a condition, and not an explicit (void) cast. The
# proactive bug was exactly this: the failure was invisible and four downstream
# systems recorded a delivery that never happened.
for f in "${FILES[@]}"; do
  [ -f "$f" ] || continue
  case "$DISCARD_BASELINE_FILES" in *" $f "*) continue ;; esac
  while IFS=: read -r ln text; do
    [ -z "$ln" ] && continue
    # Allowed: assigned, tested, explicitly voided, or a declaration/typedef.
    echo "$text" | grep -qE '=[[:space:]]*[a-zA-Z_(]|if[[:space:]]*\(|while[[:space:]]*\(|return|\(void\)|\(\*send\)|hu_error_t[[:space:]]' && continue
    echo "FAIL[silent-success]: $f:$ln discards a send/store return value"
    echo "  ${text:0:96}"
    echo "  A failed send that is not checked still runs the success bookkeeping."
    echo "  Assign and check it, or write (void) to state the discard is deliberate."
    fail=1
  done < <(grep -nE '\->(send|store|write)\(' "$f" 2>/dev/null | head -40)
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "Pre-commit blocked: code that can report success while doing nothing."
  echo "See .claude/rules/reports-success-does-nothing.md"
  exit 1
fi
exit 0
