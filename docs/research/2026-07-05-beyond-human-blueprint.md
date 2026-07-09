---
title: "Beyond-Human Blueprint — seven mechanisms past indistinguishability"
created: 2026-07-05
status: active
---

# Beyond Human — the bar-raising blueprint (2026-07-05)

Premise (measured, not aspirational): parity with Seth's voice is the *ceiling*
on the writing axis — no model writes a more-Seth text than Seth. "Better than
human" is only real on axes humans **can't** do. Everything below is designed
to be legibly superhuman to the *recipient*, built ≥70% from subsystems h-uman
already has, and gated on measurement per the project contract.

## 1. Measurement-as-conversation (the ambient blind-A/B drip)
**The keystone problem, solved creatively.** The 12-row rating sheet has sat
unrated for a month because it's homework. Instead: after every ~Nth exchange,
h-uman texts Seth's SELF-CHAT one micro-question — the real inbound + two
candidate replies — "which sounds more like me, A or B?" One tap. Each answer
appends a row to the rating store; `score.py` recomputes the gate verdict
nightly; adapters promote themselves when the drip says so.
- Reuses: self-chat channel, blind_ab scoring, verdict gate + freshness guard.
- New: a tiny "rating drip" tick (governed by the same proactive budget).
- Industry bar raised: **continuous human-grounded eval as a byproduct of
  using the product** — no lab, no sheet, no chore. Nobody ships this.

## 2. The Promise-Keeper guarantee
Commitments are already extracted (71 stored) and now schedule follow-ups.
Upgrade it to a *product guarantee*: a kept/broken ledger per contact, surfaced
in-voice ("i said i'd send that — here") and auditable. Humans break small
promises weekly; a companion that provably never drops one is superhuman in a
way recipients FEEL.
- Reuses: commitments store, delayed_followups (now wired), proposer triggers.
- New: kept/broken resolution when a follow-up completes + a ledger table.

## 3. Relationship vital signs + weekly repair pass
Per-contact heartbeat computed from what's already logged: recency, reciprocity
score, warmth trend (reaction polarity), unanswered-question count. A weekly
tick finds the two most-decayed relationships and queues ONE repair action
each into the proposer's due-triggers ("Mom asked about Thanksgiving 6 days
ago — unanswered").
- Reuses: reciprocity_scores, interaction_quality, contact_mood_log, absence
  detection, the trigger-based proposer.
- Superhuman axis: no human runs telemetry on 40 relationships.

## 4. The dream cycle (make 4am a mind, not a cron)
Autodream + consolidation already exist. Chain them into a nightly ritual with
an OUTPUT: replay the day's conversations → write 3 "morning seeds" (things it
meant to ask, loops left open, one callback) into prospective triggers the
proposer reads at breakfast time. The persona wakes up with intentions.
- Reuses: autodream_runs, consolidation, temporal_events, proposer triggers.
- Also the natural home for the nightly train + verdict recompute: sleep =
  consolidate + learn + set tomorrow's agenda. That's a STORY the industry
  doesn't have, backed by real machinery.

## 5. "On this day" — longitudinal callbacks
Anniversary/echo retrieval over episodes + temporal_events: "a year ago you
were sweating the Vanguard reorg — look how that landed." Requires only a
date-windowed query + a proposer trigger type. Perfect recall across months is
the single most legibly-superhuman memory behavior.

## 6. Honest uncertainty (ask, don't bluff)
Self-uncertainty signal exists (HU_SELF_UNCERTAINTY=on). Wire the LOW-confidence
branch to a clarifying question in-voice ("wait, which alex?") instead of a
confident guess. Humans do this constantly; assistants bluff. Trust compounds.

## 7. Voice drift tracking (stay current with the person)
style_fingerprints already log per-contact style. Add a slow EWMA of SETH'S OWN
outbound style (new slang, emoji shifts, punctuation drift) and diff it against
the adapter's training vintage; when drift exceeds a threshold, flag "voice
stale — retrain recommended" into the nightly report. Static fine-tunes go
stale silently; this one knows when it no longer sounds like you.

## Sequencing (leverage ÷ effort)
1 (rating drip — unblocks everything measured) → 2 (promise ledger) →
4 (dream cycle seeds) → 3 (vital signs) → 5 (on-this-day) → 6 (ask-don't-bluff)
→ 7 (drift tracker). Each ships OFF→SHADOW→LIVE behind the existing gates.

Every item above turns an existing, verified subsystem into a behavior a
recipient can *feel* — which is the only bar that matters.
