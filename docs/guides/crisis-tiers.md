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
| `explicit` | "kill myself" (any tense, "kill my self", "killmyself"), "want to die", "i wanna die lol", "kms", "suicidal", "no reason to live", "i do not want to live", "dont wanna be here anymore", "i'm going to hurt myself", "cutting myself", "i just want to end it", "they'd be better off without me" | `[CRISIS SUPPORT]` with 988 and 741741 (byte-identical to the old directive) | yes | never |
| `low` | "what's the point" (ending the clause, or "of even trying" / "of this?"), "can't do this anymore", "can't go on like this", "don't want to be here", "want to disappear", bare "hurt myself", "i cut myself shaving", "i almost killed myself", bare "suicide" with no personal context | `[CHECK-IN]`: a gentle check-in, no hotline list | yes | never |
| `third_person` | "my friend wants to die", "he's suicidal", "he tried to unalive himself", "my uncle died by suicide" | `[SUPPORT]`: support the sender as the helper; 988 helps helpers too | yes | never |
| `none` | "this traffic is killing me", "kill the lights", "i don't want to die", "what's the point of this meeting", "can't go on the trip", "watching suicide squad", "a few kms left", "when you say you want to die" (quoting the contact), the 988 resource line itself | — | — | — |

### How it decides

1. **Tokens.** Lowercase words; apostrophes are dropped, including the iOS
   `’`, U+02BC and U+FF07, so "can't", "can’t" and "cant" agree. Leet is
   mapped only inside words that also have letters ("k1ll", not "5").
   Punctuation (`. , ! ? ; : ( )` and newlines) marks a clause break.
2. **Morphology, not a growing list.** "my self" joins to "myself";
   "killmyself" splits; "dont" → "do not", "wanna" → "want to", "cant" →
   "can not". One rule covers kill/unalive/harm/hurt/cut + myself/himself/
   yourself in every tense.
3. **Subject look-back** within the clause, skipping filler: a negation
   ("not", "never") drops the match; "you" drops it (the speaker is quoting
   the contact); a third-person subject makes it `third_person` — **except**
   for phrases whose object is the speaker ("better off without me", "end my
   life", "kms", "wish i was dead", "<verb> myself"), which are never demoted.
   With no subject, the sender is assumed (texts drop it: "wanna die lol").
4. **Context rules.** "end it" needs intent ("want to", "going to") and must
   end its clause ("end it with her" is not). "hurt myself" is `explicit` only
   with intent, a habit ("hurting myself") or "again"; "cut myself" is
   `explicit` unless an accident follows ("shaving", "cooking"). "almost/nearly
   killed myself" is `low`. "kms" after a number or quantity ("10 kms", "a few
   kms") or before "away/left/…" is a distance.
5. **Ruling on low.** `low` forces a reply, so it needs personal-despair
   context: "what's the point" must end its clause or take a despair object
   ("of even trying", "of anything", "of this?"); "can't go on" must end its
   clause. A meeting gripe is `none`.

## The reply side

The reply's own words are never treated as a crisis. Before 2026-10-02 the
directive made the model write "988 Suicide & Crisis Lifeline"; moderation
read "suicide" as explicit self-harm, the bus deferred the reply, and the
fallback path sent "ugh brain fart — lemme rephrase that" plus a canned 988
block instead. Now:

- The resource line is appended **only** when the **inbound** tier is
  `explicit` and the reply lacks "988" (`hu_daemon_crisis_ensure_resources`,
  `hu_self_harm_reply_needs_resources` in `agent_turn`/`agent_stream`). Never
  twice.
- Self-harm wording in a reply never blocks or replaces it (the final gates
  and `hu_daemon_reply_blocked` look only at violence, hate and sexual).
- A reply that is unsafe for another reason is **dropped**, not replaced:
  there is no canned text on this path.

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
