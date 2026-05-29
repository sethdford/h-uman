#!/usr/bin/env bash
# check-agent-core-boundary.sh
#
# Agent-core boundary guard (DDD bounded-context refactor, Phase 0). The agent
# orchestration core must not (a) instantiate providers via the factory
# directly — it gets a provider vtable injected — or (b) branch on channel
# identity by string memcmp (channel knowledge belongs in the Channels
# context, behind hu_channel_behavior_class_for_name()).
#
# Both checks are RATCHETS (baseline current count, fail on growth) so the
# guard is green today and safe to wire into pre-commit. Phase 1 removes the
# channel-name memcmp from the turn loop and Phase 4 removes the factory
# includes — when they land, lower the matching BASELINE (to 0) to lock it.
set -euo pipefail

# Measured 2026-05-29 at the start of Phase 0.
FACTORY_BASELINE=4   # Phase 4 (provider injection) drives this to 0.
MEMCMP_BASELINE=11   # Phase 1 (channel behavior_class) drives this to 0.

cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

fail=0

fac=$(grep -rln '#include "human/providers/factory.h"' src/agent 2>/dev/null \
  | wc -l | tr -d ' ')
echo "agent/ provider-factory includes: $fac (ceiling $FACTORY_BASELINE)"
if [ "$fac" -gt "$FACTORY_BASELINE" ]; then
  echo "FAIL: new direct provider-factory include in agent core. Inject the" >&2
  echo "      provider vtable instead of instantiating via the factory:" >&2
  grep -rln '#include "human/providers/factory.h"' src/agent >&2
  fail=1
elif [ "$fac" -lt "$FACTORY_BASELINE" ]; then
  echo "NOTE: factory includes dropped — lower FACTORY_BASELINE to $fac." >&2
fi

memcmp_pat='memcmp\([a-zA-Z_]+ *, *"(imessage|slack|telegram|discord|whatsapp|signal|sms|email|voice|imap|gmail|mattermost|matrix|irc|line|lark|messenger)"'
chan=$(grep -rnE "$memcmp_pat" src/agent 2>/dev/null | wc -l | tr -d ' ')
echo "agent/ hardcoded channel-name memcmp: $chan (ceiling $MEMCMP_BASELINE)"
if [ "$chan" -gt "$MEMCMP_BASELINE" ]; then
  echo "FAIL: new hardcoded channel-name memcmp in agent core. Use" >&2
  echo "      hu_channel_behavior_class_for_name() (Channels context):" >&2
  grep -rnE "$memcmp_pat" src/agent >&2
  fail=1
elif [ "$chan" -lt "$MEMCMP_BASELINE" ]; then
  echo "NOTE: channel memcmp dropped — lower MEMCMP_BASELINE to $chan." >&2
fi

if [ "$chan" -gt 0 ]; then
  echo "REMINDER: Phase 1 (channel behavior_class) should drive the channel" >&2
  echo "          memcmp count to 0; then set MEMCMP_BASELINE=0 here." >&2
fi

exit $fail
