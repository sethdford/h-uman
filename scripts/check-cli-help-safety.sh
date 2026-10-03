#!/usr/bin/env bash
# Prove `human <command> --help` describes a command and never runs it.
#
# Before src/app/cli_help.c, --help reached every handler, so commands that
# ignore their arguments executed: `service-loop --help` started the daemon
# loop, `gateway --help` bound the gateway, `init --help` wrote a config,
# `migrate --help` migrated memory, `update --help` hit the network.
#
# For every command `human help` lists, run `<cmd> --help`, `<cmd> -h` and
# `help <cmd>` in a scratch HOME/state dir with no stdin, and require:
#   - exit 0 within 10 s (a hang means the handler ran)
#   - non-empty output mentioning usage
#   - nothing written to the scratch dirs (a write means the handler ran)
#
# Usage: scripts/check-cli-help-safety.sh [path/to/human]   (default build/human)
# Exit 0 = all safe, 1 = at least one command ran or failed, 2 = cannot measure.

set -u

BIN="${1:-build/human}"
case "$BIN" in /*) ;; *) BIN="$(pwd)/$BIN" ;; esac
if [ ! -x "$BIN" ]; then
    echo "RATCHET_SKIP: no executable at $BIN" >&2
    exit 2
fi

SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/hu-cli-help.XXXXXX")" || exit 2
trap 'rm -rf "$SANDBOX"' EXIT
mkdir -p "$SANDBOX/home" "$SANDBOX/state" "$SANDBOX/cwd"

# cwd is a scratch dir too: main() loads ./.env, and the repo's must not leak in.
run() {
    (cd "$SANDBOX/cwd" &&
        env -i PATH="$PATH" HOME="$SANDBOX/home" HU_STATE_DIR="$SANDBOX/state" \
            HU_CHATDB=/nonexistent/chat.db TMPDIR="$SANDBOX/cwd" \
            perl -e 'alarm 10; exec @ARGV or exit 127' "$BIN" "$@" </dev/null 2>&1)
}

cmds=()
while IFS= read -r name; do
    cmds+=("$name")
done < <(run help | awk '/^Commands:/ {on=1; next} on && /^  [a-z]/ {print $1}')

if [ "${#cmds[@]}" -lt 10 ]; then
    echo "RATCHET_SKIP: 'human help' listed only ${#cmds[@]} commands; cannot measure" >&2
    exit 2
fi

fail=0
checked=0
for cmd in "${cmds[@]}"; do
    for form in "$cmd --help" "$cmd -h" "help $cmd"; do
        # shellcheck disable=SC2086  # form is split into argv on purpose
        out="$(run $form)"
        rc=$?
        checked=$((checked + 1))
        if [ "$rc" -eq 142 ]; then
            echo "FAIL: human $form hung (killed after 10 s) — the handler ran"
            fail=1
        elif [ "$rc" -ne 0 ]; then
            echo "FAIL: human $form exited $rc"
            fail=1
        elif ! printf '%s' "$out" | grep -qi 'usage'; then
            echo "FAIL: human $form printed no usage: $(printf '%s' "$out" | head -1)"
            fail=1
        fi
        written="$(find "$SANDBOX/home" "$SANDBOX/state" "$SANDBOX/cwd" -mindepth 1 | head -3)"
        if [ -n "$written" ]; then
            echo "FAIL: human $form wrote files — the handler ran:"
            printf '%s\n' "$written" | sed 's/^/  /'
            rm -rf "${SANDBOX:?}/home" "${SANDBOX:?}/state" "${SANDBOX:?}/cwd"
            mkdir -p "$SANDBOX/home" "$SANDBOX/state" "$SANDBOX/cwd"
            fail=1
        fi
    done
done

if [ "$fail" -ne 0 ]; then
    echo "check-cli-help-safety: FAIL (${#cmds[@]} commands, $checked invocations)"
    exit 1
fi
echo "check-cli-help-safety: PASS (${#cmds[@]} commands, $checked invocations, none ran)"
exit 0
