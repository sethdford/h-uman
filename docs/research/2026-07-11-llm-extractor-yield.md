# LLM Fact-Extractor Yield Measurement (event schema)

Date: 2026-07-11. Harness: offline, sequential, dedicated base-model server
(mlx-community/gemma-4-31b-it-8bit on :8744, no adapter — never the live
:8741). Corpus: 30 deduped real inbound iMessages from
`blind_ab_run/triples_profiled.json` (192-row export). max_tokens=2048,
temperature=0.0.

## Result

| Extractor | Facts | Msgs with ≥1 | Recall | Facts/msg |
|---|---|---|---|---|
| Dict scanner (hu_fast_capture family) | ~2 / 192 | — | ~1% | ~0.01 |
| LLM, ORIGINAL schema (pre-ea4755c8) | 0 / 8 pilot | 0/8 | 0% | 0.00 |
| **LLM, EVENT schema (ea4755c8)** | **24 / 29** | **18/29** | **62%** | **0.83** |

(1 of 30 requests timed out; excluded.)

## Why the original schema measured zero

1. Predicate vocabulary was preference/state-only — a schema-faithful model
   correctly emits `{"facts":[]}` for event messages ("I finally graduate
   sunday"). Observed verbatim in the model's reasoning.
2. gemma-4 thought channel echoes the requested schema (with braces) before
   the answer; the old first-{..last-} extraction spanned thought+answer and
   failed to parse.
3. mlx-server's 256-token default completion cap is fully consumed by
   thinking → finish=stop with zero answer content (the 2026-05-24
   thinkingBudget gotcha, local edition; skip_thinking_primer is still
   unwired on the non-stream path).

All three fixed/mitigated in ea4755c8 (event predicates, last-`{"facts"`
envelope parse, immediate-JSON instruction). Note the daemon-side provider
must allow an adequate completion budget for the thought+JSON — verify when
flipping the gate.

## Sample extractions (quality check)

- "I finally graduate sunday🎈" → contact/graduating/sunday (0.9)
- "are you gonna move to seattle at all?" → contact/asking_about/user moving to seattle
- "Possibly law school its j a huge financial commitment" → contact/planning/law school
- "Hell of a spreadsheet though. I also like the Claude chrome plugin" → contact/likes/Claude chrome plugin
- "okay" / "7911" / "Oh wow" → correctly zero

## Activation path

Pipeline already wired end-to-end: daemon.c:2432 injects the provider;
`maybe_llm_fact_fallback` (src/memory/personal_model.c) runs on regex-miss;
gate `HU_LLM_FACT_EXTRACT` off/shadow/live, default OFF. Next step per
feature-gate-requires-measurement.md: set `HU_LLM_FACT_EXTRACT=shadow` in
the daemon plist and watch `llm_fact_extract` shadow lines before LIVE.
