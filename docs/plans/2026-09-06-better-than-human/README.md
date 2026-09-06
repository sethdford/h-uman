# Better than human — program plan (2026-09-06)

Owner session: 089d4814. Successor to `~/.claude` memory `project_h-uman_sota_next_2026-09-04`.
Definition of "better than human": detection ≤ 0.35 on the blind human gate (n≥40) AND
specificity per reply ≥ Seth's own AND prospective-memory F1 > 0.65 AND timing within rater
preference. Today only the first is measured (0.225, n=40, 2026-07-27).

Each item ships behind its gate; no item is DONE on a green suite alone.

| # | Item | Gate (measurement) | Status |
|---|------|--------------------|--------|
| 1 | Identity invariants: `hu_persona_creator_write` round-trips unknown keys + atomic write; doctor check flags a persona that lost keys vs its newest backup | test: write-then-load preserves an unknown key; doctor errors on the 2026-09-06 gutted file, ok on the restored one | DONE (writer 3/3, doctor 7/7, live doctor ok) |
| 2 | State-first memory read path (graph_state.c resolver built 09-04): honor event_end/supersedes_id end-to-end | held-out changed-facts set retrieved at the right version | DONE (resolver + composer deployed 09-05; held-out gate 6797ac465: 24 shuffled chains × 3 moments, 12/12). Residual: one relation per neighbor entity in the composer dedupe |
| 3 | Insight stream + budget-reserved recall block in the compact head (survives the 16 KB MLX cap) | named entities / insider refs per reply vs Seth; LUAR twin not dropping; EI flat | todo |
| 4 | Sleep-time consolidation into a linted Seth-wiki feeding the head (uses GPU hours freed by the reflection fix) | prompt bytes down, composite flat/up | todo |
| 5 | Prospective memory wired (table exists, reads empty) | deferred intentions executed on cue | todo |
| 6 | Close the parametric loop: adapters self-promote behind LUAR + Binoculars + Seth-judge; v6 human-rated | promotion happens without a hand install; doctor blind_ab_gate error clears | todo |
| 7 | Local-only: tool turns stay on GLM (no gemini degrade), proposer local, fallback = say less | zero Vertex calls on the reply path over 24h | todo |
| 8 | Human gate re-run n=40 after 2–4 | detection ≤ 0.35 held | todo |

Do NOT: another voice round, steering, Q6, reconstructive-recollection redesign before its harness key fix.

## Log
- 2026-09-06 08:xx — persona gutted by ungated style reanalyze; restored + gated (1a7f395ea). Program started.
- 2026-09-06 — item 1 landed: creator_write preserves unknown keys + atomic rename; doctor persona_integrity (newest-backup reference). Attribution: 06:01 writer was a partial-struct CLI write (analyzer shape), not reanalyze. Gemini 3.8 Flash verified 200 and set as fallback + proposer per Seth.
- 2026-09-06 — item 3 step A: prompt budget 16 KB → 24 KB. Re-measured GLM on :8741 with cache-defeating prompts, fact at 60% depth: 16/24/32/40 KB all answered + recalled, prompt_tokens linear (3031/4518/5947/7518), cold latency 10.2/15.0/20.6/27.5 s. Live prompt mean ~17 KB (persona 9.0K, pm 2.1K, guard 1.8K, memory 1.6K, stm 1.1K) so the old cap cut memory+pm most turns. Gate for step A: per-turn prompt_trim/truncation log shows 0 memory/pm bytes cut on ordinary turns after deploy.
