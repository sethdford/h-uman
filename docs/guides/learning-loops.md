---
title: Learning loops — outcome join, honest strategy signal, spontaneity gates
created: 2026-10-02
status: operator-facing
---

# Learning loops (`HU_OUTCOME_JOIN`, `HU_STRATEGY_SIGNAL`, `HU_SPONTANEITY`)

Four defects made h-uman's "learn from outcomes" machinery learn nothing
(static-rules inventory, DEF-5 / DEF-8 / DEF-15 / DEF-17). This guide covers
the fixes, the three gates that ship them, and what licenses each gate to go
from SHADOW to LIVE. Every gate defaults to OFF, and OFF is the previous code
path.

| Gate | What LIVE changes | Default |
|---|---|---|
| `HU_OUTCOME_JOIN` | Contact tapbacks on our replies land in `production_outcomes.tapback_polarity` and in `dpo_pairs` | off |
| `HU_STRATEGY_SIGNAL` | The retrieval strategy learner stops writing `success=1` tautologies and stops steering retrieval | off |
| `HU_SPONTANEITY` | The double-text afterthought, self-reaction and GIF can fire after a reply, at Seth's learned rates | off |

## DEF-5: the humanization bandit samples — and stops exploring blind (no gate)

`hu_humanization_decide_contact_params` copied the sampler seed into a local
and dropped it, so θ was a fixed but arbitrary draw from (α, β) for the life of
the process. `hu_humanization_bandit_sample_theta` now draws through the
bandit's own seed, for consumers that report an outcome back. The backchannel
tier (`src/daemon.c` reactive path) never does: nothing credits the arm when a
backchannel lands or flops (the arm learns only from proactive REPLY/IGNORED).
Exploration without feedback is noise, so that decision uses the posterior
mean α / (α + β), which moves only when outcomes move the arm. A TODO marks
the follow-up: a (contact, tier) bandit credited on the reply or the DEF-8
tapback join. Behind the existing `HU_BANDIT_HUMANIZATION` gate (live in prod).

## DEF-8: tapbacks never reached `production_outcomes` (`HU_OUTCOME_JOIN`)

Measured 2026-10-02: `tapback_polarity` NULL in 661 of 661 rows, and zero
`imessage_tapback` DPO pairs ever. Two stacked causes:

1. **The reply's GUID was never resolved.** After sending, the router looked
   up our chat.db row with `m.text LIKE <reply> || '%'`
   (`src/channels/imessage_reactions.c:256` before this change). 1,695 of
   1,725 of our sent rows in the last 30 days have `text` NULL (the body is in
   `attributedBody`), so the lookup missed and
   `src/daemon/daemon_message_router.c:472` registered a synthetic
   `out-<unix time>` ref — 440 of 492 `reaction_lookup` rows. A tapback
   carries the real GUID, so the exact join in
   `src/agent/reaction_handler.c` never hit and returned before reaching the
   outcome writer.
2. **A hit could not land either.** `hu_dpo_record_outcome`
   (`src/ml/dpo.c:480`) only updates rows with `outcome_resolved_at IS NULL`,
   and the contact's text reply resolves the row first (652 of 661).

**Attribution.** `is_from_me = 1` is true of Seth's own typing as well as the
daemon's sends, so the tapped row is attributed to the daemon only through its
own delivery record: the tapback poll returns the target's chat.db ROWID, the
ROWID of our previous message in that chat and its send time; the handler
then needs an `outbound_sends` row for that contact whose chat.db boundary
(`prior_max_rowid`) sits in [previous own ROWID, target ROWID) — so the target
is the first message of ours after that send — and whose `sent_at_ms` is
within 5 s of the target (192 of 197 sends matched within 3 s on
2026-10-02). No such send, no join. That ONE delivery anchors both the
outcome row (`hu_dpo_record_tapback`: the latest reply row sent in
[delivery − 60 s, delivery + 5 s], whatever its resolution state; 60 s is the
FU-1 window inside which nothing but the reply's own bubbles reaches the
contact) and the DPO pair (the reaction_lookup registration in the same
window). A changed reaction (love → dislike) overwrites the polarity and
replaces the earlier pair instead of adding an opposite one. A custom-emoji
tapback (code 2006) takes its polarity from the glyph in LIVE (😢 → −1,
❤️ → +1, unknown → neutral, which records nothing) instead of a blanket +1.

- **off** — the old exact join only.
- **shadow** — finds the row without writing; one line per tapback:
  `[HU_OUTCOME_JOIN shadow] tapback polarity=<-1|0|1> is_from_me_target=<0|1> daemon_sent=<0|1> exact_hit=<0|1> anchored_hit=<0|1> outcome_row=<0|1>`.
- **live** — the same line tagged `live`; writes the polarity and the DPO pair.

