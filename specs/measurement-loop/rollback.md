# Humanness Gate Rollback Runbook

> Tasks 7+8 of the measurement-loop spec (AC-5, AC-6). What to do when a humanness
> measurement fails: flip the offending gate OFF/SHADOW. Operator-initiated, not
> automatic (design D5).

## When to run this

Per design D5, a rollback is triggered by **one** of:
- A **sustained** regression in the scheduled live-judge proxy (`blind-ab-scores.json` verdict = fail across ≥2 consecutive runs vs `humanness-baseline.json`). A single failing run is noise — do not roll back on it.
- The human blind-A/B sheet, once filled, reading **DISTINGUISHABLE** (ground truth; supersedes the proxy).

The deterministic PR gate is **advisory** (D2) and **never** triggers a production rollback — it gates *merges*, not the running daemon.

## Control surface: the launchd plist env

The gates are flipped via the daemon's launchd plist `EnvironmentVariables`, not via
source. Find your service + plist:

```sh
launchctl list | grep -i human          # find the loaded label
# plist is typically ~/Library/LaunchAgents/<label>.plist
```

### Gate → env var → values

| Gate | Env var | OFF value | SHADOW value | Verified |
|---|---|---|---|---|
| GraphRAG grounding | `HU_GRAPH_GROUNDING` | `off` | `shadow` | ✅ source |
| Salience | `HU_SALIENCE` (+ `HU_SALIENCE_LIVE` / `HU_SALIENCE_SHADOW`) | unset / `off` | `shadow` | ✅ source |
| Bandit humanization | `HU_BANDIT_HUMANIZATION` | `off` | (n/a) | ✅ source |
| Intent directive | `HU_INTENT_DIRECTIVE` | `off` | (n/a) | ✅ source |
| Theory of Mind | **TBD** — no `getenv("HU_*")` found in source; likely config-flag-gated, not env. **Verify the actual control with Seth** before relying on this row. | — | — | ⚠️ unverified |
| Self-uncertainty | **TBD** — same as ToM; verify the real control surface. | — | — | ⚠️ unverified |

> The two ⚠️ rows are deliberately not guessed. A rollback runbook with wrong var
> names is worse than none (audit-verify-before-allege). Confirm ToM /
> self-uncertainty's actual gate (env? config.json? source default?) and fill
> these in — likely part of Task 9 (durable gate, coordinate-with-Seth).

### Procedure (env-gated rows)

1. Edit the plist `EnvironmentVariables` — set the gate's var to `off` (or `shadow`).
2. Reload the service:
   ```sh
   launchctl bootout  gui/$(id -u)/<label>
   launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/<label>.plist
   ```
3. Verify it took:
   ```sh
   human doctor                              # or the daemon's health/gate report
   tail -f ~/.human/logs/service-loop*.log   # confirm the gate's SHADOW/OFF log line
   ```

## ⚠️ Durability caveat

**A reinstall regenerates the plist and re-drops the gates to whatever
`scripts/install-human-daemon.sh` sets.** A manual plist edit does NOT survive a
reinstall. This is the gap Task 9 closes:

**Durable fix (Task 9, coordinate-with-Seth):** move the gate defaults off
plist-env-only onto a reviewed control that survives reinstall — a `config.json`
section the daemon reads, or source defaults — so a rollback decision persists.
Until then, after any reinstall, re-apply this runbook.

## Reconciliation policy (AC-6 / design D5) — operator note

The 6 gates are currently **LIVE**, flipped via the plist env. Their only basis is a
**simulated proxy that reads DISTINGUISHABLE**; the human blind-A/B sheet is **blank**.
This runbook + the measurement loop reconcile that with
`feature-gate-requires-measurement.md` as follows:

- **Stay LIVE** while the automated proxy is ≥ baseline (honors the current daemon state).
- The **human sheet is ground truth**; the proxy only approximates it. Do not let the
  proxy silently become the unchallenged source of truth — the scheduled job flags the
  sheet as stale/blank (AC-7).
- **Stricter, contract-honest alternative:** because the proxy *currently* reads
  DISTINGUISHABLE, the by-the-book move is to roll all 6 gates to **SHADOW now** and
  only return them to LIVE once a real measurement (proxy ≥ baseline, ideally confirmed
  by a filled human sheet) passes. This runbook documents both; the chosen default (D5)
  is stay-LIVE-with-tripwire, reversible to the stricter option by running the env-gated
  rows above for all gates.

**Highest-leverage next action:** run the human blind-A/B sheet once
(`scripts/blind_eval_export.py` → rate → `blind_eval_ingest.py`) to correlate the proxy
against ground truth. Until that exists, the proxy's DISTINGUISHABLE reading is
uncalibrated — neither trust it enough to auto-rollback nor dismiss it.
