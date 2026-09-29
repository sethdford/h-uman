# US-5 register gate — live paired measurement, 2026-09-05

Two runs against production `:8741` (GLM-4.5-Air-4bit + bound v6 adapter), same fixed
40-context selection as the 09-03 semantic-recall PROMOTE (33 casual ≤12 words, 7
substantive), same judge and tolerances, installed `human-daemon` for retrieval/scoring.

| Run | Pair | Verdict | Casual (n=33) composite | Casual EI | Reality |
|---|---|---|---|---|---|
| r1 `register-gate-live-paired-2026-09-05.json` | no-recall vs gate-LIVE | INCONCLUSIVE (coverage denominator counted suppressed contexts; substantive PROMOTE on n=7) | 0.886 → 0.886 (identical: neither arm had recall on casual) | 3.970 → 3.970 | 5.0 |
| r2 `register-gate-live-paired-2026-09-05-r2.json` | semantic LIVE, gate OFF vs gate LIVE | **PROMOTE** | **0.865 → 0.886** | **3.788 → 3.970** | 5.0 → 5.0 |

r2 is the measurement US-5 asked for: both arms carry semantic recall; only casual
contexts differ. Coverage 1.0 (all 33 casual contexts had a recall block to withhold,
18,165 bytes total); LIVE-arm casual `recall_bytes` = 0 on all 33 (AC-5.4). Substantive
(n=7) is an identical control across arms and is reported INCONCLUSIVE (< min_n 30).

Reading: on short casual turns, injecting retrieved memories costs ~0.02 composite and
~0.18 EI on this corpus; the register gate removes exactly that cost. The r1 casual
no-recall number (0.886 / 3.970) matches r2's gate-LIVE arm, so the effect reproduces
across two independent generations.

Harness note: r1 exposed that the original `--register-gate live` mode could not see the
effect (fixed in `a20ee93ca`); the sprint's verifier, critic and panel all passed the
unfixed harness. See `~/.claude/lessons.md` 2026-09-05.

`HU_SEMANTIC_RECALL_REGISTER_GATE` remains **OFF** in the service-loop plist. Flipping it
to `live` is Seth's call.
