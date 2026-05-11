#!/usr/bin/env bash
# verify-ac1-3-4-5.sh — Hermetic verification driver for Story B
# (sprints/sprint-1/stories.md, ACs B.1–B.5). AC-B.2 is verified by
# grep against scripts/lora-runner-ab.sh; AC-B.6 is verified by
# `shellcheck scripts/lora-runner-ab.sh` and captured separately as
# evidence/B/shellcheck.log.
#
# Design reference: sprints/sprint-1/designs/B.md §6.5 (hermetic
# wrapper recipe). The driver replaces $HUMAN_BIN with a small shell
# script that emits canned JSON results for the three orchestrator
# subcommands (`ml lora-runner`, `ml lora-ab`, `ml fidelity-status`).
# This lets the success path run to completion without any real
# provider, while the empty-response wrapper exercises the exit-2
# failure path that AC-B.5 demands.
#
# Each AC produces a labelled PASS/FAIL line. A non-zero overall exit
# means at least one AC failed. The script wipes its tempdir via
# `trap` on EXIT, so /tmp stays clean even on early abort.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/lora-runner-ab.sh"

# Single sandbox tempdir houses the fake $HOME, the fake adapter, and
# both wrappers. Wiped on EXIT regardless of pass/fail/early-abort.
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/lora-ab-verifyB.XXXXXX")"
trap 'rm -rf "$SANDBOX" "/tmp/test-ab.json" 2>/dev/null || true' EXIT

mkdir -p "$SANDBOX/.human/personas"

PERSONA_NAME="story_b_fixture"
# Minimal persona JSON — the wrappers never consult it (they just
# emit canned output keyed on the $HUMAN_BIN argv), but the
# orchestrator passes --persona through to every subcommand and a
# real `human ml ...` would reject a missing persona. We stage it
# anyway so a future implementer can swap in the real binary without
# rewriting the driver.
cat >"$SANDBOX/.human/personas/${PERSONA_NAME}.json" <<'EOF'
{"identity": {"name": "story-b-fixture"}, "traits": [], "example_banks": []}
EOF

# Any non-empty file satisfies the orchestrator's adapter-exists
# check at scripts/lora-runner-ab.sh:109. The success wrapper does
# not attempt to load it.
ADAPTER="$SANDBOX/fake-adapter.bin"
printf 'fixture' >"$ADAPTER"

# ---------------------------------------------------------------------
# Wrapper #1 — success path
#
# Emits a 3-element non-empty JSON array for `ml lora-runner`, a noop
# success for `ml lora-ab`, and a canned status JSON for `ml
# fidelity-status`. The status content is intentionally trivial — the
# orchestrator never inspects it; only the canonical-publish step
# cares whether the file exists.
# ---------------------------------------------------------------------
WRAPPER_OK="$SANDBOX/fake-human-ok"
cat >"$WRAPPER_OK" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

# Strip the leading "ml" subcommand selector so we can match cleanly
# on the verb. Args: ml <verb> <flags...>
verb="${2:-}"

out=""
while [[ $# -gt 0 ]]; do
    if [[ "$1" == "--output" ]]; then
        out="$2"
        shift 2
        continue
    fi
    shift
done

case "$verb" in
    lora-runner)
        printf '%s' '["ok-response-0","ok-response-1","ok-response-2"]' >"$out"
        ;;
    lora-ab)
        # Comparator gate passes — exit 0 with no output to stdout.
        ;;
    fidelity-status)
        printf '%s' '{"persona":"story_b_fixture","baseline":{"mean":0.923,"min":0.9,"max":0.95,"scored":3},"ab":{"available":true,"before_mean":0.5,"after_mean":0.7,"delta":0.2,"scored_before":3,"scored_after":3}}' >"$out"
        ;;
    *) ;;
esac

exit 0
EOF
chmod +x "$WRAPPER_OK"

# ---------------------------------------------------------------------
# Wrapper #2 — exit-2 failure path
#
# Emits an all-empty `[""]` JSON array for `ml lora-runner` so the
# orchestrator's `empty_response_set` check fires on step 1 and
# raises exit 2. Other verbs are never reached.
# ---------------------------------------------------------------------
WRAPPER_EMPTY="$SANDBOX/fake-human-empty"
cat >"$WRAPPER_EMPTY" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

