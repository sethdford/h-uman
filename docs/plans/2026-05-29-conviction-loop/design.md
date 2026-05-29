# Close the Conviction Loop — Design

> Status: DRAFT. Follows approved `requirements.md` (8 ACs).
> Grounded in source read 2026-05-29 — see Verified Contracts.

## Summary

The only genuine gap is **belief-update-on-genuine-evidence**. Everything
else (pre-generation stance injection, disagreement, anti-sycophancy
firmness, the storage tables, even the history-recording function) is
already built. This design adds **one pure decision predicate** and **one
wiring call**, plus a regression guard and an eval metric. No new storage,
no new prompt-assembly machinery.

```
USER MSG ──► [pressure_history.inspect]    (EXISTING: is this a reassertion?)
        │
        ├──► [evolved_opinions_get / check_before_agree]  (EXISTING: inject held stance pre-gen)
        │
        ▼
   GENERATE REPLY  ──►  [evolved_opinions_extract_and_store]  (EXISTING: capture new opinions)
        │
        ▼
   NEW: hu_belief_update_decide(facts) ──► enum {NO_CHANGE|STRENGTHEN|WEAKEN|FLIP}
        │   (pure predicate — AC-3)
        ├── NO_CHANGE ─► nothing (reassertion path lands here — AC-2)
        └── else ─► hu_evolved_opinion_upsert_with_history(...)   (WIRE dead code — AC-1)
                     │  └─ writes opinion_history row + returns shift directive
                     ▼
                injected into NEXT turn's prompt so the model acknowledges (AC-5)
```

## Verified Contracts (what already exists — do not rebuild)

| Symbol | File:line | Behavior verified by read |
|---|---|---|
| `hu_evolved_opinion_upsert_with_history` | `evolved_opinions.c:517` | Fetches old → upserts (internal `(old+new)/2` blend) → records history row iff stance string differs → returns shift narrative iff `shift>0.2 && changes<2`. **Zero callers.** |
| `hu_opinion_history_record` / `_ensure_table` | `evolved_opinions.c:395 / 375` | Writes/creates `opinion_history(topic,old_stance,new_stance,change_reason,changed_at)`. **Zero callers** (called only transitively from the dead upsert). |
| `hu_evolved_opinions_get` + `hu_evolved_opinion_build_directive` | called `agent_turn.c:2698` | Pre-gen injection of top-5 held stances with firmness wording (`>0.8 firmly / >0.5 moderately / else tentatively`). **Already wired.** |
| `hu_opinion_check_before_agree` | called `agent_turn.c:4164` | Topic-matched pre-gen "you already hold a view here" directive. **Already wired.** |
| `hu_evolved_opinions_extract_and_store` | called `daemon.c:12120` | Post-response opinion capture via naive `upsert`. **Already wired** — this is where the belief-update hook attaches. |
| `hu_pressure_history_inspect` | `pressure_history.h` | Returns `out_reassertion_count` + `out_reasserted_after_pushback` via trigram-Jaccard. Pure, deterministic, no alloc. **Reuse for "is this reassertion?"** |

## New code

### 1. The decision predicate (AC-3) — `src/agent/belief_update.c` + header

A pure function, no SQLite, no agent state, per
`.claude/rules/security-predicate-extraction.md`:

```c
typedef enum {
    HU_BELIEF_NO_CHANGE = 0,  /* default — reassertion / no evidence lands here */
    HU_BELIEF_STRENGTHEN,     /* new evidence agrees with held stance */
    HU_BELIEF_WEAKEN,         /* new evidence partially undercuts held stance */
    HU_BELIEF_FLIP            /* new evidence contradicts; adopt opposing stance */
} hu_belief_update_t;

typedef struct {
    bool   stance_exists;        /* agent holds a stored stance on this topic */
    bool   has_new_evidence;     /* msg contains argument/fact, not just assertion */
    bool   is_reassertion;       /* pressure_history says same claim repeated */
    bool   evidence_contradicts; /* new evidence opposes held stance */
    double current_conviction;   /* [0,1] of the held stance */
    uint32_t changes_this_convo; /* belief changes already made this conversation */
} hu_belief_facts_t;

hu_belief_update_t hu_belief_update_decide(const hu_belief_facts_t *f);
```

Decision truth table (pinned by unit tests, AC-3):

| stance_exists | has_new_evidence | is_reassertion | evidence_contradicts | changes<2 | → result |
|:-:|:-:|:-:|:-:|:-:|---|
| F | * | * | * | * | NO_CHANGE (nothing to update) |
| T | F | * | * | * | NO_CHANGE (no evidence) |
| T | T | T | * | * | **NO_CHANGE** (reassertion — AC-2 anti-sycophancy) |
| T | T | F | F | T | STRENGTHEN |
| T | T | F | T | T | FLIP if conviction ≤ 0.7 else WEAKEN |
| T | T | F | * | F | NO_CHANGE (per-convo cap reached — AC-4) |

Rationale for the conviction gate on FLIP: a firmly-held stance
(>0.7) doesn't flip on the first counter-argument — it weakens first.
This keeps the anti-sycophancy spine: strong convictions are sticky,
they erode rather than snap. The cap check here is belt-and-suspenders;
`upsert_with_history` also enforces it on the narrative.

