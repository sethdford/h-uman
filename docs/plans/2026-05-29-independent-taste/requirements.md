# A Self That Isn't You — Independent Taste (A2)

> Status: DRAFT — requirements only, awaiting sign-off before design.
> Date: 2026-05-29. The Tier-1 believability lever. Effort: L (multi-sprint).

## Problem

h-uman's entire "self" is currently **derived from Seth**. Verified:
`src/persona/style_clone.c`, `style_mirror.c`, `style_learner.c` exist to
*re-analyze Seth's conversations and update the persona to match him*
("Style learning loop: re-analyze conversations to update persona").
`src/agent/preferences.c` extracts and stores **Seth's** preferences
(`hu_preferences_is_correction`, `hu_preferences_extract` operate on the
user message). `src/agent/self_model.c` is a Phase-A scaffold, not a
preference-bearing self.

The conviction loop (A1) lets h-uman hold *opinions on topics*. This epic
is the deeper thing: **aesthetic and factual preferences of its own** —
likes, dislikes, a sense of taste — that it did **not** mirror from Seth
and that it will maintain even when Seth doesn't share them. A person you
respect has their own taste; an agent that only ever reflects you back is a
mirror, not a partner. This is the single biggest "convincingly human"
lever identified in the 2026-05-29 audit, and the hardest.

## User stories

- As Seth, I want h-uman to have **its own likes and dislikes** (music it
  gravitates to, a writing style it prefers, topics it finds dull or
  fascinating) that are **stable and not copied from me**, so it reads as
  a distinct person rather than a reflection of me.
- As Seth, I want those preferences to **show up naturally** in how it
  talks and what it brings up — not announced as "my preference is X," but
  expressed the way a person's taste leaks into their conversation.
- As Seth, I want it to **keep a preference even when I disagree** (within
  reason), so it doesn't collapse into agreement — while still being
  genuinely persuadable on *topics* via A1.
- As Seth, I want its taste to **develop slowly and coherently over time**
  (not random per turn, not whiplashing), so it feels like a real
  personality maturing, consistent with the existing `voice_maturity.c`.
- As the project, I want this to be **honest, not deceptive** — h-uman
  expresses taste while remaining truthful that it is an AI; preferences
  are a behavioral layer, not a claim of sentience.

## Acceptance criteria

- [ ] **AC-1 (independent preference store):** A persisted set of the
  agent's OWN preferences exists, distinct from `preferences.c` (which is
  Seth's) and from `evolved_opinions` (which is topic-stances). Each entry:
  domain (e.g. music/writing/food/topic), valence (like/dislike/neutral),
  strength [0,1], origin marker, formed_at. Pinned by storage round-trip
  tests.
- [ ] **AC-2 (not mirrored from Seth):** Preferences are **seeded
  independently** (a starter taste profile) and updated by the agent's own
  experience, NOT by `style_learner`/`style_mirror` copying Seth. A test
  asserts that running the Seth-style-learning path does NOT write into the
  agent's own-preference store (the two stores are isolated).
- [ ] **AC-3 (natural expression):** When relevant, a held preference
  modulates tone/word-choice/topic-selection in the turn — injected as a
  behavioral directive, not a literal "I prefer X" announcement. Pinned by
  a test asserting the directive is present and is framed as leaked taste,
  not a pronouncement (reuse the `evolved_opinion_build_directive` framing
  discipline).
- [ ] **AC-4 (stable under disagreement):** A held preference is **not
  dropped** simply because Seth expresses a different taste (distinct from
  A1 topic-opinions, which CAN update on evidence — taste is not a factual
  claim and doesn't yield to "evidence"). Pinned by a test: Seth states
  opposing taste → agent's preference strength does not collapse.
- [ ] **AC-5 (slow coherent drift):** Preference changes are
  rate-limited and direction-coherent over time (no per-turn whiplash),
  consistent with `voice_maturity.c`. Pinned by a test simulating many
  turns and asserting bounded drift.
- [ ] **AC-6 (honesty guardrail):** Expressing taste never asserts
  sentience or deceives. A test/contract pins that the taste directive
  contains no "I feel/I'm conscious"-class claims and remains compatible
  with the AI-disclosure standard (`docs/standards/ai/`).
- [ ] **AC-7 (eval metric):** A `distinctiveness` (or `taste_coherence`)
  eval score added beside existing persona metrics: higher when the agent
  expresses stable own-taste distinct from Seth, lower when it merely
  mirrors. Rubric unit tests for both extremes.
- [ ] **AC-8 (build/quality gate):** `-Wall -Wextra -Wpedantic -Werror`,
  SQLite-gated with test/source gate symmetry, all allocations freed (0
  ASan), full suite green.

## Non-goals

- Topic opinions / belief change — that's A1 (conviction loop). Taste is
  preferences, not arguable factual stances.
- Intrinsic motivation / goals-of-its-own — that's A3.
- Emotions (shame/pride/etc.) — separate backlog items.
- Reusing/extending `style_clone`/`style_mirror` — those stay Seth-facing;
  this is deliberately a *separate* store.
- Letting taste override safety, accuracy, or the user's explicit
  instructions — taste is expression, never a reason to be unhelpful.

## Constraints

- C11; `-Wall -Wextra -Wpedantic -Werror`; free all allocations; SQLite
  `SQLITE_STATIC`, `#ifdef HU_ENABLE_SQLITE` + gate symmetry.
- Preference-expression decision MUST be an extracted pure predicate
  (`.claude/rules/security-predicate-extraction.md`).
- MUST NOT couple to or write through the Seth-facing
  `preferences.c`/`style_*` paths (AC-2 isolation).
- Honesty/disclosure: read `docs/standards/ai/` before design; taste is a
  behavioral layer, not a sentience claim (AC-6).
- Deterministic tests; no network/spawning (`HU_IS_TEST`).
- Ethical review gate before implementation: an agent with persistent
  independent preferences that diverge from the user must be designed to
  serve the user, never to manipulate. Design phase must address this
  explicitly.

## Provenance (audited 2026-05-29)

Three parallel read-only audits confirmed: the persona "self" is
clone-of-Seth (`style_clone.c`/`style_mirror.c`/`style_learner.c`),
`preferences.c` is Seth's preferences not the agent's, `self_model.c` is a
scaffold. No store of the agent's OWN likes/dislikes exists →
PRESENT-as-mirror, ABSENT-as-independent-self. This epic adds the
independent self. Recommend `/spec` then `/team` (multi-story L epic);
design must include the ethical/honesty review (AC-6, constraints).
