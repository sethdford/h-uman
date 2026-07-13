#!/usr/bin/env bash
# install-human-daemon.sh — install the h-uman service-loop daemon at a stable
# absolute path so macOS Full Disk Access (TCC) grants do not get churned by
# every `cmake --build`.
#
# Why this exists:
#   The default workflow rebuilds `build/human` constantly. macOS TCC tracks
#   binary identity by its codesign (cdhash + designated requirement). Even
#   when the certificate identifier is stable, the cdhash changes on every
#   rebuild, and self-signed certs are not enough for TCC to preserve grants.
#   This script separates the *daemon* binary (stable, intentional) from the
#   *build* binary (replaced on every iteration).
#
# What it does:
#   1. Copies build/human atomically to ${PREFIX}/bin/human-daemon.
#   2. Re-signs at the install path with the "Human Local Dev" cert and the
#      stable identifier ai.human.daemon.
#   3. Updates ~/Library/LaunchAgents/ai.human.service-loop.plist to point to
#      the install path.
#   4. Kickstarts the service.
#   5. Tells you exactly what to do for Full Disk Access.
#
# Honest limit:
#   FDA grants are NOT preserved across re-installs because the cdhash will
#   change. You will be prompted to re-grant once after each `install-human-
#   daemon.sh` run. The win is that you can iterate on code (`cmake --build`)
#   *without* breaking the running daemon's FDA grant — only an intentional
#   re-install costs you the grant.

set -euo pipefail

# ── Configuration ────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
PREFIX="${PREFIX:-$HOME/.local}"
INSTALL_BIN="$PREFIX/bin/human-daemon"
PLIST_PATH="$HOME/Library/LaunchAgents/ai.human.service-loop.plist"
SERVICE_LABEL="ai.human.service-loop"
CODESIGN_IDENT="${CODESIGN_IDENT:-Human Local Dev}"
TCC_IDENTIFIER="ai.human.daemon"
SOURCE_BIN="$BUILD_DIR/human"

# ── Sanity checks ────────────────────────────────────────────────────────
if [[ "$(uname)" != "Darwin" ]]; then
    echo "error: install-human-daemon.sh is macOS-only (TCC / launchd)." >&2
    echo "       on Linux use the systemd user-service approach in docs/guides/." >&2
    exit 1
fi

if [[ ! -x "$SOURCE_BIN" ]]; then
    echo "error: source binary not found at $SOURCE_BIN" >&2
    echo "       run: cmake --build $BUILD_DIR -j" >&2
    exit 1
fi

