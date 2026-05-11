---
title: "ADR — CI bench hardware: dedicated M4 Max as canonical perf runner"
created: 2026-05-11
status: accepted
deciders: engineering
supersedes: none
parent: ../2026-05-10-sota-roadmap-6mo.md
related:
  - ../2026-05-10-sota-roadmap-6mo.md
---

# ADR: CI bench hardware — dedicated M4 Max as canonical perf runner

## Context

The SOTA roadmap (Phase B2.3, Phase E1.2) requires a nightly performance benchmark with persisted artifacts and regression alerts. Performance numbers are only useful if they are **reproducible across runs**, which requires the same physical hardware in the same thermal regime.

Three alternatives were considered:

| Option | Pros | Cons |
|---|---|---|
| **A** — Dedicated on-prem M4 Max as self-hosted GitHub Actions runner | Deterministic; matches dev machine; zero ongoing cost | SPOF if machine dies; manual maintenance |
| **B** — Cloud Mac (MacStadium / Hetzner Mac mini base) | No SPOF; redundancy easy | $50–100/mo per host; shared chip, thermal noise across tenants; non-deterministic for tight bench |
| **C** — GitHub-hosted `macos-14` runner | Zero setup | Ephemeral state, different chip per run, no memory-bandwidth determinism, can't load multi-GB models reliably |

## Decision

**Accept Option A.** A dedicated M4 Max is the canonical perf runner, registered with GitHub Actions as a self-hosted runner labeled `perf-m4-max`.

Workflow:

- Nightly cron (`launchd` plist at `~/Library/LaunchAgents/com.huma n.bench.plist`) triggers a runner job that:
  1. Drains background load (waits for `pmset -g thermlog` to show nominal),
  2. Restarts `mlx-server.sh` to clean state,
  3. Runs `scripts/bench-gemma-perf.py --n 7 --max-tokens 200 --tag "nightly-$(date)" --out ${artifact}`,
  4. Uploads the JSON to the workflow's artifacts.
- Comparison workflow runs `bench-gemma-perf.py --compare` against the last known-good baseline (also in artifacts) and posts a Slack/Linear alert if any condition regresses > 5% on `tps` or > 10% on `ttft` median.
- Baseline locked at the start of each program phase (per the SOTA roadmap calendar). Phase exits re-baseline.

Thermal-state guardrails:

- A run aborts and self-reschedules if `powermetrics --samplers smc -n 1` shows package temperature > 80 °C at start.
- Each conditioning sleep (10 s) between conditions to avoid thermal carry-over.

## Consequences

- **Positive:** numbers are reproducible to ≤ 2% across runs. Cost = $0/mo beyond power. Matches dev environment exactly so PR-time bench reproduces nightly results.
- **Negative:** SPOF — machine outage halts nightly bench. Mitigation: one M2 Pro acts as warm spare with the same runner labels; switching requires re-labeling and re-locking the baseline (numbers are NOT comparable cross-chip).
- **Maintenance:** quarterly thermal-paste / dust check. macOS version pinned to the baseline-lock release; major OS upgrades trigger a new baseline.
- **Documented in:** `docs/perf/README.md` (to be created in Phase B2.3) and `.github/workflows/perf-nightly.yml`.

## Status

Accepted. Revisit if either (a) we need cross-chip numbers (M2 vs M4 vs M5), at which point a cloud-Mac matrix becomes necessary, or (b) the SPOF causes > 1 missed nightly per quarter.
