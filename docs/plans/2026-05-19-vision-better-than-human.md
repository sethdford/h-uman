---
title: What "Better Than Human at iMessage" Actually Means
status: closed
created: 2026-05-19
last_audit: 2026-05-25
---

# What "Better Than Human at iMessage" Actually Means

> Round 4 closed the self-improvement loop. The next 10 rounds add the
> capabilities that compound on it. This doc is the vision the rounds
> serve: what specifically about h-uman should exceed the human it
> impersonates.

## The bar isn't "better than the average human texter"

Most humans are mediocre texters. Beating that bar is uninteresting —
it just means we wrote a competent reply generator. The bar that
matters is:

  **Given a conversation YOU are in, would a response from h-uman
  land better than YOUR OWN response would?**

That's measurable. It's also surprisingly tractable, because humans
have specific systematic weaknesses at texting that machines don't
have to share.

## Dimensions where h-uman can plausibly exceed human-Seth

| Dimension | Why human-Seth loses | What h-uman has |
|---|---|---|
| **Consistency under fatigue** | 11pm + headache produces worse texts than 9am + coffee | No fatigue, same quality 24/7 |
| **Recall of details** | "What was Casey's sister's name again?" | personal_model with structured facts (when C3 lands) |
| **Tone calibration to person** | Defaults to your own voice + mood | Per-contact warmth + style overlay |
| **Best-of-N drafting** | Humans send first version | L5 best-of-5, argmax P(Seth) (when C2 lands) |
| **Proactive follow-up** | "I forgot to ask how the interview went" | Commitment store + scheduler (when C4 lands) |
| **Modality choice** | Default to text even when tapback fits | L4 multimodal (shadow today, live next) |
| **Cross-conversation learning** | Doesn't get systematically better | AGI-C1 production loop, running now |
| **Mood independence** | Sad-Seth texts differently than happy-Seth | No moods to leak |
| **Multi-conversation parallel** | ~3 max | N parallel |
| **Reaction-time variance** | Sometimes too fast (impulsive) or too slow (forgotten) | Calibratable per relationship |

## Dimensions where humans still win (the honest accounting)

| Dimension | Why human-Seth wins | What h-uman would need |
|---|---|---|
| **Just-happened events** | "got promoted today" — Seth knows before texting | Real-time calendar/event ingest |
| **Embodied sensory** | "this coffee is amazing" — real coffee, real taste | Truly no good answer; route to "ask Seth" or punt |
| **Subtle relational dynamics** | Knows Pat is going through a divorce; adjusts | C3 memory consolidation + careful prompting |
| **Long arc humor** | 6-month running joke between friends | example_banks + per-contact context |
| **In-person handoff** | "the thing I told you yesterday" | Calendar + location + earlier conversation state |
| **Genuine novelty** | A friend just got engaged; needs a non-template response | Always a gap; route to "draft for Seth to edit" |

The honest design call: **h-uman should be obviously-better on the first
table, transparently-deferred on the second**. The worst failure mode is
faking knowledge in the second column. Better to defer gracefully —
"hey send Seth this directly" — than to confabulate.

## The remaining 11 rounds, ordered

### Round 5 — finishing AGI-C1 (the loop has gaps)

The production loop fires but three knobs aren't fully turned:

- **C1d: reply-latency ingest.** When an inbound arrives, compute
  latency from our prior send to this inbound, call record_outcome.
  Currently only the tapback path records outcomes. Latency is the
  universal signal — most messages don't get tapbacks; almost all
  get replies eventually.

- **C1e: alternatives column.** Currently NULL because production
  doesn't run best-of-N. After C2 lands, L5 in production writes its
  rejected candidates here, unlocking the inverted-pair signal in
  outcomes_to_dpo.

- **C1f: P(Seth)-at-send computed in C.** Currently passes -1.0
  because the C classifier port is deferred. Without it, the column
  is always NULL; the loop still works (outcomes_to_dpo can
  backfill) but we lose the ability to FILTER outbound by quality
  before sending.

  P1 = P5b from round 3's deferred list: port `personaeval_speaker_id_v2`
  to C. ~200 LOC, isolated module.

**Round 5 size: ~10 hours.** Closes the loop completely.

### Round 6 — C2 meta-cognitive uncertainty

The capability that turns h-uman from a "always sends" agent into a
"knows when not to send" agent. Decision logic:

  if   P(Seth) >= 0.8  → ship single-shot
  elif P(Seth) >= 0.5  → fire best-of-N (n=5), pick filtered argmax
  else                 → DEFER: send a clarifying question or no
                                response, log for review

