# US-8 difficulty routing — first paired measurement, 2026-09-06 08:23–08:31

`scripts/eval_difficulty_route_shadow.py -n 25 --min-n 20` (commit 555cc30ad). Both arms
use the PRODUCTION persona head (4,909 chars, AC-8.1). Contexts: 25 real inbound messages
of > 12 words with a real Seth reply on file (sha-ordered, deterministic). One on-device
generation failed; 24 paired.

| Arm | composite | anti_ai | EI | reality | LUAR twin (ci95) | median reply words |
|---|---|---|---|---|---|---|
| on_device (:8741, GLM-4.5-Air + bound v6, CONVERSATIONAL) | 0.848 | 0.994 | 3.67 | 5.00 | 0.655 [0.527, 0.744] | 7 |
| cloud_shadow (gemini-3.1-pro-preview, thinkingBudget 4096, ANALYTICAL treatment) | 0.955 | 0.994 | 4.62 | 5.00 | 0.642 [0.540, 0.755] | 11 |

LUAR ceiling (Seth vs Seth) 0.726, floor (Seth vs other humans) 0.640. Seth's real replies
to these contexts: median 31.5 chars.

**Verdict: HOLD** — `composite_delta=+0.1065 (tol 0.02)`, `twin_delta=-0.0130`. The gate
(AC-8.4) requires the cloud twin to be >= the on-device twin with no tolerance; it is
0.013 lower, inside a ci95 half-width of ~0.10. Read: the ANALYTICAL treatment is clearly
better on emotional intelligence for substantive turns and indistinguishable on authorship;
its replies are ~50% longer than the on-device ones, which already sit close to Seth's
own length.

What this does NOT license: no flip. `HU_DIFFICULTY_ROUTE` stays at its default (OFF; the
C side is SHADOW-only and LIVE fails closed). Two follow-ups before a promotion decision:
1. The twin rule should be noise-aware (the same F1 the critic raised on US-2): HOLD only
   when the twin regression is CI-distinguishable, otherwise treat the twin as "no
   regression" and let the composite decide. That is a contract change to AC-8.4 — Seth's
   call, recorded here, not applied.
2. Re-run with `-n 40` so both arms' twin CIs tighten; latency and cost of the cloud arm
   were not measured here and matter for a reactive channel.

Evidence: `difficulty-route-shadow-2026-09-06.json` (ids, counts, scores, lengths only).
