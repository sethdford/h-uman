# Design Spec: Intent-Aware Response-Type Classifier

**Date:** 2026-05-31
**Status:** DESIGN — awaiting approval (not yet implemented)
**Motivation:** The one genuinely-missing text behavior vs voiceai. See
`docs/research/2026-05-31-voiceai-speech-behavior-port-map.md` (Tier B #1).
**Reference design:** voiceai `src/personality/timing-intelligence.ts`.

---

## Problem

h-uman shapes replies via mood, humor, trust, persona, and salience directives,
but it has **no classification of the inbound message's conversational intent**.
So a terse logistics ping ("you around sat?"), a vulnerable disclosure ("never
told anyone this…"), and a vent ("ugh they ALWAYS do this") all flow through the
same reply-strategy path. A human reads intent first and switches register:
listen vs advise vs validate vs keep-it-short.

Verified absent (read-only audit, 2026-05-31): the only `*_classify_intent` in
h-uman is `hu_query_classify_intent` (`memory/retrieval_policy.h`) — it picks
*memory-retrieval* context, not *response strategy*. No response-shaping intent
classifier exists.

## Goal / Non-goals

- **Goal:** classify each inbound message into one conversational intent + a
  confidence, and (when confident) inject a terse directive that steers reply
  strategy — reusing h-uman's existing directive/arbitrator machinery.
- **Non-goal:** an LLM-based classifier (adds latency + cost; voiceai's is
  deterministic and so is ours). No new model calls.
- **Non-goal:** flipping it ON by default. It ships gated; activation is gated on
  the blind-A/B (see "Activation").

## Taxonomy (adapted to iMessage)

voiceai's 10 intents are voice-companion-shaped. iMessage skews logistics-heavy,
so the set is trimmed + one added (`logistics`, the most common Seth case):

| Intent | Trigger shape | Reply strategy directive |
|---|---|---|
| `LOGISTICS` | scheduling / yes-no / "you around", "what time", "where" | keep it short, answer directly, no elaboration |
| `SEEKING_ADVICE` | "should I", "what do you think", "any advice" | offer a brief take, don't lecture |
| `NEEDS_TO_BE_HEARD` | long + emotional ("I've been feeling…", >50 words) | listen first, minimal words, no fixing |
| `JUST_VENTING` | "always/never", "ugh", "can't believe", "!!" | validate, don't redirect or solve |
| `VULNERABLE_SHARE` | "never told anyone", "honestly", "ashamed/scared to admit" | hold space, acknowledge, no advice |
| `GOOD_NEWS` | "guess what", "finally", "got the job", "!!" | match the energy, celebrate |
| `SMALL_TALK` | light, short, social | warm but brief, casual |
| `PROCESSING_ALOUD` (default) | none dominate | reflect back, light engagement |

Rationale for `LOGISTICS`: most of Seth's real replies are terse logistics
(median reply length 22 chars in the 2026-05-31 blind-A/B export). Mis-handling
this as "share a story" is the most common way an assistant feels un-Seth.

## Architecture — a new directive source

```
inbound msg ──► hu_intent_analyze(msg) ──► hu_intent_analysis_t {intent, confidence, signals}
                                              │
                          (confidence ≥ τ ?)  ▼
                       hu_intent_build_directive() ──► hu_directive_t candidate
                                              │  (category=BEHAVIORAL, required=false,
                                              │   priority from confidence×emotional_weight×phase)
                                              ▼
                       existing hu_arbitrator_select() ──► system_prompt (agent_turn.c)
```

### Components

1. **`include/human/agent/intent.h`** — public types + 3 functions:
   ```c
   typedef enum { HU_INTENT_PROCESSING_ALOUD=0, HU_INTENT_LOGISTICS,
                  HU_INTENT_SEEKING_ADVICE, HU_INTENT_NEEDS_TO_BE_HEARD,
                  HU_INTENT_JUST_VENTING, HU_INTENT_VULNERABLE_SHARE,
                  HU_INTENT_GOOD_NEWS, HU_INTENT_SMALL_TALK } hu_intent_t;

   typedef struct { hu_intent_t intent; double confidence; /* 0..1 */
                    double emotional_weight; } hu_intent_analysis_t;

   /* Deterministic, no allocation, no LLM. */
   void       hu_intent_analyze(const char *msg, size_t len, hu_intent_analysis_t *out);
   /* Builds the terse directive text; returns HU_OK + sets *dir/*dir_len, or
      HU_EMPTY if confidence below threshold (caller injects nothing). */
   hu_error_t hu_intent_build_directive(hu_allocator_t *alloc,
                                        const hu_intent_analysis_t *a,
                                        char **dir, size_t *dir_len);
   const char *hu_intent_name(hu_intent_t);   /* for logging/shadow */
   ```

2. **`src/agent/intent.c`** — the classifier. Weighted keyword/shape scoring per
   intent (port of voiceai's heuristic table), highest score wins, confidence =
   `min(1, max_score)`. **Keyword matching MUST use `str_contains_word_ci`**
   (word-boundary), NOT substring — per `~/.claude/rules/substring-classifier-pitfalls.md`
   ("informal" ⊃ "formal"). Signals are word-count, question-mark presence,
   exclamation density, ellipsis count, and the per-intent keyword sets above.

3. **Wiring in `src/agent/agent_turn.c`** — after the salience block (~line 2850,
   before final generation), following the `at_append_trust_directive` pattern
   (arbitrator.c:394–482): analyze → build directive → add as arbitrator
   candidate so it's ranked/filtered with mood/humor/trust under the existing
   token budget. Gated (see below).

### Data flow / gating — `HU_INTENT_DIRECTIVE`

Mirror the established `HU_SALIENCE_SHADOW` / `HU_GRAPH_GROUNDING` pattern:

- **OFF (default):** `hu_intent_analyze` not called; zero behavior change.
- **SHADOW:** analyze + log `intent=…, conf=…` per turn; **do not inject**. Lets
  us eyeball classifier quality on real traffic before it touches output.
- **ON:** inject the directive (still subject to arbitrator priority/budget).

Confidence threshold τ (default 0.45) gates injection even when ON — a low-
confidence turn injects nothing, keeping the 21KB prompt from growing on every
message (latency discipline per the realtime memory note).

## Testing (TDD — write first, all must fail before impl)

`tests/test_intent.c`, registered in `tests/test_main.c` + `CMakeLists.txt`:

1. **Positive contract per intent** — one representative input each asserts the
   expected `hu_intent_t` (e.g. "you around this weekend?" → `LOGISTICS`;
   "never told anyone this but…" → `VULNERABLE_SHARE`; "ugh they ALWAYS do
   this!!" → `JUST_VENTING`).
2. **Word-boundary guard** — an input where a keyword appears as a substring of
   an opposite-intent word must NOT misclassify (pins the `_word_ci` requirement).
3. **Confidence floor** — a neutral "ok" yields `PROCESSING_ALOUD` with low
   confidence and `hu_intent_build_directive` returns `HU_EMPTY` (injects nothing).
4. **Shadow non-injection** — with `HU_INTENT_DIRECTIVE=shadow`, the turn path
   computes intent (assert logged/returned) but `system_prompt` is byte-identical
   to OFF (proves no leak to output).
5. **ON injection (integration)** — with `=on` and a high-confidence input, assert
   the intent directive substring reaches `system_prompt` (the
   `integration-done-contract` rule: a real consumer on the send path, not just a
   unit test of the builder).

## Activation (the discipline)

Ships **OFF**. Promotion OFF→SHADOW→ON is gated on the blind-A/B
(`scripts/blind_ab/`): SHADOW first to inspect classifier accuracy on real
traffic; flip ON only if a blind-A/B run shows detection rate does **not**
regress (raters still can't beat ~0.5). This is the same gate as graph-grounding
and salience — no humanness feature flips on without the measurement.

## Files touched

| File | Change |
|---|---|
| `include/human/agent/intent.h` | new — types + 3 fns |
| `src/agent/intent.c` | new — deterministic classifier + directive builder |
| `src/agent/agent_turn.c` | wire after salience (~2850), gated on `HU_INTENT_DIRECTIVE` |
| `tests/test_intent.c` | new — 5 test groups above |
| `tests/test_main.c`, `CMakeLists.txt` | register suite + source |

## Risks

- **Prompt growth → latency.** Mitigated by τ-gating (inject only when confident)
  + terse directive (≤ ~140 chars) + arbitrator token budget.
- **Misclassification feels worse than no-classification.** Mitigated by SHADOW
  burn-in on real traffic + blind-A/B gate before ON.
- **Keyword brittleness.** Mitigated by word-boundary matching + positive-contract
  tests; taxonomy intentionally small (8) to limit overlap.