out=""
while [[ $# -gt 0 ]]; do
    if [[ "$1" == "--output" ]]; then
        out="$2"
        shift 2
        continue
    fi
    shift
done

if [[ -n "$out" ]]; then
    printf '%s' '[""]' >"$out"
fi
exit 0
EOF
chmod +x "$WRAPPER_EMPTY"

PASS=0
FAIL=0

ac_pass() { echo "[verify-B] PASS: $1"; PASS=$((PASS+1)); }
ac_fail() { echo "[verify-B] FAIL: $1" >&2; FAIL=$((FAIL+1)); }

# ---------------------------------------------------------------------
# AC-B.2 — `mv` present in script, no direct redirect to canonical path
# ---------------------------------------------------------------------
echo
echo "=== AC-B.2: mv-based atomic rename present in script ==="
# shellcheck disable=SC2016
# The grep patterns intentionally contain literal `$tmpfile` /
# `$dest` / `$HOME` characters — we are looking for those tokens in
# the script's source, not expanding them in the driver. Single
# quotes are the correct choice.
if grep -nE 'mv\s+"\$tmpfile"\s+"\$dest"' "$SCRIPT" >/dev/null; then
    grep -nE 'mv\s+"\$tmpfile"\s+"\$dest"' "$SCRIPT"
    if grep -nE '>\s*"?\$?\{?HOME[^"]*last_fidelity_ab\.json' "$SCRIPT" >/dev/null \
        || grep -nE '>\s*~/\.human/last_fidelity_ab\.json' "$SCRIPT" >/dev/null; then
        ac_fail "AC-B.2: direct redirect to canonical path found"
    else
        ac_pass "AC-B.2"
    fi
else
    ac_fail "AC-B.2: mv \"\$tmpfile\" \"\$dest\" not found in $SCRIPT"
fi

# ---------------------------------------------------------------------
# AC-B.5 — exit-2 no-provider stub path does NOT publish
#
# Pre-state: ensure the canonical file does not exist. Run the
# orchestrator with the empty-response wrapper. Confirm exit code
# is 2 AND the canonical file is still absent.
# ---------------------------------------------------------------------
echo
echo "=== AC-B.5: exit-2 path leaves canonical file untouched ==="
rm -f "$SANDBOX/.human/last_fidelity_ab.json"
out_b5="$(mktemp -d "$SANDBOX/out-b5.XXXXXX")"
set +e
HOME="$SANDBOX" HUMAN_BIN="$WRAPPER_EMPTY" \
    bash "$SCRIPT" \
        --persona "$PERSONA_NAME" \
        --adapter "$ADAPTER" \
        --output-dir "$out_b5" >"$SANDBOX/b5.stdout" 2>"$SANDBOX/b5.stderr"
ec_b5=$?
set -e
echo "  exit code: $ec_b5"
echo "  stderr tail: $(tail -1 "$SANDBOX/b5.stderr" 2>/dev/null || echo '<empty>')"
if [[ "$ec_b5" -ne 2 ]]; then
    ac_fail "AC-B.5: expected exit 2, got $ec_b5"
elif [[ -f "$SANDBOX/.human/last_fidelity_ab.json" ]]; then
    ac_fail "AC-B.5: canonical file was created despite exit 2"
else
    ac_pass "AC-B.5"
fi

# ---------------------------------------------------------------------
# AC-B.1 — successful run publishes to canonical path; file equals
# <output-dir>/status.json
# ---------------------------------------------------------------------
echo
echo "=== AC-B.1: success publishes status.json to canonical path ==="
rm -f "$SANDBOX/.human/last_fidelity_ab.json"
out_b1="$(mktemp -d "$SANDBOX/out-b1.XXXXXX")"
HOME="$SANDBOX" HUMAN_BIN="$WRAPPER_OK" \
    bash "$SCRIPT" \
        --persona "$PERSONA_NAME" \
        --adapter "$ADAPTER" \
        --output-dir "$out_b1" >"$SANDBOX/b1.stdout" 2>"$SANDBOX/b1.stderr"
echo "  exit code: 0 (set -e would have aborted otherwise)"
echo "  stdout tail: $(tail -2 "$SANDBOX/b1.stdout")"
if [[ ! -f "$SANDBOX/.human/last_fidelity_ab.json" ]]; then
    ac_fail "AC-B.1: canonical file was not written"
elif ! diff "$SANDBOX/.human/last_fidelity_ab.json" "$out_b1/status.json" >/dev/null; then
    echo "  diff:"
    diff "$SANDBOX/.human/last_fidelity_ab.json" "$out_b1/status.json" || true
    ac_fail "AC-B.1: canonical file differs from $out_b1/status.json"
else
    echo "  diff (canonical vs status.json): empty"
    ac_pass "AC-B.1"
fi

# ---------------------------------------------------------------------
# AC-B.3 — --no-publish does NOT touch the canonical file
#
# Pre-condition: canonical file currently exists from AC-B.1.
# Capture its stat, then run with --no-publish, then confirm the stat
# is unchanged.
# ---------------------------------------------------------------------
echo
echo "=== AC-B.3: --no-publish leaves canonical file unchanged ==="
canonical="$SANDBOX/.human/last_fidelity_ab.json"
if [[ ! -f "$canonical" ]]; then
    # If AC-B.1 failed, plant a sentinel so we can still assert
    # "unchanged" semantics.
    printf '%s' '{"sentinel":"pre-b3"}' >"$canonical"
fi
pre_b3="$(shasum "$canonical" | awk '{print $1}')"
out_b3="$(mktemp -d "$SANDBOX/out-b3.XXXXXX")"
HOME="$SANDBOX" HUMAN_BIN="$WRAPPER_OK" \
    bash "$SCRIPT" \
        --persona "$PERSONA_NAME" \
        --adapter "$ADAPTER" \
        --output-dir "$out_b3" \
        --no-publish >"$SANDBOX/b3.stdout" 2>"$SANDBOX/b3.stderr"
post_b3="$(shasum "$canonical" | awk '{print $1}')"
echo "  pre  sha1: $pre_b3"
echo "  post sha1: $post_b3"
if grep -q 'published →' "$SANDBOX/b3.stdout"; then
    ac_fail "AC-B.3: 'published →' line printed under --no-publish"
elif [[ "$pre_b3" != "$post_b3" ]]; then
    ac_fail "AC-B.3: canonical file changed despite --no-publish"
else
    ac_pass "AC-B.3"
fi

# ---------------------------------------------------------------------
# AC-B.4 — HUMAN_FIDELITY_AB_PATH override writes to that path,
# leaves default canonical path unchanged.
# ---------------------------------------------------------------------
echo
echo "=== AC-B.4: HUMAN_FIDELITY_AB_PATH override directs publish ==="
override="/tmp/test-ab.json"
rm -f "$override"
# Reset canonical so we can confirm it stays unchanged through this run.
rm -f "$canonical"
out_b4="$(mktemp -d "$SANDBOX/out-b4.XXXXXX")"
HOME="$SANDBOX" HUMAN_BIN="$WRAPPER_OK" HUMAN_FIDELITY_AB_PATH="$override" \
    bash "$SCRIPT" \
        --persona "$PERSONA_NAME" \
        --adapter "$ADAPTER" \
        --output-dir "$out_b4" >"$SANDBOX/b4.stdout" 2>"$SANDBOX/b4.stderr"
echo "  override path: $override"
echo "  override exists: $([[ -f "$override" ]] && echo yes || echo no)"
echo "  canonical exists: $([[ -f "$canonical" ]] && echo yes || echo no)"
if [[ ! -f "$override" ]]; then
    ac_fail "AC-B.4: $override was not created"
elif [[ -f "$canonical" ]]; then
    ac_fail "AC-B.4: canonical file was also written"
elif ! diff "$override" "$out_b4/status.json" >/dev/null; then
    ac_fail "AC-B.4: $override differs from $out_b4/status.json"
else
    ac_pass "AC-B.4"
fi

echo
echo "=========================================================="
echo "Story B verification: PASS=$PASS FAIL=$FAIL"
echo "=========================================================="

[[ "$FAIL" -eq 0 ]]
