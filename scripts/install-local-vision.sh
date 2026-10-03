#!/usr/bin/env bash
# install-local-vision.sh — install the HU_LOCAL_VISION pieces: the Apple Vision
# OCR helper and the loopback caption server (ai.human.vision-server).
#
#   scripts/install-local-vision.sh --dry-run     render + validate, write nothing
#   scripts/install-local-vision.sh               build helper, install + load plist
#   scripts/install-local-vision.sh --uninstall   boot the server out, remove plist
#
# Options (env): HU_VISION_PYTHON  python with mlx_vlm (default: the pinned
#                                  ~/Documents/gemma-realtime-1/.venv312)
#                HU_VISION_MODEL   HF id (default mlx-community/gemma-4-e2b-it-4bit)
#                HU_VISION_PORT    default 8746; 8741/8743 (prod/spare) refused
#
# Installing the server does NOT turn the feature on: the daemon only calls it
# when HU_LOCAL_VISION=shadow|live (docs/guides/local-vision.md). Rollback is
# HU_LOCAL_VISION=off, then --uninstall to free the ~4.3 GB.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMPLATE="$ROOT/scripts/launchd/ai.human.vision-server.plist.template"
LABEL="ai.human.vision-server"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
PYTHON="${HU_VISION_PYTHON:-$HOME/Documents/gemma-realtime-1/.venv312/bin/python}"
MODEL="${HU_VISION_MODEL:-mlx-community/gemma-4-e2b-it-4bit}"
PORT="${HU_VISION_PORT:-8746}"
mode="${1:-install}"

die() { echo "install-local-vision: $*" >&2; exit 1; }

case "$mode" in --dry-run | install | --uninstall) ;; *) die "unknown argument: $mode" ;; esac

if [[ "$mode" == "--uninstall" ]]; then
    launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
    rm -f "$PLIST"
    echo "install-local-vision: $LABEL booted out and $PLIST removed"
    exit 0
fi

[[ "$PORT" =~ ^[0-9]{2,5}$ ]] || die "HU_VISION_PORT must be a port number"
[[ "$PORT" != 8741 && "$PORT" != 8743 ]] || die "port $PORT belongs to the serving model"
[[ "$MODEL" =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]] || die "HU_VISION_MODEL must be <org>/<name>"

# render_plist — the template with the four placeholders filled in.
render_plist() {
    sed -e "s#@HOME@#$HOME#g" -e "s#@PYTHON@#$PYTHON#g" -e "s#@MODEL@#$MODEL#g" \
        -e "s#@PORT@#$PORT#g" "$TEMPLATE"
}

# validate_plist <file> — the invariants a reviewer would check by eye.
validate_plist() {
    local f="$1"
    if command -v plutil >/dev/null 2>&1; then plutil -lint -s "$f" || die "rendered plist is not valid"; fi
    grep -q '@[A-Z]*@' "$f" && die "unfilled placeholder in rendered plist"
    grep -A1 -- '<string>--host</string>' "$f" | grep -q '<string>127.0.0.1</string>' ||
        die "server is not pinned to 127.0.0.1"
    grep -q '0\.0\.0\.0' "$f" && die "rendered plist mentions 0.0.0.0"
    grep -A1 '<key>HF_HUB_OFFLINE</key>' "$f" | grep -q '<string>1</string>' ||
        die "HF_HUB_OFFLINE=1 missing"
    return 0
}

staged="$(mktemp "${TMPDIR:-/tmp}/$LABEL.XXXXXX")"
trap 'rm -f "$staged"' EXIT
render_plist >"$staged"
validate_plist "$staged"

if [[ "$mode" == "--dry-run" ]]; then
    cat "$staged"
    echo "install-local-vision: dry run OK (127.0.0.1:$PORT, HF_HUB_OFFLINE=1, $MODEL) — nothing written" >&2
    exit 0
fi

[[ -x "$PYTHON" ]] || die "no python at $PYTHON (set HU_VISION_PYTHON)"
"$PYTHON" -c 'import mlx_vlm.server' 2>/dev/null || die "$PYTHON cannot import mlx_vlm.server"
cache="${HF_HUB_CACHE:-${HF_HOME:-$HOME/.cache/huggingface}/hub}"
[[ -d "$cache/models--${MODEL//\//--}" ]] || die "$MODEL is not in $cache (offline: download it first)"
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1 &&
    ! launchctl print "gui/$(id -u)/$LABEL" >/dev/null 2>&1; then
    die "port $PORT is already in use by something else"
fi

"$ROOT/tools/hu-vision-ocr/build.sh" "$HOME/.local/bin"
mkdir -p "$HOME/.human/logs" "$(dirname "$PLIST")"
chmod 0644 "$staged"
mv -f "$staged" "$PLIST"
trap - EXIT
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "install-local-vision: $LABEL loaded on 127.0.0.1:$PORT; health: curl -s http://127.0.0.1:$PORT/health"
