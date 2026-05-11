---
title: Perf bench infrastructure
last-updated: 2026-05-11
owners:
  - perf
parent-plan: ../plans/2026-05-10-sota-roadmap-6mo.md
adrs:
  - ../plans/adr/2026-05-11-ci-bench-hardware.md
---

# Perf bench infrastructure

This directory holds the artifacts produced by the nightly MLX inference
benchmark and any ad-hoc perf investigations. The bench protects the
N5 (TPS), N6 (TTFT), and N7 (RSS) metrics defined in the SOTA roadmap.

## Quick reference

| What | Where |
|------|-------|
| Workflow | [`.github/workflows/perf-nightly.yml`](../../.github/workflows/perf-nightly.yml) |
| Bench script | [`scripts/bench-gemma-perf.py`](../../scripts/bench-gemma-perf.py) |
| Locked baseline | [`docs/perf/baseline.json`](baseline.json) |
| Runner plist template | [`scripts/perf-nightly-launchd.plist.template`](../../scripts/perf-nightly-launchd.plist.template) |
| Hardware ADR | [`docs/plans/adr/2026-05-11-ci-bench-hardware.md`](../plans/adr/2026-05-11-ci-bench-hardware.md) |
| Drift reports (E4) | `docs/perf/drift/<YYYY-WW>.json` (created weekly) |
| Profile artifacts (B2.4) | `docs/perf/profile-2026-MM.md` |
| Competitive bench (B6) | `docs/perf/competitive/` |

## Bench design

The bench hits `POST /v1/chat/completions` on the local MLX server
(`scripts/human-serve.sh`) and measures:

- prompt / completion token counts (from server-reported usage),
- TTFT (stream mode only — measured from request start to first content
  delta),
- total elapsed,
- generation tok/s.

Each measurement is repeated `n` times across 5 prompt shapes (short
ack, short reply, mid factual, mid creative, long reply) in both
streaming and non-streaming modes — 10 conditions total per run.

Per-condition output is the median, mean, stddev, min, and max so we
can both alert on the central tendency and detect tail-latency
regressions.

## How nightly runs work

1. **Trigger.** GitHub-hosted scheduler fires `perf-nightly.yml` at
   07:00 UTC (~03:00 ET).
2. **Runner picks it up.** The `perf-m4-max` self-hosted runner is
   always online (managed by `LaunchAgents/com.github.actions.runner.perf-m4-max`).
3. **Thermal guardrail.** If `powermetrics` reports the CPU die above
   80 C, the run aborts before doing any meaningful work and an artifact
   captures the temperature for triage.
4. **MLX-server health check.** If the server isn't responding,
   `scripts/human-serve.sh restart` is called and we wait 8 s before
   re-checking. If still down, the job fails.
5. **Bench runs.** `bench-gemma-perf.py --n 7 --max-tokens 200` against
   `http://127.0.0.1:8741`. Output to `/tmp/bench-<tag>.json` and
   uploaded as a workflow artifact (30-day retention).
6. **Compare against baseline.** A short Python diff against
   `docs/evaluation/baseline.json` checks:
   - TPS regression > 5% on any condition → trip,
   - TTFT regression > 10% on any condition → trip.
7. **Regression issue.** If any condition tripped, a GitHub Issue is
   opened with the comparison report attached. Issue is labelled
   `perf-regression`, `sota-roadmap`.

## How to register a new self-hosted runner

These are the one-time operator steps. Do not commit any token.

```bash
# 1) Get a runner registration token from GitHub:
#    https://github.com/sethford/h-uman/settings/actions/runners
#    Click "New self-hosted runner", choose macOS aarch64.

# 2) Stage the runner directory (outside the repo, never under git):
mkdir -p ~/Documents/actions-runner-perf-m4-max
cd ~/Documents/actions-runner-perf-m4-max

# 3) Download the runner package (copy URL + sha from the GH page above).
curl -o actions-runner-osx-arm64.tar.gz -L \
  https://github.com/actions/runner/releases/download/v2.X.Y/actions-runner-osx-arm64-2.X.Y.tar.gz
tar xzf actions-runner-osx-arm64.tar.gz

# 4) Register (REPLACE <TOKEN> with the one-time token from the GH UI):
./config.sh --url https://github.com/sethford/h-uman \
            --token <TOKEN> \
            --name perf-m4-max \
            --labels self-hosted,macOS,perf-m4-max \
            --work _work --unattended

# 5) Render and install the LaunchAgent:
cd ~/Documents/h-uman
sed -e "s|@HOME@|$HOME|g" \
    -e "s|@RUNNER_DIR@|$HOME/Documents/actions-runner-perf-m4-max|g" \
    scripts/perf-nightly-launchd.plist.template \
  > ~/Library/LaunchAgents/com.github.actions.runner.perf-m4-max.plist
mkdir -p ~/Library/Logs/perf-runner
launchctl load -w ~/Library/LaunchAgents/com.github.actions.runner.perf-m4-max.plist
launchctl list | grep com.github.actions.runner.perf-m4-max
```

After load, the runner should appear `online` in
https://github.com/sethford/h-uman/settings/actions/runners within ~30 s.

## How to manually trigger a bench

```bash
gh workflow run perf-nightly.yml \
  -f tag=manual-$(date -u +%Y%m%d-%H%M) \
  -f iterations=11 \
  -f max_tokens=200
```

Or locally, against your own MLX server:

```bash
# (server must be running on :8741 — see scripts/human-serve.sh)
scripts/bench-gemma-perf.py \
  --tag local-$(date -u +%Y%m%d-%H%M) \
  --n 7 --max-tokens 200 \
  --out /tmp/bench-local.json
```

## How to lock a new baseline

The baseline is intentionally hard to update — it's a signal of intent.
Update it when:

- A planned optimization landed and the new numbers are the floor we
  intend to defend, OR
- A planned regression landed (with ADR) and the previous floor no
  longer applies.

Do not update the baseline to silence a regression alert. The alert is
the point.

Procedure:

1. Reach a known-good server state (config + adapter + draft).
2. Run `bench-gemma-perf.py --n 11 --out /tmp/baseline-candidate.json`.
3. Inspect the JSON — verify n=11 across all conditions, no errors.
4. `cp /tmp/baseline-candidate.json docs/perf/baseline.json`
5. Update [`docs/perf/baseline.json`](baseline.json)'s accompanying ADR /
   commit message with:
   - the date,
   - the commit / config / adapter state,
   - which optimizations are in play.
6. PR. The PR description must state what changed since the previous
   baseline and why we're committing to defend the new floor.

## Thresholds (defended in CI)

Per [`docs/plans/adr/2026-05-11-ci-bench-hardware.md`](../plans/adr/2026-05-11-ci-bench-hardware.md):

| Condition | Metric | Regression trip |
|-----------|--------|-----------------|
| any | median TPS | > 5% drop vs baseline |
| any | median TTFT | > 10% increase vs baseline |

Bypass requires a `regression-accepted` label on the PR/Issue plus a
linked ADR explaining the trade-off.

## Future work referenced from here

- **B2.4** — per-layer MLX profile. Lives at `docs/perf/profile-2026-MM.md`.
- **B6** — public head-to-head benchmark. Lives at `docs/perf/competitive/`.
- **E4** — weekly longitudinal drift reports. Live at `docs/perf/drift/`.
