#!/usr/bin/env bash
# Tests for the source-unchanged skip in nightly-retrain.sh — the check that
# decides whether tonight's window is worth stopping the production server for.
#
# Hermetic: fake HOME, fake repo dir (so the script's python helpers import
# nothing real), and fake launchctl / pgrep / lsof / sleep / vm_stat / curl
# FIRST on PATH. The real launchd job, the real ~/.human tree and the real
# :8741 are never touched — the fake launchctl only records its argv, and the
# fake pgrep reports the server as still alive so stop_serving refuses and the
# script exits before any training, steering or classifier stage.
#
# Why this exists: through 2026-09-05 this window retrained every night from
# ~/.human/training-data/m3-outcomes.jsonl, a file unchanged since 2026-08-02,
# producing byte-identical 556 MB adapters on 09-04 21:29 and 09-05 03:07 —
# ~5 minutes of persona downtime per run to learn nothing. Case 1 pins the
# skip; cases 2-5 pin that it never skips when it must not.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/nightly-retrain.sh"
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; echo "  --- output ---"; echo "$3" | sed 's/^/  /'; fail=1; fi; }

bash -n "$SCRIPT" || { echo "FAIL bash -n $SCRIPT"; exit 1; }
echo "PASS bash -n $SCRIPT"

# The digest function the script itself uses to WRITE stamps. Sourcing with
# HU_RETRAIN_STAGE_TEST=1 defines log()/hu_sha256()/stop_serving() and returns
# before the window check or any real work. Building the fixture stamps with
# this (and with the writer's exact "<sha>  <path>" printf) is what ties the
# reader under test to the writer in the "adapter real:" branch — a format
# drift on either side fails case 1 rather than silently disabling the skip.
sha_of() { HU_RETRAIN_STAGE_TEST=1 bash -c 'source "'"$SCRIPT"'"; hu_sha256 "$1"' _ "$1"; }
write_stamp() { printf '%s  %s\n' "$(sha_of "$2")" "$2" > "$1/source.sha256"; }

# make_env: fake HOME + fake repo + fake bins. Echoes the temp root.
#   launchctl: records argv to <root>/launchctl.args, exits 3 ("not loaded")
#   pgrep/lsof: exit 0 => "the server is still alive", so stop_serving refuses
#               with FATAL and the script exits 1 before it can train.
make_env() {
    local root; root=$(mktemp -d)
    mkdir -p "$root/home/.human/logs" "$root/home/.human/training-data/adapters" \
             "$root/repo/scripts" "$root/bin"
    printf '#!/bin/sh\necho "$*" >> "%s/launchctl.args"\nexit 3\n' "$root" > "$root/bin/launchctl"
    printf '#!/bin/sh\nexit 0\n' > "$root/bin/pgrep"
    printf '#!/bin/sh\nexit 0\n' > "$root/bin/lsof"
    printf '#!/bin/sh\nexit 0\n' > "$root/bin/sleep"
    printf '#!/bin/sh\necho "Pages free:                             6553600."\n' > "$root/bin/vm_stat"
    printf '#!/bin/sh\necho 000\n' > "$root/bin/curl"
    chmod +x "$root/bin/"*
    printf '{"a": 1}\n{"b": 2}\n' > "$root/home/.human/training-data/m3-outcomes.jsonl"
    echo "$root"
}

# adapter <root> <name> <mtime YYYYMMDDhhmm>: a staged adapter dir, mtime pinned
# so `ls -dt` ordering is deterministic rather than creation-order luck.
adapter() {
    local d="$1/home/.human/training-data/adapters/$2"
    mkdir -p "$d"; printf 'weights\n' > "$d/adapters.safetensors"; echo "$d"
}
set_mtime() { touch -t "$2" "$1"; }

# run <root> [extra env assignments...]: run the script with the window
# override (HU_RETRAIN_FORCE=1) so it reaches the stamp check, and with a dead
# port everywhere a health probe might look. Echoes combined output, then a
# final "rc=<n>" line.
run() {
    local root="$1"; shift
    local out rc
    out=$(env HOME="$root/home" PATH="$root/bin:$PATH" HU_REPO_DIR="$root/repo" \
              HU_RETRAIN_FORCE=1 HU_RETRAIN_PORT=59999 \
              HU_MLX_HEALTH="http://127.0.0.1:59999/health" \
              HU_RETRAIN_STOP_WAIT_SECS=10 "$@" \
              bash "$SCRIPT" 2>&1); rc=$?
    printf '%s\nrc=%s\n' "$out" "$rc"
}

SRC_REL=".human/training-data/m3-outcomes.jsonl"

# ── Case 1: newest stamped adapter matches the source — skip, never stop ────
R1=$(make_env)
A1=$(adapter "$R1" "seth-m3-outcomes-20260905-030710-glm")
write_stamp "$A1" "$R1/home/$SRC_REL"; set_mtime "$A1" 202609050307
SHA1=$(sha_of "$R1/home/$SRC_REL")
out1=$(run "$R1")
check "unchanged: logs the skip line naming the adapter and the 12-char sha" \
    "[[ \"\$out1\" == *'source unchanged since seth-m3-outcomes-20260905-030710-glm (sha ${SHA1:0:12}) — skipping training, serving untouched'* ]]" "$out1"