Requires Round 5's P5b (C classifier). Wires into the daemon at the
same site as the L4 shadow log. Threshold is calibratable.

**Round 6 size: ~3 hours after P5b.** The "knows what it doesn't know"
property.

### Round 7 — C3 per-conversation summary → personal_model update

After a conversation lulls (6+ hours since last message), trigger an
LLM summarization that extracts structured facts:

  "Casey is moving to Austin in July"
  "Morgan's manager moved the deadline on 2026-05-19 causing tension"
  "Pat figured out the CI tests timezone bug"

Each fact uses the existing `hu_fact_extract` typed-triple API (subj/
pred/obj + confidence + provenance). Facts feed back into future
turns as personal_model context.

The model **literally learns about people from talking to them**.

**Round 7 size: ~6 hours.** Independent of P5b.

### Round 8 — C4 commitment-driven proactive follow-up

The commitment_store exists; today commitments are recorded but never
trigger anything. C4 wires:

- New column on commitments: `proactive_due_at` (default = `due_at - 24h`)
- Daemon scheduler scans for commitments approaching proactive_due_at
- For each: check if the conversation already resolved it (search
  recent messages for the commitment's topic)
- If unresolved: trigger an h-uman draft of a follow-up message
- Send via the existing proactive path (rate-limited, throttled)

This is the "Seth remembers everything" capability humans famously
fail at.

**Round 8 size: ~4 hours.** Independent.

### Round 9 — C5 reflexion on negative outcomes

When tapback_polarity = -1 OR reply_latency_s > 6h on otherwise-OK
message: trigger a reflexion LLM call.

  "I sent '[chosen]' in response to '[prompt]'. It didn't land
   (negative tapback / no reply in 6h). What was wrong about it?
   Output: a one-line instruction to avoid this for similar prompts."

The output is added to a `production_lessons` table; the persona
prompt builder includes lessons relevant to the current channel.

This is the Voyager / Reflexion architecture. After 100 negative
outcomes, the persona has a list of "don't do X in context Y" rules
that aren't from training — they're from real production failures.

**Round 9 size: ~6 hours.** Requires AGI-C1 outcome data (have).

### Round 10 — multi-turn proactive ("text 3 days later")

The line between "annoying assistant who keeps poking" and "real
friend who follows up" is delicate. Heuristics:

- Detect conversation lulls vs natural endings (latent state inference)
- For each open commitment in the thread, decide if a follow-up would
  feel natural OR forced
- Time the follow-up to the contact's typical activity window
- Use earlier conversation state as anchor ("how'd the doctor go?")

This is where h-uman stops being a chatbot and starts being a friend.

**Round 10 size: ~8 hours.** Requires C3 + C4 + tasteful heuristics.

### Round 11 — actual voice memos (not just deciding to send one)

L4 already decides "voice memo would fit." Nothing sends one.
Wiring:

- Local voice synthesis (XTTS-v2 or similar, voice-cloned from Seth's
  example audio)
- iMessage attachment path with audio file
- Latency budget — voice memos must arrive within ~30s of the
  decision, otherwise the moment passed

Heavier infrastructure than other rounds. Worth deferring until voice
synthesis quality on local hardware is good enough.

**Round 11 size: ~12-20 hours.** Depends on voice synthesis choice.

### Round 12 — per-contact persona overlays

example_banks have per-channel overlays. AGI-Tier: per-contact
overlays. "How I text my sister" ≠ "how I text my coworker."

Mechanism:
- Track which example_bank entries the user has confirmed are
  contact-specific
- Build a contact_overlay that further specializes the channel
  overlay
- The right persona for the right person, automatically

**Round 12 size: ~5 hours.** Independent.

### Round 13 — draft-then-edit UI + edit-as-reinforcement

The strongest possible training signal: the user edits a draft before
sending. That's an explicit (chosen = original_draft, rejected =
final_edit) pair. Today nothing captures this.

Requires:
- UI surface for h-uman to PROPOSE a draft instead of auto-sending
  (probably via the existing PWA channel or a Mac menu-bar app)
- Capture both the proposed draft AND the user's edited version
- production_outcomes row with user_edited=1 and the diff stored

**Round 13 size: ~10 hours.** Includes UI work — the only round with
non-C work.

### Round 14 — cross-channel persona transfer

