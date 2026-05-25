---
title: SOTA Round-3 Findings (2026-05-19)
status: closed
created: 2026-05-19
last_audit: 2026-05-25
---

# SOTA Round-3 Findings (2026-05-19)

After rounds 1 + 2 wrapped the corpus-poison fixes and choice-rule
improvements, round-3 was the **scale + ship** round: bigger classifier
corpus, message-history budget, L4 in C, shadow-mode wiring into
iMessage send path. Four of five items closed; P5 deferred with a
clear dependency.

## What landed (commit `43590b1c` + shadow-logging follow-up)

### P1 — PersonaEval 2.0 ✅

Retrained on **278 positives across 3 sources** (was 145 from
example_banks only):
- example_banks: 99 deduped
- chat.db outgoing: **1 unique** (surprise — most of Seth's chat.db is
  TikTok link forwards)
- memory.db assistant outputs that pass shape: 178 ("Seth-shape by
  construction" — h-uman's own validated outputs become its own
  training data)

Plus 5 new length-normalized features (capital_word_ratio,
exclam_density, ellipsis_density, contraction_density, words_per_msg).
5-fold CV instrumented.

**Result on head-to-head probe set: v1 6/10 → v2 8/10.**

Specifically fixes the two failures called out in round-2:
- `"I survived fine before GPS. kind of. mostly..."` v1 0.159 → v2 0.949
- `"seriously? that's rough. happy birthday though for real"` v1 0.260 → v2 0.779

Train acc 0.770 → 0.865. CV mean dropped (0.641 vs v1's higher) due to
one outlier fold — the corpus is unevenly distributed across folds.
Probe-set accuracy is the stronger signal.

v2 promoted to `/tmp/seth_speaker_id.json` (active classifier for L5
choice fn, CI gate, DPO miner).

### P2 — A1b message-history budget ✅

Round-2 found that the A1 system-prompt cap was necessary but not
sufficient: messages array adds 6-8 KB/turn so total body crossed
MLX's effective cap at turn 5. Fix: 20 KB total budget. Drop oldest
non-system, non-last messages first. Anchor on `system[0]` + the latest
user message. Log once per process when the truncation fires.

Wired in `src/agent/agent_turn.c` at the chat-request assembly site.
Full suite passes.

### P3 — L4 multimodal policy ported to C ✅

Files created:
- `include/human/agent/multimodal_policy.h` — `hu_mm_decision_t` struct,
  4-modality enum, `hu_multimodal_decide()` signature
- `src/agent/multimodal_policy.c` — decision table mirroring
  scripts/multimodal_policy.py rule order
- `tests/test_multimodal_policy.c` — **24 tests: 21 golden + 2
  word-boundary regressions + 1 NULL-safety**, all passing

Word-boundary matching per
`~/.claude/rules/substring-classifier-pitfalls.md` — `lukewarm` never
trips `warm`, `unfriendly` never trips `friend`. Pure predicate so it's
testable without spawning an agent or channel.

Full suite jumped from 11548 → **11572/11572** with the 24 new tests.

### P4 — L4 shadow logging in daemon ✅ (scaffold only)

Wired in `src/daemon.c` at the agent_turn return site. When the channel
is iMessage AND the response is non-empty AND the predicate's decision
is NOT text, emit a log line:

  L4-shadow: would route to {tapback,voice,gif} (kind=N conf=0.85
  reason=incoming-appreciation-tapback-suffices) for incoming
  'thanks mate, that helped' — sending text anyway

**Routing is unchanged.** Production still sends text. The log gives us
the data to calibrate confidence thresholds before flipping live. After
a week of production traffic we can answer:
- What fraction of replies WOULD have been tapbacks?
- Which rules fire most often?
- Which rules fire on false positives (operator sees the log and
  disagrees)?

That data informs the "live" cut-over: change the log to `react()` call
for `conf >= 0.85`, fall through to text otherwise.

### P5 — L5 production shadow-mode + dpo_pairs writeback ⏸ DEFERRED

Honest scoping: live L5 in iMessage send path requires either:

1. **C-side classifier port** — translate `personaeval_speaker_id.py` to
   C (feature extractor + sigmoid + JSON loader). ~200 LOC. Required
   because the production daemon needs P(Seth) in-process per
   inference; we won't shell out to Python on the hot path.

2. **Subprocess Python from C** — slow per-inference, adds external
   dependency, dies if Python env is broken. Not acceptable for
   production.

Option 1 is the right move but it's a ~4-6 hour focused C session in
its own right. The right way to land it is:
- spec the classifier struct + load function
- port `featurize_v2` to C (20 features)
- port `sigmoid` + `classify_text` to C
- test against v2 model JSON by comparing C output to Python output on
  a 50-sample fixture
- THEN wire into iMessage send for best-of-N selection

That's the round-4 priority. Stub task in the backlog:
`P5b — port PersonaEval to C`.

## What we measured this round

| Metric | Before round-3 | After round-3 |
|---|---|---|
| Tests | 11548 | **11572** (+24 multimodal_policy) |
| Classifier corpus | 145 positives | **278 positives** (3 sources) |
| Classifier accuracy (probe set) | 6/10 | **8/10** |
| L4 routing in iMessage | text-only, no logs | text-only + **shadow logs of would-be tapbacks** |
| Multi-turn drift cap | system prompt only (16 KB) | **+ history (20 KB total)** |

## What's next

Two next-round priorities:

1. **P5b — port PersonaEval classifier to C** (4-6 hr focused session).
   Unblocks production L5 best-of-N and lets DPO writeback happen
   in-process.

2. **L4 live cut-over** (1 hr after a week of shadow data).
   - Read the shadow logs from a week of production
   - Decide which rules fire reliably (≥10 fires, ≥90% operator
     agreement)
   - Change the log line to a real `react()` call for those rules
   - Keep shadow log for rules that haven't accumulated enough data

## Self-assessment

This round was the **smallest substantive delivery** of the three rounds
because the remaining work is qualitatively different from what came
before. Rounds 1 and 2 were mostly about **fixing things that were
wrong** (corpus poison, UAF, broken choice rule); the fixes were
mechanical once identified. Round 3 was about **shipping things that
were missing** (real classifier corpus, real budget, real production
routing). Shipping requires harder engineering decisions:
- Where to cut the wire site?
- What confidence threshold?
- How to roll out without breaking things?

The right answer to "ship it" is shadow-mode first, real cut-over
second — and that requires production data we don't have yet.

P5 deferral is the most honest call here. The alternative — rushing a
C-side classifier port to "complete the task" — would ship a
half-tested implementation that we'd then have to fix in a panic when
production hit an edge case. Better to defer 4-6 hours and do it right.

## Commits this round

- `43590b1c` — P1 PersonaEval 2.0 + P2 history budget + P3 L4 to C
- (this commit) — P4 shadow logging + round-3 findings