check "unchanged: exits 0" "[[ \"\$out1\" == *'rc=0'* ]]" "$out1"
check "unchanged: NEVER reaches the serving-stop path" \
    "[[ \"\$out1\" != *'stopping mlx-server'* ]]" "$out1"
check "unchanged: launchctl was never invoked at all" \
    "[ ! -f \"$R1/launchctl.args\" ]" "$(cat "$R1/launchctl.args" 2>/dev/null)"
check "unchanged: still writes today's dated line, so the watchdog does not re-run it" \
    "grep -q \"^\\[$(date +%Y-%m-%d)\" \"$R1/home/.human/logs/nightly-retrain.log\"" \
    "$(cat "$R1/home/.human/logs/nightly-retrain.log" 2>/dev/null)"
rm -rf "$R1"

# ── Case 2: the source changed since the stamp — must proceed ───────────────
R2=$(make_env)
A2=$(adapter "$R2" "seth-m3-outcomes-20260905-030710-glm")
write_stamp "$A2" "$R2/home/$SRC_REL"
printf '{"c": 3}\n' >> "$R2/home/$SRC_REL"     # corpus grew after the stamp
set_mtime "$A2" 202609050307
out2=$(run "$R2")
check "changed: logs that the digests differ" \
    "[[ \"\$out2\" == *'source changed since seth-m3-outcomes-20260905-030710-glm'* ]]" "$out2"
check "changed: proceeds into the serving-stop path" \
    "[[ \"\$out2\" == *'stopping mlx-server'* ]]" "$out2"
check "changed: the (faked) refusal stops it there, not in training" \
    "[[ \"\$out2\" == *'FATAL: mlx-server still alive after bootout'* ]]" "$out2"
rm -rf "$R2"

# ── Case 3: adapters exist but none carries a stamp — must proceed ──────────
R3=$(make_env)
A3=$(adapter "$R3" "seth-m3-outcomes-20260904-212919-glm"); set_mtime "$A3" 202609042129
out3=$(run "$R3")
check "no stamp: says so explicitly" \
    "[[ \"\$out3\" == *'no source.sha256 stamp on any staged adapter — training'* ]]" "$out3"
check "no stamp: proceeds into the serving-stop path" \
    "[[ \"\$out3\" == *'stopping mlx-server'* ]]" "$out3"
rm -rf "$R3"

# ── Case 4: HU_RETRAIN_FORCE_TRAIN=1 overrides a matching stamp ─────────────
R4=$(make_env)
A4=$(adapter "$R4" "seth-m3-outcomes-20260905-030710-glm")
write_stamp "$A4" "$R4/home/$SRC_REL"; set_mtime "$A4" 202609050307
out4=$(run "$R4" HU_RETRAIN_FORCE_TRAIN=1)
check "force: logs the override" \
    "[[ \"\$out4\" == *'HU_RETRAIN_FORCE_TRAIN=1 — training even if the source is unchanged'* ]]" "$out4"
check "force: never claims the source is unchanged" \
    "[[ \"\$out4\" != *'source unchanged since'* ]]" "$out4"
check "force: proceeds into the serving-stop path" \
    "[[ \"\$out4\" == *'stopping mlx-server'* ]]" "$out4"
rm -rf "$R4"

# ── Case 5: a NEWER .rejected-* dir with a matching stamp is ignored ────────
# mtime survives the quarantine rename, so an unfiltered `ls -t` would let a
# quarantined no-op adapter authorise skipping tonight's real retrain.
R5=$(make_env)
A5ok=$(adapter "$R5" "seth-m3-outcomes-20260904-212919-glm")
printf 'deadbeef  %s\n' "$R5/home/$SRC_REL" > "$A5ok/source.sha256"   # different digest
set_mtime "$A5ok" 202609042129
A5rej=$(adapter "$R5" "seth-m3-outcomes-20260905-030710-glm.rejected-1788515667")
write_stamp "$A5rej" "$R5/home/$SRC_REL"                              # MATCHES, but rejected
set_mtime "$A5rej" 202609050307
out5=$(run "$R5")
check "rejected: does not skip on a quarantined adapter's matching stamp" \
    "[[ \"\$out5\" != *'source unchanged since'* ]]" "$out5"
check "rejected: reads the newest NON-rejected stamp instead" \
    "[[ \"\$out5\" == *'source changed since seth-m3-outcomes-20260904-212919-glm (stamp deadbeef'* ]]" "$out5"
check "rejected: proceeds into the serving-stop path" \
    "[[ \"\$out5\" == *'stopping mlx-server'* ]]" "$out5"
rm -rf "$R5"

if [[ "$fail" == "0" ]]; then echo "ALL PASS"; else echo "SOME FAILED"; fi
exit "$fail"
