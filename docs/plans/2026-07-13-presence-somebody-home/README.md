---
title: "Somebody Home — from responder to self"
status: active
created: 2026-07-13
last_audit: 2026-07-13
owner: cognition + behavior + agent + learning
related:
  - docs/plans/2026-05-29-seth-aliveness/README.md
  - docs/plans/2026-05-29-intrinsic-motivation/
  - docs/plans/2026-05-29-humanness-north-star-metric/
  - docs/plans/2026-05-29-persona-vectors-runtime/
  - docs/standards/engineering/bounded-contexts.md   # Modeled Person: persona→cognition→behavior
---

# Somebody Home

> Everything shipped through 2026-07-12 **removed negatives** — leaks, garbled
> stammers, the rule contradictions, the terminal-period tell. That moves the
> system from "obviously a bot" to "not obviously a bot." It adds no presence.
> This plan is the other kind of work: putting **someone behind the glass**.

## The thesis (why nothing so far made it feel like Seth)

The base model's entire post-training made it an **assistant**: helpful,
responsive, present, agreeable — an orientation baked far deeper than any
rank-8 LoRA or prompt reaches. Its fundamental stance is *serve the
conversation well*. A person's stance is the opposite: they bring a **self** —
a want, an avoidance, a mood that leaks, a day that intrudes, opinions they
defend past politeness. Humans aren't primarily responsive; they're primarily
**self-having**. Every "tell" we've chased is downstream of that one gap.

Continuity and intuition — the two things that feel missing — are not two
features. They are both *what a self produces*. You cannot post-process a self
onto a responder. The frontier is the shift from **"respond well"** to
**"have somebody home."**

## The reframe after reading the code (2026-07-13, verified)

The interior is **already largely built** — it is dormant, per-turn, and
advisory, not persistent, deciding, and shipping. This is an
**activation / persistence / agency / training** plan over a rich substrate,
not a greenfield build. That matches h-uman's whole history: the gains come
from wiring built-but-dormant subsystems, not new ML.

What exists (verified):

| Substrate | Where | State |
|---|---|---|
| Interoception (energy, social battery, "tired") | `hu_somatic` in `agent->frontiers`, updated per turn (`agent_turn.c:3419`); gates behavior (`:5576`) & choreography | **built, persists within an agent's life, already decides a little** |
| Cognition layer | `src/cognition/`: presence, attachment, dual_process, emotional, episodic, intrinsic_drive, metacognition, novelty, rupture_repair | **built**; mostly feeds prompt context |
| Behavior layer | `src/behavior/`: affect, dialog_act, policy, pressure, prosocial, rel_dynamics, support_strategy, win_detect | **built** |
| Theory of Mind | `hu_tom` (`src/agent/theory_of_mind.c`) — per-turn expectation/belief/gap | **built, shallow, per-turn** |
| Agency / initiative | `hu_init_proposer` — decides whether to reach out first | **partial**: persona/contact/conversation/instruction wired; **memory/personal_model/awareness/stm still zero-stubbed ("T2 stub")**; propose→send tail unwired for A3 intrinsic |
| Learning loop | DPO/RLAIF, `dpo_pairs`, `m3-rewrite-pairs`, KTO | **built**; not yet training against the assistant-prior |

The four gaps, precisely:

1. **The interior decorates the prompt; it rarely decides.** `somatic` gates a
   couple of thresholds, but cognition/behavior mostly render *context text*
   the model may ignore. A self *decides* — whether to reply, when, how terse,
   whether to bring its own thing.
2. **No one is home across the real timescale.** `frontiers` init once per
   agent and update per turn — but is the agent long-lived across the daemon's
   turn dispatches, and does any of it survive a **daemon restart**? If the
   interior resets on restart (it does today — nothing persists somatic/mood/
   agenda to disk), then across the days-and-weeks arc of a real relationship,
   there is no one home. **This is the crux investigation (P0 below).**
3. **The agency tail is stubbed.** The proposer decides with partial context;
   intrinsic/prosocial routines log intent but never send.
