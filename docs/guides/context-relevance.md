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
| `off` (default) | Byte-identical to before: both cliffs apply. |
| `shadow` | Retrieval and the cliffs run as before. One aggregate line per turn and source says what relevance would inject. Nothing sent changes. |
| `live` | An item is injected when its relevance clears the threshold, whatever the word count or tier. Casual turns keep a small byte budget instead of zero. |

Relevance per source:

- **semantic** — the retrieval scorer's cosine score per hit. Threshold
  `$HU_CONTEXT_RELEVANCE_MIN_SCORE`, default `0.46`. Casual turns (the old
  ≤ 12-word classifier, which now only sizes the budget) get
  `$HU_CONTEXT_RELEVANCE_CASUAL_BYTES`, default 480 bytes (two 240-byte hits);
  substantive turns keep the full recall budget (`HU_SEMANTIC_RECALL_MAX_BYTES`,
  default 1200). Only applies under `HU_SEMANTIC_RECALL=live`.
- **graph** — the composer's own query-conditioning: relevant when the message
  names one of the contact's entities, or an owner fact by full name. Kept
  blocks are cut to the casual budget at a line boundary. The contact
  fallback (`HU_GRAPH_GROUNDING_CONTACT_FALLBACK`) is not conditioned on the
  message, so on a casual turn it is still dropped. Only applies under
  `HU_GRAPH_GROUNDING=on`; prod runs grounding in `shadow`
  (2026-09-30), so in prod today this gate changes semantic recall only.

## Where the threshold comes from

No labelled data exists yet on which recalled items Seth's real replies
referenced, so the default is the scorer's own floor, measured on the live
index: the **median cosine similarity between two arbitrary stored memories**.
A hit must be at least as close to the message as an unrelated pair of
memories are to each other.

```bash
python3 scripts/context_relevance_floor.py            # reads ~/.human/memory.db read-only
# vectors=72 pairs=2556  p50: 0.4582  p75: 0.6948  p90: 0.7624  p95: 0.798  p99: 0.8614
```

(2026-10-02). Document-to-document similarities run higher than
query-to-document ones under an asymmetric embedder, so this floor is on the
conservative side: it injects less, not more. Re-run the script after a
reindex and set `HU_CONTEXT_RELEVANCE_MIN_SCORE` if the floor moves.

**Learning it.** Every shadow/live line carries a score histogram, so the
threshold can be refit from real turns:

```
[context_relevance shadow] src=semantic casual=1 items_considered=5 items_passing=2
    bytes_would_inject=311 top_score=0.712 min_score=0.460 hist=0/1/2/1/1
```

Bands are `<0.2 / 0.2-0.4 / 0.4-0.6 / 0.6-0.8 / >=0.8`. Fields are counts,
bytes and scores only: never text, keys or contact ids. The v2 learned
behaviour profile (static-rules inventory §5) is where a per-contact threshold
fitted against reply references belongs.

## Promotion: SHADOW → LIVE

1. Deploy with `HU_CONTEXT_RELEVANCE=shadow` and read a week of lines:
   `grep 'context_relevance shadow' ~/.human/logs/service-loop-error.log`.
   Expect `casual=1` lines with `items_passing>0` on a real share of turns. If
   `items_passing` is almost always 0, the threshold is too high for
   query-to-document scores — lower it from the histogram before going further.
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
