---
title: "ADR — Adapter rollback signal: multi-signal AND, auto-rollback, manual reinstate"
created: 2026-05-11
status: accepted
deciders: engineering, ml
parent: ../2026-05-10-sota-roadmap-6mo.md
related:
  - ../2026-05-10-sota-roadmap-6mo.md
  - ../2026-05-10-m3-frontier-model-bridge.md
---

# ADR: Adapter rollback signal — multi-signal, auto-rollback, manual reinstate

## Context

The SOTA roadmap (Phase A5) describes a continuous-learning loop: nightly retraining produces a **candidate** LoRA adapter, an A/B harness compares it to the current adapter, and an automatic promotion decision lands. The matching rollback contract is the other half:

> If a promoted candidate degrades behavior in production, the system must roll back **automatically**, without waiting for a human to notice.

Two failure modes are at issue:

1. **Quality regression caught at promotion** — eval said yes but reality says no.
2. **Drift over time** — adapter was fine at promotion but degrades as new data arrives.

A single-signal trigger (e.g. "eval drops 5%, roll back") is too brittle: judge-model variance alone exceeds 5% across runs. Multi-signal AND with conservative thresholds is required.

## Decision

**Auto-rollback fires when ANY of the following three conditions trips, evaluated on every nightly run.** Trip thresholds are explicit floors; any one trip is sufficient.

| Signal | Source | Threshold |
|---|---|---|
| **S1 — Persona eval regression** | Local-judge persona eval suite (`eval_suites/persona/`), rolling 7-night median | Median ≥ 5 percentage points below the last-promoted adapter's anchor score |
| **S2 — Perplexity drift on held-out** | Deterministic PPL on `eval_suites/perplexity/holdout.txt` (no judge, just NLL) | PPL ≥ 10% above last-promoted baseline |
| **S3 — User-feedback signal** | Local-only feedback events (thumbs-down rate, retry-rate, abandonment) collected per Phase C5.1 | 7-day rate ≥ 50% above pre-promotion baseline AND minimum 30 events |

Auto-rollback procedure:

1. Detect: nightly bench job marks the trip and emits a `rollback-required` event.
2. Switch: daemon hot-reloads the previously-promoted adapter (last-known-good kept on disk per A5.4).
3. Lock: candidate-promotion is blocked for 7 days; the system stays on the last-known-good adapter.
4. Open ADR-style incident record under `docs/plans/incidents/YYYY-MM-DD-adapter-rollback-*.md` with the trip evidence (which signal, what magnitude, training-data hash that produced the candidate).
5. Notify: Slack/Linear alert with the incident link.

Manual reinstate procedure (no automatic re-promotion after rollback):

- An engineer reviews the incident record, root-causes the regression, and explicitly reinstates with `human ml adapter promote --force <adapter-id>`. The `--force` flag is required and is logged.

Metadata gates:

- The candidate's training-data hash, eval scores, and PPL are stored alongside the adapter file (`<adapter>.metadata.json`). Rollback comparison uses this metadata, not in-memory state, so a daemon restart doesn't lose the comparison anchor.
- Last-known-good adapter retained for at least 30 days or 3 rollback cycles, whichever is longer.

Meta-metric:

- **Rollback frequency** is tracked as a quality signal in its own right (Phase E4 longitudinal eval). Frequency > 1 per 7 nights signals the continuous-learning loop is too aggressive; the auto-promotion threshold (Phase A5.2, "Δ ≥ 1 stderr") tightens by 0.5 stderr until rollback frequency drops below the floor.

## Consequences

- **Positive:** defense in depth — judge variance alone cannot trigger rollback; perplexity drift caught even when judge looks happy; user signal caught even when both eval surfaces miss; manual reinstate prevents a thrashing loop.
- **Negative:** too-conservative thresholds could mask real improvements as noise. Mitigation: the auto-promotion side requires Δ ≥ 1 stderr lift; auto-rollback requires the absolute regression thresholds above. Asymmetry is intentional — easier to keep the current adapter than to roll back.
- **Operational cost:** an extra daily compute slot for PPL eval. PPL eval is deterministic and ~10 seconds; cost negligible.
- **Documented in:** `src/ml/CLAUDE.md` (rollback section), `docs/standards/ai/evaluation.md` (rollback policy section to be added).

## Status

Accepted. Initial thresholds locked at the values above. Revisit at Phase A6 (Month 6) with empirical data on rollback frequency; tune thresholds based on actual judge-variance and PPL-variance distributions.