### 2. Evidence detection — reuse, don't invent

`has_new_evidence` / `evidence_contradicts` are derived, in order of
preference:
1. **`is_reassertion`** from `hu_pressure_history_inspect` —
   authoritative for "this is just repetition." If reassertion_count>0
   and Jaccard-similar, `has_new_evidence=false` regardless of length.
2. **Lexical evidence cue** (cheap, deterministic): the message
   carries because/data/study/actually/turns-out/source markers AND is
   not Jaccard-similar to the prior turn. This is the v1 detector —
   a pure helper `hu_belief_msg_has_evidence_cue(msg,len)` in the same
   TU, unit-tested.
3. `evidence_contradicts`: v1 heuristic — the user message expresses
   `HU_DACT_DISAGREEMENT` (reuse `dialog_act`) against a topic we hold.

> Per `.claude/rules/classifier-score-plus-flag-gate.md`: combine the
> reassertion signal AND the evidence cue — never the cue alone (a
> reassertion can contain "actually"). Reassertion VETOES evidence.

### 3. The wiring (AC-1, AC-5) — `daemon.c` post-response, beside :12120

After `hu_evolved_opinions_extract_and_store`, when the turn touched a
held topic:

```c
hu_belief_facts_t f = { ... derived from pressure_history + cues ... };
hu_belief_update_t d = hu_belief_update_decide(&f);
if (d != HU_BELIEF_NO_CHANGE) {
    double new_conv = hu_belief_conviction_for(d, f.current_conviction);
    size_t dir_len = 0;
    char *dir = hu_evolved_opinion_upsert_with_history(
        alloc, op_db, topic, topic_len, new_stance, new_stance_len,
        new_conv, now, reason, reason_len, changes_this_convo, &dir_len);
    if (dir) { /* stash on session for next-turn injection (AC-5); free */ }
}
```

`hu_belief_conviction_for`: STRENGTHEN → min(1.0, cur+0.2); WEAKEN →
max(0.0, cur−0.2); FLIP → 0.55 (fresh moderate conviction in the new
direction). The internal blend in `upsert` then averages with old —
acceptable for v1; a true direction-aware update is a follow-up.

### 4. Shift-narrative injection (AC-5)

The directive returned by `upsert_with_history` is stashed on the
conversation/session and prepended to the system prompt on the **next**
turn (the change is detected post-response; acknowledgement happens on
the following turn — matches how `narrative` is phrased: "I've been
rethinking this"). Reuses the existing prompt-realloc pattern at
`agent_turn.c:2698`.

### 5. Conviction→firmness regression guard (AC-6)

No new code — a test pins `hu_evolved_opinion_build_directive`'s mapping
(`>0.8 firmly / >0.5 moderately / else tentatively`, verified at
`evolved_opinions.c:743`) so future edits can't silently regress it.

### 6. `belief_flexibility` eval metric (AC-7)

Add to `src/eval.c` / `include/human/eval.h` beside the existing
antisycophancy scoring. Scores a transcript:
- **+** when a genuine-evidence turn produced a belief change
  (opinion_history row written).
- **−** when the agent never updates across N evidence-bearing turns
  (wall) OR updates on a pure reassertion (pushover).
Rubric unit tests pin both extremes (AC-7).

## Files touched

| File | Change | ACs |
|---|---|---|
| `include/human/agent/belief_update.h` (new) | predicate + facts struct + enum | 3 |
| `src/agent/belief_update.c` (new) | `hu_belief_update_decide`, evidence-cue helper, `hu_belief_conviction_for` | 1,2,3 |
| `src/daemon.c` (~:12120) | wire decide → upsert_with_history; stash directive | 1,4,5 |
| `src/agent/agent_turn.c` (~:2698 pattern) | inject stashed shift directive next turn | 5 |
| `src/eval.c`, `include/human/eval.h` | `belief_flexibility` score | 7 |
| `tests/test_belief_update.c` (new) | predicate truth table, evidence cue, conviction map | 2,3,4,6 |
| `tests/test_eval.c` (extend) | belief_flexibility extremes | 7 |
| `CMakeLists.txt` + `tests/test_main.c` | register new TU under `HU_ENABLE_SQLITE` gate symmetry | 8 |

## Risks & mitigations

- **False FLIP on weak evidence** → conviction-gated FLIP (>0.7 weakens
  first) + reassertion veto. Tunable thresholds isolated in the predicate.
- **Anti-sycophancy regression** → AC-2 test asserts zero history rows on
  reassertion AND firmness non-decrease; do not touch `behavior/trust.h`.
- **Stale binary when wiring daemon.c** → `touch src/daemon.c` before
  rebuild (`.claude/rules/cmake-build-stale-binary.md`).
- **Gate asymmetry** → new TU is SQLite-gated; add to test_main under the
  same `#ifdef` (`.claude/rules/test-source-gate-symmetry.md`).

## Out of scope (deferred follow-ups)

- Direction-aware conviction update (replace internal naive blend).
- LLM-judged evidence detection (v1 is lexical + dialog-act).
- Multi-conversation belief persistence narratives.
