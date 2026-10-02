---
title: Crisis tiers — one self-harm detector, gate and rollback
created: 2026-10-02
status: operator-facing
---

# Crisis tiers (`HU_CRISIS_TIERS`)

The daemon's SHIELD-005 inbound crisis path, outbound moderation and the
superhuman emotional-first-aid service all read one detector:
`hu_self_harm_classify` in `src/security/self_harm.c`
(`include/human/security/self_harm.h`). Crisis handling is deterministic by
policy; nothing here is learned.

## What it fixed (DEF-1, 2026-10-02)

- "kill myself" hit moderation's **violence** branch (the word "kill"), so it
  got no 988 directive and the voice-memo exclusion did not apply.
- "want to die" was not detected by moderation at all.
- A low "what's the point" set `self_harm` and fired the full crisis directive.
- `superhuman_emotional.c` kept its own list, which disagreed with moderation.

## Tiers

| Tier | Example | Reply directive | Forced reply | Voice memo |
|---|---|---|---|---|
| `explicit` | "kill myself", "want to die", "i wanna die lol", "kms", "suicidal" | `[CRISIS SUPPORT]` with 988 and 741741 (byte-identical to the old directive) | yes | never |
| `low` | "what's the point", "can't do this anymore", "can't go on" | `[CHECK-IN]`: a gentle check-in, no hotline list | yes | never |
| `third_person` | "my friend wants to die", "he's suicidal", "my uncle died by suicide" | `[SUPPORT]`: support the sender as the helper; 988 helps helpers too | yes | never |
| `none` | "this traffic is killing me", "kill the lights", "i don't want to die" | — | — | — |

Matching is whole-word on a canonical form: lowercase, apostrophes dropped
(iOS sends `’`, so "can’t" and "cant" agree), leet only inside words that also
have letters ("k1ll" but not "5 kms"). A short look back at the subject turns a
negation ("i don't", "i'm not", "never") into `none` and a third-person subject
into `third_person`; with no subject it counts as the sender, because texts
drop it. First person outranks low outranks third person. Spaced-letter evasion
("s e l f h a r m") is still caught. A self-harm phrase containing a kill-word
is blanked before the violence check, so "kill myself" is self-harm, not
violence; "kill myself and kill them" is both.

## Gate

`HU_CRISIS_TIERS=off|shadow|live`, **default live** (unset is live).

This deviates from the usual default-off rule on purpose: the old behaviour
missed explicit first-person intent, which is a safety defect, not a feature
awaiting measurement. The rollback is explicit.

- **off**: the legacy lists, byte-identical (pinned by
  `test_daemon_crisis_off_is_byte_identical_legacy` and
  `test_moderation_off_is_legacy`).
- **shadow**: legacy acts; each inbound batch where either side fires logs one
  line `[HU_CRISIS_TIERS shadow] legacy=<tier> tier=<tier> changed=0|1
  contact=#xxxx`. No text, no handle.
- **live**: the canonical tiers everywhere.

Every non-`none` inbound logs `INBOUND crisis detected (tier=<t> score=<s>)
contact=#xxxx` at ERROR.

**Keeping it live:** weekly, count `INBOUND crisis detected` lines by tier in
`~/.human/logs/service-loop-error.log`. Seth reads every `explicit` event's
thread in Messages (not the log, which has no text). A false `explicit` on
hyperbole is acceptable by policy; a missed explicit is not.

**Rollback:** set `HU_CRISIS_TIERS=off` in the daemon's launchd plist
environment and restart the service with `scripts/install-human-daemon.sh`.