# ── Guard-sentinel: refuse to deploy a binary missing critical outbound guards
# On 2026-07-11..13 a concurrent session built from a branch that PREDATED the
# G10 deliberation-leak guard and installed it here, silently un-deploying the
# guard — and NEEDS_RETRY / "please revise the response" meta-text reached real
# contacts again. A fix that doesn't stay deployed isn't a fix. These sentinels
# are stable string literals compiled into the guard (src/agent/response_guard.c
# + reflection.c). If a candidate binary lacks ANY of them it is a regressed
# build; refuse it rather than clobber a good daemon. Override for a deliberate
# downgrade with HU_SKIP_GUARD_SENTINEL=1.
if [[ "${HU_SKIP_GUARD_SENTINEL:-0}" != "1" ]]; then
    GUARD_SENTINELS=(
        "revise the response"        # G10 D7 — GVR/meta-instruction echo
        "nobody texts"               # G10 D6 — reflection-critique echo
        "That draft isn't quite right" # reflection retry rewrite (not the judge prompt)
    )
    missing=()
    for sentinel in "${GUARD_SENTINELS[@]}"; do
        if ! strings -a "$SOURCE_BIN" 2>/dev/null | grep -qF "$sentinel"; then
            missing+=("$sentinel")
        fi
    done
    if (( ${#missing[@]} > 0 )); then
        echo "error: refusing to install — $SOURCE_BIN is missing outbound-guard sentinels:" >&2
        for m in "${missing[@]}"; do echo "         - \"$m\"" >&2; done
        echo "       This binary predates the deliberation-leak guards; installing it would" >&2
        echo "       regress production (meta-text like NEEDS_RETRY could reach real contacts)." >&2
        echo "       Rebuild from a branch that contains the guards (merged to main), or set" >&2
        echo "       HU_SKIP_GUARD_SENTINEL=1 to force a deliberate downgrade." >&2
        exit 1
    fi
    echo "==> guard-sentinel: all ${#GUARD_SENTINELS[@]} outbound guards present in candidate binary"
fi

# Verify the local cert exists; if not, refuse to fall back to ad-hoc because
# that produces an even less stable TCC identity.
if ! security find-identity -v -p codesigning 2>/dev/null | grep -qE "\"$CODESIGN_IDENT\""; then
    echo "error: codesign identity '$CODESIGN_IDENT' not found in keychain." >&2
    echo "       create it via: just setup-codesign" >&2
    echo "       (or set CODESIGN_IDENT=- to force ad-hoc signing — not recommended)" >&2
    if [[ "$CODESIGN_IDENT" != "-" ]]; then
        exit 1
    fi
fi

mkdir -p "$(dirname "$INSTALL_BIN")"
mkdir -p "$HOME/.human/logs"
mkdir -p "$HOME/Library/LaunchAgents"

# ── Detect prior install for honest user messaging ───────────────────────
PRIOR_BIN_EXISTED=false
PRIOR_BIN_HASH=""
if [[ -f "$INSTALL_BIN" ]]; then
    PRIOR_BIN_EXISTED=true
    PRIOR_BIN_HASH="$(shasum -a 256 "$INSTALL_BIN" | awk '{print $1}')"
fi
NEW_BIN_HASH="$(shasum -a 256 "$SOURCE_BIN" | awk '{print $1}')"

if [[ "$PRIOR_BIN_EXISTED" == "true" && "$PRIOR_BIN_HASH" == "$NEW_BIN_HASH" ]]; then
    echo "skip: installed binary is byte-identical to $SOURCE_BIN ($NEW_BIN_HASH)"
    echo "      no copy / re-sign needed — FDA grant preserved."
    SKIP_INSTALL=true
else
    SKIP_INSTALL=false
fi

# ── Atomic copy + re-sign ────────────────────────────────────────────────
if [[ "$SKIP_INSTALL" == "false" ]]; then
    STAGED="${INSTALL_BIN}.staged-$$"
    trap 'rm -f "$STAGED"' EXIT

    echo "==> copy   $SOURCE_BIN  →  $STAGED"
    cp "$SOURCE_BIN" "$STAGED"
    chmod 0755 "$STAGED"

    echo "==> sign   identifier=$TCC_IDENTIFIER  cert='$CODESIGN_IDENT'"
    # NOTE: deliberately do NOT pass --options runtime. Hardened runtime
    # enables library validation, which makes dyld reject Homebrew dylibs
    # (e.g. libssl) that lack a matching Team ID — they're signed by Apple
    # under a different team. The CMake build's codesign step omits it for
    # the same reason. Without library validation, the self-signed dev cert
    # is still enough for TCC to track the binary across rebuilds when its
    # path is stable.
    codesign --force \
             --sign "$CODESIGN_IDENT" \
             --identifier "$TCC_IDENTIFIER" \
             --timestamp=none \
             "$STAGED"

    echo "==> verify codesign"
    codesign -dv "$STAGED" 2>&1 | sed 's/^/    /'

    echo "==> atomic mv → $INSTALL_BIN"
    mv -f "$STAGED" "$INSTALL_BIN"
    trap - EXIT
fi

# ── Render / update launchd plist ────────────────────────────────────────
# Snapshot operator-set EnvironmentVariables BEFORE the heredoc overwrites the
# plist. The template below only writes the base infra env (HOME/PATH/HU_DEBUG/
# ASAN_OPTIONS); without this, a reinstall silently DROPS any feature gates an
# operator activated in the plist (e.g. HU_GRAPH_GROUNDING=on, HU_TERSENESS=live,
# HU_PROACTIVE_CONTEXTUAL=on). We capture every non-base key now and re-apply it
# after rendering. See ~/.claude/rules/silent-config-gated-subsystems.md.
PRESERVED_ENV_KEYS=()
PRESERVED_ENV_VALS=()
if [[ -f "$PLIST_PATH" ]]; then
    while IFS= read -r _k; do
        [[ -z "$_k" ]] && continue
        case "$_k" in HOME | PATH | HU_DEBUG | ASAN_OPTIONS) continue ;; esac
        _v="$(/usr/libexec/PlistBuddy -c "Print :EnvironmentVariables:$_k" "$PLIST_PATH" 2>/dev/null)" || continue
        PRESERVED_ENV_KEYS+=("$_k")
        PRESERVED_ENV_VALS+=("$_v")
    done < <(/usr/libexec/PlistBuddy -c "Print :EnvironmentVariables" "$PLIST_PATH" 2>/dev/null |
        sed -n 's/^[[:space:]][[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*=.*/\1/p')
fi

echo "==> launchd plist → $PLIST_PATH"
cat > "$PLIST_PATH" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${SERVICE_LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>${INSTALL_BIN}</string>
        <string>service-loop</string>
        <string>--with-gateway</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>HOME</key>
        <string>${HOME}</string>
        <key>PATH</key>
        <string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:${HOME}/.local/bin</string>
        <key>HU_DEBUG</key>
        <string>1</string>
        <key>ASAN_OPTIONS</key>
        <string>halt_on_error=0:detect_leaks=0:log_path=${HOME}/.human/logs/asan.log</string>
    </dict>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
    </dict>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>StandardOutPath</key>
    <string>${HOME}/.human/logs/service-loop.log</string>
    <key>StandardErrorPath</key>
    <string>${HOME}/.human/logs/service-loop-error.log</string>
    <key>WorkingDirectory</key>
    <string>${HOME}</string>
    <key>Nice</key>
    <integer>5</integer>
</dict>
</plist>
PLIST

# Re-apply operator-set env vars captured above, so activated feature gates
# survive the reinstall instead of being silently dropped.
if [[ ${#PRESERVED_ENV_KEYS[@]} -gt 0 ]]; then
    _i=0
    while [[ $_i -lt ${#PRESERVED_ENV_KEYS[@]} ]]; do
        _k="${PRESERVED_ENV_KEYS[$_i]}"
        _v="${PRESERVED_ENV_VALS[$_i]}"
        /usr/libexec/PlistBuddy -c "Add :EnvironmentVariables:$_k string $_v" "$PLIST_PATH" 2>/dev/null ||
            /usr/libexec/PlistBuddy -c "Set :EnvironmentVariables:$_k $_v" "$PLIST_PATH"
        _i=$((_i + 1))
    done
    echo "==> preserved ${#PRESERVED_ENV_KEYS[@]} operator env var(s): ${PRESERVED_ENV_KEYS[*]}"
fi

# Validate the rendered plist parses before bootstrapping launchd against it.
if ! plutil -lint "$PLIST_PATH" >/dev/null; then
    echo "error: rendered plist failed plutil -lint — aborting before reload." >&2
    exit 1
fi

echo "==> launchctl bootstrap (idempotent)"
# Best-effort bootout. Returns 3/EIO if not loaded — both are fine.
launchctl bootout "gui/$UID/$SERVICE_LABEL" 2>/dev/null || true

# Wait for the service label to actually be released. Without this loop,
# `bootstrap` races and returns EIO (5) on busy systems / macOS Sequoia,
# even after the bootout call has returned. We poll `print` until it
# reports "Could not find service" (113) which means the label is free.
echo "==> waiting for service label to release"
for i in 1 2 3 4 5 6 7 8 9 10; do
    if ! launchctl print "gui/$UID/$SERVICE_LABEL" >/dev/null 2>&1; then
        # Label is gone — safe to bootstrap.
        break
    fi
    sleep 1
done

# Bootstrap with retry. EIO (5) on first attempt is common right after
# bootout; we back off and retry up to 5 times.
BOOTSTRAP_OK=false
for attempt in 1 2 3 4 5; do
    if launchctl bootstrap "gui/$UID" "$PLIST_PATH" 2>&1; then
        BOOTSTRAP_OK=true
        break
    fi
    echo "    bootstrap attempt $attempt failed, retrying in $((attempt * 2))s..."
    sleep $((attempt * 2))
done

if [[ "$BOOTSTRAP_OK" == "false" ]]; then
    echo "error: launchctl bootstrap failed after 5 retries." >&2
    echo "       check 'launchctl print-disabled gui/$UID' and the plist." >&2
    exit 1
fi

launchctl kickstart -k -p "gui/$UID/$SERVICE_LABEL"

# Give it a beat to do its first poll, then tell the truth about state.
sleep 4
STATUS_FILE="$HOME/.human/imessage.poll_status"
echo
if [[ "$SKIP_INSTALL" == "true" ]]; then
    echo "Done — daemon kickstarted at unchanged install path."
else
    echo "Done — daemon installed at $INSTALL_BIN and kickstarted."
fi
echo

if [[ -f "$STATUS_FILE" ]]; then
    echo "current iMessage poll status:"
    sed 's/^/    /' "$STATUS_FILE"
    echo
fi

# Run the doctor as the source of truth.
if "$INSTALL_BIN" doctor imessage 2>&1 | sed 's/^/    /'; then
    DOCTOR_OK=true
else
    DOCTOR_OK=false
fi
echo

if [[ "$SKIP_INSTALL" == "false" || "$DOCTOR_OK" == "false" ]]; then
    cat <<EOM
Next step — Full Disk Access (one-time per install):

    1. Open System Settings → Privacy & Security → Full Disk Access
    2. Click + (or remove the old entry first if it points at a stale path)
    3. Add this exact path:
           $INSTALL_BIN
    4. Toggle it ON
    5. Re-run:  launchctl kickstart -k gui/\$UID/$SERVICE_LABEL
    6. Verify:  $INSTALL_BIN doctor imessage

If 'doctor imessage' says circuit breaker OK and a fresh poll, you're done.
If it still reports TRIPPED after re-grant + kickstart, the breaker resets
on the first successful poll (1 second cadence) so wait ~5s and re-run.
EOM
fi