4. **Nothing fights the assistant-prior.** Training reinforces "good answers,"
   never penalizes "too helpful / too agreeable / asks a follow-up every turn."

## P0 — The crux investigation (do FIRST, it re-shapes everything)

**Question:** across the lifetime of a real relationship (turns within a
conversation, conversations across days, and *daemon restarts*), what interior
state persists, and what resets?

**Method (read + instrument, no behavior change):** trace the `hu_agent_t`
lifecycle in the daemon — is there one long-lived agent per contact, or a fresh
agent per inbound batch? Log `frontiers.somatic.energy` / `attachment` / any
mood at turn start across (a) consecutive turns, (b) a restart. Confirm what is
durable.

**Deliverable:** a one-page map of "what's home between turns / conversations /
restarts today." Every workstream below depends on the answer. If the interior
is session-scoped (likely), **durable persistence is the foundation, not an
add-on.**

## The four workstreams (mapped to the diagnosis)

Each ships **OFF → SHADOW → LIVE**, gated to LIVE only by a human verdict
(`feature-gate-requires-measurement`). Nothing changes what SHIPS until Seth's
gut says it's more him. None of it touches the model, adapter, or the send
path's safety guards.

### WS1 — Continuity: memory as lived story, not a briefing

*Problem:* `memory_loader` assembles a fact-sheet rebuilt each turn; the model
reads it like a new hire skimming a handoff doc. (It's also where today's
NUL-overflow crashloop lived — the substrate isn't even clean.)

*Build:*
- A **durable per-contact relationship state**: open loops ("we never finished
  the beach-house thread"), the emotional arc since last contact, "what we were
  mid-conversation about," running warmth/tension. Not facts — *narrative
  state* that updates and persists to disk (extend `personal_model` /
  `cognition/episodic`, `cognition/evolving`).
- The loader assembles **continuity** (unresolved + arc + last real moment)
  ahead of recalled facts.
- **Memory hygiene:** sanitize embedded NULs / non-text at the memory *write*
  boundary (root of Bug A/B, `memory_loader_nul_overflow` memory). A history
  you can't trust to be clean can't become continuity.

*Feels like:* "knowing," not "informed." Picks a thread back up unprompted.

### WS2 — An interior that persists and costs something

*Problem:* the model resets to "warm and present" every turn. Real texture is
**variance** — terse when busy, cold when annoyed, half-there when distracted.

*Build on `hu_somatic` + `cognition/`:*
- **Durable mood/energy/preoccupation vector per contact/global**, evolving on
  its own clock (circadian `mood.c`/`circadian.c`, recent load, time since last
  contact) and **surviving restarts**. Today `somatic` is per-agent-life only.
- Elevate it from prompt-context to **decision + generation shaping**:
  - decision: whether to reply now, how terse, whether to leave-on-read;
  - generation: project mood onto output via the **activation-steering** rig
    that already exists (`persona-vectors-runtime`, built, default-off) — a
    "tired/curt/warm" residual vector, not a content instruction.
- Bounded and safe: crisis/grief/required directives always override mood
  (reuse the salience safety floor).

*Feels like:* the same person on a good day and a bad day. Not always "on."

### WS3 — A self with stakes (agency + training against the prior)

*Problem:* the model serves; it never brings its own thing. Proposer context is
stubbed; A3 intrinsic logs intent but never sends.

*Build:*
- **Finish `init_proposer` context** — wire the remaining memory /
  personal_model / awareness / stm fields (the "T2 stub"). Agency decided on
  full context, not a third of it.
- **Close the propose→send tail** so intrinsic-motivation (A3) and prosocial
  routines can actually *initiate* — gated, rate-limited, and fed by the
  proactive-engagement reward already collected (the governor/bandit), so
  frequency self-tunes per contact and a miscalibrated text costs less than
  ten good replies earn.
- **A persistent agenda:** threads the self wants to continue, things it wants
  to bring up — surfaced when apt, dropped when stale.
- **Train against the assistant-prior (the deep lever):** build a DPO/KTO
  corpus where the *rejected* sample is the tell — "asks a follow-up question
  every turn," "too agreeable," "perfectly serves," "certainly-shaped
  helpfulness." Reuse the existing DPO/RLAIF loop and `m3-rewrite-pairs`
  pipeline. This is the only lever that moves the *weights'* orientation; the
  rest shapes around a frozen prior.

*Feels like:* friction. Sometimes disagrees, sometimes doesn't ask, sometimes
starts the conversation. The not-perfectly-serving-the-moment that reads human.

### WS4 — Theory of mind that accumulates

*Problem:* `hu_tom` models per-turn expectations; there's no persistent,
deepening model of the *other* person's interior across the relationship.

*Build:* durable per-contact ToM that accumulates — what they're going through,
their patterns, what "you up?" means from *them specifically*, an emotional
trajectory. Answer the **unspoken** message, not the literal one. Extend
`hu_tom` + `personal_model`; feed WS1's continuity and WS2's read of the room.

*Feels like:* "gets me." Reads subtext.

## Measurement — the human is the only instrument

The rating drip measures whether *short exchanges* are indistinguishable from
*a* human — a strictly easier bar than *this specific person with a self*.

- **Keep** the human-tier rating drip; **retire** the saturated synthetic judge
  as a gate.
- **Add harder measures:** relationship-arc / multi-turn evaluation; a
  Seth-specific "does it feel like ME" self-rating; **friction metrics** (did it
  ever *not* reply, *not* ask a question, disagree, initiate?); a longitudinal
  "did it bring something up on its own that landed."
- Every WS LIVE-promotion gates on Seth's gut, not a metric. The gut is the
  most accurate signal in the system; honor it.

## The honest ceiling

A frozen model that regenerates from a prompt each turn has **no one home
between the turns** except what we re-inject and persist. Durable state +
agency + steering is *simulated* interiority bolted onto a responder, and it
has a ceiling. What this plan can genuinely reach: persistent mood that shapes
output, memory that reads as continuity, a self that initiates and sometimes
refuses, a prior nudged away from pure helpfulness. What it **cannot** fully
reach without deeper architecture: a continuously-present self. Closing the last
gap likely needs work that fights the base model harder than any adapter —
larger-rank or continued-pretraining on Seth's corpus, and eventually a runtime
that carries **latent state across turns**, not just re-injected text. Name that
honestly; aim "indistinguishable" at short casual exchanges (reachable soon) and
be patient about sustained, high-stakes, long-context conversation (the hard
tail). This is asymptotic — you approach "somebody home," you don't cross a
finish line.

## Sequencing, risk, discipline

1. **P0 crux investigation** (read-only) — what persists today. Everything
   depends on it.
2. **WS1 durable memory + hygiene** — the foundation everything else writes to.
3. **WS2 durable mood → decisions + steering (SHADOW first)**.
4. **WS4 accumulating ToM** (reads WS1/WS2).
5. **WS3 agency + anti-prior training** — highest reward, highest risk; the
   training lever is the only one that moves the weights.

Non-negotiable discipline (earned the hard way this week):
- **Do not break the line that texts real people.** Every change OFF/SHADOW by
  default; the reactive send path is the hot path; verify with full suite +
  ASan + a real-corpus probe + `doctor` before any deploy.
- **Persistence means new on-disk state** — schema-migrate carefully; a
  corrupt interior is worse than none (see the NUL crashloop).
- **Gate on the human, measure the friction, retire the synthetic judge.**

## Relationship to existing plans (this is the umbrella)

Not a rewrite — the unifying arc over: `seth-aliveness` (A1–A4 wiring),
`intrinsic-motivation` (A3 send tail), `persona-vectors-runtime` (steering =
WS2's projector), `humanness-north-star-metric` (measurement), and the
`egress-single-funnel` cleanup (so a shaped self ships cleanly on every path).
