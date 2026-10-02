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

## DEF-5: the humanization bandit now samples (no gate)

`hu_humanization_decide_contact_params` copied the sampler seed into a local
and dropped it, so θ was a fixed function of (α, β) for the life of the
process. `hu_humanization_bandit_sample_theta` draws through the bandit's own
seed, so every call is a fresh Thompson draw. This sits behind the existing
`HU_BANDIT_HUMANIZATION` gate (live in prod); with it off nothing changes.

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

The fix resolves the reacted-to message in the tapback poll itself
(`target_is_ours`, `target_sent_unix`), joins by thread and send time when the
exact ref misses, and writes through `hu_dpo_record_tapback`, which matches by
`message_ref` or by the latest row sent in `[sent − 600 s, sent + 15 s]`,
whatever its resolution state.

- **off** — the old exact join only.
- **shadow** — finds the row without writing; one line per tapback:
  `[HU_OUTCOME_JOIN shadow] tapback polarity=<-1|0|1> ours=<0|1> exact_hit=<0|1> time_hit=<0|1> outcome_row=<0|1>`.
- **live** — writes polarity on the row and records the DPO pair.

**Promotion measurement.** Run shadow for at least 7 days. Over the lines with
`ours=1`, `outcome_row=1` must be ≥ 80 % and `time_hit=1` ≥ 80 %. Then hand-check
10 joined rows: the row's `send_timestamp` must be the reply that precedes the
tapped message's chat.db `date` in the same thread (compare by row id and
timestamps only; never print text). Any join to the wrong reply blocks
promotion.

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
- **shadow** — the original path, plus one line per eligible extra:
  `[HU_SPONTANEITY shadow] kind=<double_text|self_reaction|gif> eligible=1 rate_src=<learned|none> p=<0..1> would_fire=<0|1> capped=<0|1>`.
- **live** — at most one extra per reactive turn. An eligible extra fires with
  `p = learned_rate × (0.8 + 0.45·θ)`, where `learned_rate` is Seth's measured
  rate and θ a fresh draw from the contact's humanization-bandit arm (×1 for a
  contact with no outcomes). Fires log `[HU_SPONTANEITY live] kind=… p=… fired=1`.

`learned_rate` comes from the optional fields `double_text_rate`,
`self_reaction_rate` and `gif_rate` (each in [0, 1]) of
`~/.human/personas/<persona>.learned-style.json`, re-read every 10 minutes.
A missing field means not measured: that extra never fires live and is only
logged. Eligibility (no farewell, not three of our last four messages, no GIF
on a question or sad news, the GIF rate cap) is the legacy predicates'.

**Promotion measurement.** Shadow for 7 days with the learned rates present.
For each kind, `would_fire=1 / eligible` must sit inside Seth's own measured
rate ±50 %. Then run a blind A/B on 40 turns that include extras; detection
must not rise above the current human-gate value.

## Rollback

Unset the gate, or set it to `off`, in the launchd plist with
`scripts/install-human-daemon.sh`. Never hand-edit the plist or `cp` over the
running binary. Then run `scripts/verify-deploy.sh`. OFF is the previous code
path for all three gates.
