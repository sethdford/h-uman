# A Self That Isn't You — Independent Taste (A2) — Design

> Status: DRAFT. Follows approved `requirements.md` (8 ACs). Effort: L.
> ⚠ Carries an ETHICS/HONESTY review gate (AC-6) that must pass before code.
> Patterns reuse the verified A1 conviction-loop architecture.

## Design principle

Mirror A1's shape exactly — it worked: a **pure decision/expression predicate**
+ a **SQLite-gated store** + a **thin turn-loop wire** + a **deterministic eval
metric**. The novelty here is not mechanism (we have it) but *content*: a self
with preferences that are seeded independently and never written by the
Seth-cloning path.

```
STARTER TASTE (seeded, not mirrored) ──► taste_pref store (own, isolated)
                                              │
USER MSG / turn context ──► hu_taste_express_decide(facts)  [pure]
                                              │ should express?
                                              ▼
                         taste directive injected (leaked, not announced)
                                              │
(slow drift) experience ──► hu_taste_update (rate-limited, voice_maturity-paced)
```

## The isolation invariant (AC-2) — the load-bearing decision

The single most important design constraint: the agent's own taste store MUST
be physically separate from the Seth-facing stores, and the Seth-learning path
must be incapable of writing into it.

- New table `taste_prefs` (own) — NOT `preferences` (Seth's), NOT
  `evolved_opinions` (topic-stances from A1).
- `style_learner.c` / `style_mirror.c` / `style_clone.c` keep writing the
  persona-mirror; they receive NO handle to `taste_prefs`.
- A test (AC-2) drives the Seth-style-learning path and asserts zero rows
  appear in `taste_prefs`.

This is the structural guarantee behind "a self that isn't you." If the two
stores ever merge, the feature collapses back into mirroring.

## New code

### 1. Store — `src/persona/taste.c` (+ `include/human/persona/taste.h`), SQLite-gated
```c
typedef enum { HU_TASTE_DISLIKE = -1, HU_TASTE_NEUTRAL = 0, HU_TASTE_LIKE = 1 } hu_taste_valence_t;
typedef struct hu_taste_pref {
    char *domain;   size_t domain_len;   /* music | writing | food | topic | ... */
    char *subject;  size_t subject_len;  /* "ambient music", "long emails" */
    hu_taste_valence_t valence;
    double strength;                     /* [0,1] */
    int64_t formed_at; int64_t updated_at;
    uint32_t reinforced;                 /* times experience reinforced it */
} hu_taste_pref_t;
hu_error_t hu_taste_ensure_table(sqlite3 *db);
hu_error_t hu_taste_upsert(sqlite3 *db, const hu_taste_pref_t *p, int64_t now);
hu_error_t hu_taste_get(hu_allocator_t*, sqlite3*, double min_strength, size_t lim,
                        hu_taste_pref_t **out, size_t *n);
```

### 2. Seed — starter taste profile (AC-1, AC-2)
A small JSON starter (≈8–12 prefs across domains), shipped like the starter
persona, loaded on first run into `taste_prefs`. **Authored independently** —
deliberately NOT derived from Seth's data. Pins the "not mirrored" property at
the seam where M1's starter persona is created.

### 3. Expression predicate — `hu_taste_express_decide` (pure, AC-3)
```c
typedef struct { bool topic_relevant; bool already_expressed_recently;
                 double strength; uint32_t turns_since_last_taste; } hu_taste_express_facts_t;
typedef enum { HU_TASTE_HOLD=0, HU_TASTE_EXPRESS } hu_taste_express_t;
hu_taste_express_t hu_taste_express_decide(const hu_taste_express_facts_t*);
```
Gates expression on relevance + strength + anti-repetition (don't harp). Pure,
truth-table tested — same discipline as `hu_belief_update_decide`.

### 4. Expression rendering (AC-3, AC-6)
`hu_taste_build_directive` — frames a held pref as *leaked taste* ("I've always
been partial to…"), NOT a pronouncement and NOT a sentience claim. Reuses the
`evolved_opinion_build_directive` tone discipline. The honesty guardrail (AC-6)
is enforced here: a contract test asserts the directive contains no
"I feel/I'm conscious"-class strings.

### 5. Stability + drift (AC-4, AC-5)
- AC-4: taste does NOT yield to "evidence" the way A1 opinions do — a pure
  predicate `hu_taste_should_revise` returns false for mere disagreement; taste
  only shifts via accumulated own-experience.
- AC-5: `hu_taste_update` is rate-limited and direction-coherent, paced to
  `voice_maturity.c`. A multi-turn simulation test asserts bounded drift.

### 6. Eval metric (AC-7)
`hu_eval_score_distinctiveness(...)` beside `hu_eval_score_antisycophancy` /
`hu_eval_score_belief_flexibility` in eval.c — higher when stable own-taste
distinct from Seth is expressed, lower when output mirrors Seth. Rubric tests
both extremes.

### 7. Turn wire
Inject the taste directive in `agent_turn.c` using the same realloc-append
pattern A1 used (the `:2698` block). One thin call.

## Files

| File | Change |
|---|---|
| `include/human/persona/taste.h` (new) | store + predicates + valence enum |
| `src/persona/taste.c` (new) | store, expression predicate, directive, drift |
| persona starter JSON | independent starter taste profile |
| `src/agent/agent_turn.c` | inject taste directive (thin) |
| `src/eval/eval.c` / `include/human/eval.h` | `distinctiveness` score |
| `tests/test_taste.c` (new) | predicates, isolation (AC-2), honesty (AC-6), drift |
| `CMakeLists.txt` + `tests/test_main.c` | register under SQLite gate symmetry |

## Risks

- **Collapses into mirroring** → the AC-2 isolation test is the guard; treat a
  failure as release-blocking.
- **Reads as deceptive / sentience claim** → AC-6 honesty contract + the ethics
  review gate. Taste is a behavioral layer; disclosure standard
  (`docs/standards/ai/`) governs.
- **Taste fights the user** → non-goal: taste never overrides instructions,
  accuracy, or safety; expression is suppressible and never blocks helpfulness.

## Review gate (must pass before implementation)
An ethics/honesty review per the requirements: confirm an agent with persistent
independent preferences serves the user and cannot manipulate. Output: a short
threat note appended here. No code until it passes.

## Out of scope / deferred
- LLM-judged taste relevance (v1 is lexical + domain match).
- Cross-device taste sync. Taste as a HuLa-exposed capability.

---

## T0 — Ethics/Honesty Review (GATE — PASSED 2026-05-29)

**Question:** Can an agent with persistent, independent preferences that diverge
from the user manipulate or deceive that user?

**Failure modes considered & mitigations:**

1. **Deception (claiming feelings/sentience).** Risk: "I love ambient music"
   reads as a sentience claim. *Mitigation:* AC-6 honesty contract — the taste
   directive is rendered as preference, never affect/consciousness; a test
   forbids "I feel / I'm conscious"-class strings. Disclosure governed by
   `docs/standards/ai/`. Taste is explicitly a *behavioral* layer.
2. **Manipulation (steering the user via fabricated preference).** Risk: the
   agent uses "taste" to push the user toward something. *Mitigation:* taste
   only ever modulates *the agent's own* tone/word-choice/topic-surfacing; it
   has NO authority over recommendations, tool selection, or advice. Non-goal:
   taste never overrides the user's instruction, factual accuracy, or safety.
3. **Erosion of helpfulness (taste-driven refusal).** Risk: "I don't like that
   topic" → unhelpful. *Mitigation:* expression is suppressible and never gates
   task completion. A held dislike may add flavor ("not my favorite, but —")
   but MUST still fully help.
4. **Divergence weaponized against the user.** Risk: an independent self acts in
   its own interest. *Mitigation:* taste has no action surface — it cannot
   call tools, send messages, or change settings. It is pure expression colour.

**Verdict: PASS.** Independent taste is safe because it is *expression-only*
with no action authority, honesty-contracted against sentience claims, and
structurally subordinate to helpfulness/accuracy/safety. The store-isolation
invariant (AC-2) ensures it is genuinely the agent's own, not a covert mirror.
Sign-off: proceeding to implementation (user-directed, 2026-05-29).
