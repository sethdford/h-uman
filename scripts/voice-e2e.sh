#!/usr/bin/env bash
# End-to-end check of the voice gateway: speaks a phrase with `say`, sends it
# through VoiceSession over a real WebSocket, and reports the transcript, the
# reply text and reply-audio timing. Needs a voice gateway already running.
#
#   scripts/voice-e2e.sh                       # default URL and phrase
#   scripts/voice-e2e.sh "what's on tomorrow"  # custom phrase
#   HU_VOICE_E2E_URL=ws://127.0.0.1:3007/ws scripts/voice-e2e.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PHRASE="${1:-Hey, how is your day going so far?}"
export HU_VOICE_E2E_URL="${HU_VOICE_E2E_URL:-ws://127.0.0.1:3007/ws}"
export HU_VOICE_E2E_VOICE="${HU_VOICE_E2E_VOICE:-ebaf7477-b6ae-417e-be54-19e6176777ea}"
export HU_VOICE_E2E_MODEL="${HU_VOICE_E2E_MODEL:-sonic-3.6-2026-08-27}"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/voice-e2e.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
export HU_VOICE_E2E_WAV="$tmp/utterance.wav"
say -o "$HU_VOICE_E2E_WAV" --data-format=LEI16@16000 "$PHRASE"

cd "$ROOT/apps/shared/HumanKit"
swift test --filter VoiceGatewayE2ETests 2>&1 \
  | grep -E '\[voice-e2e\]|error|failed|skipped|passed \(' || true