**Promotion measurement.** Shadow for at least 7 days. Over lines with
`daemon_sent=1`, `outcome_row=1` and `anchored_hit=1` must each be ≥ 80 %, and
`daemon_sent=1 / is_from_me_target=1` must sit near the share of our sent
chat.db rows that `outbound_sends` claims for the same window (a much higher
share means Seth's own messages are being attributed). Then hand-check 10
joined rows by row ids and timestamps only; any wrong join blocks promotion.

**`reply_sentiment` stays NULL.** There is no local sentiment classifier to
fill it. The only scorer in the tree is a two-list word count private to
`src/context/theory_of_mind.c`, which is a static rule, not a model. The DPO
miner already treats the column as optional.

## DEF-17: the strategy learner learned from a tautology (`HU_STRATEGY_SIGNAL`)

`strategy_outcomes.success` was `count > 0`, written only inside
`if (count > 0)` (`src/agent/memory_loader.c`): 4,850 of 4,850 rows are
successes. `recommend()` picked an arbitrary all-success strategy and its own
picks fed it more successes; every SEMANTIC query row (2,433) is KEYWORD-only.
A real signal — the retrieved item was referenced in the reply AND the contact
engaged, or a verifier supported the claim — is not available at the loader.
So:

- **off** — unchanged.
- **shadow** — unchanged, plus one line per recommendation:
  `[HU_STRATEGY_SIGNAL shadow] category=<n> learned=<n> overrides_default=<0|1>`.
- **live** — writes no rows and returns HYBRID (no override; the adaptive
  query analyzer decides).

**Promotion measurement.** Shadow for 3 days: `overrides_default=1` on most
lines confirms the vacuous history is steering retrieval. Then run a paired
A/B in the shape of `scripts/grounding_ab.py`: same incoming message, gate off
vs live, blinded judge. Live must not lose (win rate ≥ 0.5 over ≥ 40 non-tied
pairs).

## DEF-15: spontaneity was unreachable (`HU_SPONTANEITY`)

The three extras were gated on "the reactive path sent to this contact in the
last 60 s", and the reactive path records that send just before checking them
(`src/daemon.c`, FU-1 recency record), so they could never fire. Their bodies
now live in `src/daemon/daemon_spontaneity.c`.

- **off** — the original path, unreachable as before.
- **shadow** — the original path, plus ONE line per reactive turn:
  `[HU_SPONTANEITY shadow] turn=1 dt=<e>/<p>/<f> sr=<e>/<p>/<f> gif=<e>/<p>/<f> chosen=<kind|none> fired=0`
  (e = eligible, p = the sampled firing probability, −1 when not measured,
  f = would fire).
- **live** — the same line tagged `live`, `fired=0|1`. At most one extra per
  reactive turn: each eligible kind draws p from its learned posterior and
  fires with probability p; among the kinds that fire one is picked uniformly
  (no fixed order). Nothing sleeps on the daemon loop: the extra is queued and
  delivered by the next housekeeping pass (`hu_daemon_spontaneity_tick`,
  `src/daemon/daemon_housekeeping.c`), the afterthought after Seth's own
  measured gap when `double_text_gap_s` is present. Delivery logs
  `[HU_SPONTANEITY live] kind=… delivered=0|1`. The conversation scheduler
  (`hu_conversation_schedule_message`) is not used: the hourly proactive pass
  drains it, so an afterthought would land up to ~90 minutes late, and its
  FU-1 defer drops anything due within 60 s of a reply.

**The learned posterior.** Per (contact, kind), p ~ Beta(r·n0 + successes,
(1 − r)·n0 + failures). The prior is Seth's own P(extra | eligible) `r` and the
number of eligible replies it was measured on `n0`; outcomes move it. After an
extra is delivered, the contact writing back or tapping back on a message the
DEF-8 join attributes to the daemon (the extra or later) is a success; 24 h of
silence (the proactive IGNORED horizon) is a failure; an extra still queued
when they write is cancelled and teaches nothing. Counts persist in
`~/.human/bandit_spontaneity.json`, one log line each:
`[HU_SPONTANEITY outcome] kind=… success=0|1`. The fields are read from the
`global` block of `~/.human/personas/<persona>.learned-style.json`:
`double_text_rate` / `double_text_n`, `self_reaction_rate` /
`self_reaction_n`, `gif_rate` / `gif_n` (n falls back to `n_eff`, then `n`),
and `double_text_gap_s`; re-read every 10 minutes. A kind with no rate never
fires live. `scripts/spontaneity_rate_check.py --emit-learned` prints these
from chat.db (read-only, aggregates only); the learned-style learner should
write them. Measured 2026-10-02 over 180 days: double-text 0.081 of 804
eligible replies, self-reaction 0 of 66, GIF 0.022 of 45, afterthought gap
217 s. Those numbers include the daemon's own sends (chat.db cannot tell them
apart before `outbound_sends` existed), so they are an upper bound on Seth.

**Promotion measurement — one that can fail.**
`scripts/spontaneity_rate_check.py --mode shadow` (then `--mode live`)
compares, per kind, the daemon's fires per reactive turn (the turn lines
above) with Seth's per-reply rate of the same extra, counted only on replies
the same eligibility rules allow (parsed from `src/context/conversation.c`, so
they cannot drift). Two-proportion z-test at 95 %: exit 0 PASS, 1 FAIL (the
daemon fires measurably more or less often than Seth), 2 INCONCLUSIVE (fewer
turns than n·p ≥ 5 needs). `--self-test` proves it fails on a daemon that
over-fires 3×. Shadow must PASS before live; live must PASS after 7 days, then
a blind A/B on 40 turns with extras must not raise detection.

## Rollback

Unset the gate, or set it to `off`, in the launchd plist with
`scripts/install-human-daemon.sh`. Never hand-edit the plist or `cp` over the
running binary. Then run `scripts/verify-deploy.sh`. OFF is the previous code
path for all three gates.