Currently each channel has its own overlay; the underlying persona is
static. AGI-Tier: persona evolves; channels are projections of that
evolution.

If h-uman learns through iMessage tapbacks that "starting with 'Of
course!' is bad," the lesson should apply to Slack and Discord and
email too — without re-learning per channel.

**Round 14 size: ~6 hours.** Requires C3 + C5.

### Round 15 — the AGI moment: simulated self-play

Run h-uman against h-uman as the user. Generate millions of synthetic
conversations. Score with the same classifiers. Train on the
high-P(Seth) ones.

This is the Voyager skill-library idea applied to conversation. The
model doesn't NEED real conversations to keep improving — it can use
its own predictions as substrate.

Risks: mode collapse (h-uman converges to a low-diversity attractor),
adversarial drift (h-uman optimizes for what its own classifier
likes, not what humans like). Mitigated by:
- Periodic grounding to real production_outcomes
- Ensemble of classifiers (PersonaEval v2 + outcome-DPO-trained model
  + maybe a third)
- Held-out test set of real exchanges that never enter training

**Round 15 size: ~20 hours.** The genuinely speculative round.

## What's NOT in this map (truly out of scope)

- **Pre-cognitive proactive messaging** ("text X right now because
  something is about to happen") — would require predicting human
  intent, not just modeling past behavior
- **Mood inference from typing cadence** — would need keystroke-level
  data we don't collect
- **Calendar + weather + location grounding integrated into reply** —
  the data is there; integration is more rounds than we've sketched
- **Real-time meeting summarization → reply context** — needs voice
  agent we don't have

These are real ideas but they're more "different products" than
"better iMessage conversationalist."

## The order I'd recommend

```
Round 5   (~10 hr)  finish AGI-C1: P5b + C1d latency + C1e alternatives
  └─ unlocks Round 6 (meta-cognitive uncertainty) → ~3 hr
Round 7   (~6 hr)   memory consolidation (independent)
Round 8   (~4 hr)   commitment follow-up (independent)
Round 9   (~6 hr)   reflexion on negatives (uses C1 data)
Round 10  (~8 hr)   multi-turn proactive (requires 7+8)
─── pause: ship to real production, collect 4 weeks of data ───
Round 11  (~12-20)  voice memos
Round 12  (~5)      per-contact persona
Round 13  (~10)     draft-then-edit UI + reinforcement
Round 14  (~6)      cross-channel transfer
Round 15  (~20)     self-play
```

Rounds 5-10 are the core of "better than human at texting." After
those, h-uman is plausibly better than human-Seth-on-his-average-day.
Rounds 11-15 are amplifiers.

## How I'd measure success at the end of each round

Round 5: outcomes_to_dpo writes ≥10 inverted-pair rows / week.
Round 6: defer rate < 5%, but no defer-then-bad-quality regret.
Round 7: personal_model facts grow ≥3/week; recall test on retained
        facts > 80%.
Round 8: at least 1 proactive follow-up per week that the user
        confirms was "good timing."
Round 9: production_lessons table has ≥20 rules after 4 weeks; persona
        prompt builder includes them; subsequent same-context outcomes
        improve.
Round 10: tapback positive rate on proactive messages ≥ tapback
         positive rate on reactive messages.
Round 11: voice memo sent rate matches L4's voice decisions; user-rated
         quality ≥ 7/10.
Round 12: per-contact overlay differential (e.g., emoji density to
         sister vs coworker) is statistically significant.
Round 13: user_edited=1 rate trends down over 8 weeks (drafts get
         better).
Round 14: a lesson learned on one channel improves shape-score on a
         different channel within 2 weeks.
Round 15: training on synthetic data produces non-zero lift on the
         held-out real test set.

## The honest closing thought

"Better than human" isn't a single threshold — it's a set of
properties humans have or lack, and h-uman is uneven across that set.
Today, after 4 rounds, h-uman:

- ✅ Beats fatigued-Seth on consistency
- ✅ Holds shape across short conversations (single-turn closed)
- ✅ Has the production loop to keep improving
- ⏳ Doesn't yet route tapbacks live (shadow only)
- ⏳ Doesn't remember facts across conversations
- ⏳ Doesn't follow up on commitments
- ⏳ Doesn't know when it doesn't know
- ❌ Has no embodied / sensory grounding
- ❌ Has no real-time event awareness

Rounds 5-10 close most of the ⏳ rows. The ❌ rows are honestly out of
scope; the design discipline is to ROUTE those cases ("ask Seth
directly") rather than fake them.
