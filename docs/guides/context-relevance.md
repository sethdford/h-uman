---
title: Context relevance — HU_CONTEXT_RELEVANCE gate, threshold and promotion
created: 2026-10-02
status: operator-facing
---

# Context relevance (`HU_CONTEXT_RELEVANCE`)

Short messages used to get no memory at all. Two cliffs decided by length or
tier, not by what memory held:

| Cliff | Where | Prod evidence (2026-09-19 → 10-02 log) |
|---|---|---|
| Semantic recall dropped when the inbound has ≤ 12 words | `src/memory/retrieval/hybrid.c`, `HU_SEMANTIC_RECALL_REGISTER_GATE=live` | 85 suppressions (395 hits dropped) vs 38 live recalls |
| Graph grounding dropped below the ANALYTICAL tier | `src/agent/graph_grounding.c`, `HU_GRAPH_GROUNDING=on` | 165 of 172 live turns dropped |

Most texting is short, so the twin forgot exactly while chatting. This gate
replaces both cliffs with one relevance decision
(`src/memory/context_relevance.c`).

## Semantics

| Mode | Effect |
|---|---|
| `off` (default) | Byte-identical to before: both cliffs apply, nothing is sampled. |
| `shadow` | Retrieval and the cliffs run as before. One aggregate line per turn and source says what relevance would inject, plus the calibration sample. Nothing sent changes. |
| `live` | On **casual** turns only (the turns the cliffs starved), an item is injected when its relevance clears the threshold, within a small byte budget instead of zero. **Substantive turns keep today's recall unchanged** until the threshold is calibrated. |

Relevance per source:

- **semantic** — the retrieval scorer's cosine score per hit (query-to-document,
  `embed_query`). Casual turns (the old ≤ 12-word classifier, which now only
  picks the turn class) get `$HU_CONTEXT_RELEVANCE_CASUAL_BYTES`, default two
  recall hits (`2 * HU_SEMANTIC_RECALL_HIT_MAX_BYTES` = 480 bytes). Only
  applies under `HU_SEMANTIC_RECALL=live`.
- **graph** — strict query-conditioning: an entity counts only when **every**
  scoreable word of its name is in the message (`HU_GG_REQUIRE_FULL_NAME`)
  and it is not an EMOTION (`HU_GG_NO_EMOTION_SEED`). "love you" does not
  ground on the contact's `love`, "at work rn" not on `work trip`. The kept
  block is that strict composition, cut to the casual budget at a line
  boundary. The contact fallback is not conditioned on the message, so it
  stays dropped. Only applies under `HU_GRAPH_GROUNDING=on`; prod runs
  grounding in `shadow` (2026-09-30), so in prod today this gate changes
  semantic recall only. A single-word entity whose whole name a casual text
  contains (a TOPIC literally named "work") still qualifies.

## The threshold: a null distribution, not a guess

The live score is query-to-document; a document-to-document number is the
wrong distribution. So every non-OFF semantic decision also scores the
message's own embedding against `HU_CONTEXT_RELEVANCE_NULL_K` (32) random
stored vectors (`hu_vector_store_sqlite_vec_sample_scores`, dot products only,
no text): what an **unrelated** memory scores against this kind of query.
Those scores go into a rolling in-process pool of 1,024. The threshold is, in
order:

1. `$HU_CONTEXT_RELEVANCE_MIN_SCORE`, when set (config after review);
2. the pool's **p95**, once it holds at least 128 scores (four decisions);
3. `0.46`, the cold-start fallback only (the median doc-to-doc cosine of the
   live index, `python3 scripts/context_relevance_floor.py`, 2026-10-02:
   72 vectors, p50 0.458).

A hit is relevant when it scores above 95% of random memories against the
same kind of message. The pool restarts empty with the process, so the first
few decisions after a restart use the cold start; `threshold_src` in the log
says which applied.

Every decision logs one line, counts and scores only:

```
[context_relevance shadow] src=semantic casual=1 applied=0 items_considered=5 items_passing=2
    bytes_would_inject=311 top_score=0.712 min_score=0.598 threshold_src=null null_k=32
    null_p95=0.601 null_pool=416 top=0.70,0.55,0.50 hist=0/0/0/.../1/2/1/0/0
```

`top` is the best five real scores at 0.05 resolution; `hist` counts all real
scores in 0.05-wide bands over [0, 1]. Together with `null_p95` they are what a
per-contact or learned threshold gets fitted on. The graph line carries
`matched_full_name` instead.

## Promotion: SHADOW → LIVE

1. Deploy with `HU_CONTEXT_RELEVANCE=shadow` and read a week of lines:
   `grep 'context_relevance shadow' ~/.human/logs/service-loop-error.log`.
   Expect `casual=1` lines with `items_passing>0` on a real share of turns and
   `threshold_src=null` on most. Compare `top` against `null_p95`: if real
   hits rarely beat the null, recall is not finding anything on casual turns
   and LIVE would change little. Optionally pin the reviewed threshold with
   `HU_CONTEXT_RELEVANCE_MIN_SCORE`.
2. **Memory probes** (PR #593, `docs/guides/memory-benchmarks.md`): replay the
   LoCoMo/MSC probes with both arms and score them:

   ```bash
   HU_STATE_DIR=<snapshot> HU_MEMORY_SQLITE_PATH=<snapshot memory.db> \
     human replay --in $H/locomo/replay/probes.jsonl --out base.jsonl --arm base \
     --endpoint http://127.0.0.1:<spare port>/v1
   HU_CONTEXT_RELEVANCE=live HU_STATE_DIR=<snapshot> HU_MEMORY_SQLITE_PATH=<snapshot memory.db> \
     human replay --in $H/locomo/replay/probes.jsonl --out cand.jsonl --arm cand \
     --endpoint http://127.0.0.1:<spare port>/v1
   python3 scripts/datasets/memory_probe_score.py --probes $H/locomo/replay/probes.jsonl \
       --arm base=base.jsonl --arm cand=cand.jsonl --json report.json
   ```

   Required: the candidate's hit rate on `beyond_window` probes is higher, with
   no category lower by more than its noise band.
3. **Real-turn replay** (PR #594, `docs/guides/replay-harness.md`), 40 turns:

   ```bash
   python3 scripts/blind_ab/replay_export_turns.py --name "$RUN" --limit 40 --since-days 14
   python3 scripts/blind_ab/replay_driver.py snapshot --name "$RUN"
   python3 scripts/blind_ab/replay_driver.py run --name "$RUN" --temperature 0 --delay-ms 3000 \
       --base-env-plist "$PLIST" --arm prod: --arm rel:HU_CONTEXT_RELEVANCE=live
   python3 scripts/blind_ab/replay_feed.py --name "$RUN" --sheets
   ```

   Required: the sensitivity check shows the arm changes the reply request;
   the local judge's emotional-intelligence and reality scores do not drop
   (the AlpsBench failure mode `semantic_recall.h` guards against), and the
   length KS against Seth does not worsen.
4. Then `HU_CONTEXT_RELEVANCE=live` on the plist.

## Rollback

`PlistBuddy -c 'Set :EnvironmentVariables:HU_CONTEXT_RELEVANCE off'` on
`~/Library/LaunchAgents/ai.human.service-loop.plist`, then reload the service.
`off` restores both cliffs exactly.
