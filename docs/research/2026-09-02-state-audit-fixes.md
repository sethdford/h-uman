---
title: "State audit 2026-09-02 — what a green doctor missed, and the fixes"
created: 2026-09-02
status: shipped
audience: maintainers
---

# State audit 2026-09-02 — what a green doctor missed

Companion to `2026-09-01-sota-reassessment.md` (which owns the product-quality
gaps: allowlist, memory store, bi-temporal facts, backfill, retrieval, nightlies,
LoRA probe, filler bank, message_ref, live priority, Binoculars, drift). This
note covers the *operational* gaps found the same morning, each re-derived from
an artifact, and what shipped for each. Every fix names its proof.

## Findings → fixes

| # | Finding (measured 2026-09-02 ~02:30) | Fix | Proof |
|---|---|---|---|
| 1 | **main red on Linux** for 5 commits: `dpo.c` new function outside `HU_ENABLE_SQLITE`; `store_sqlite_vec.c` compiled without `SQLITE_CORE` so Debian's `sqlite3ext.h` rewrote `sqlite3_*` → `sqlite3_api->…` (Apple's header defines `SQLITE_OMIT_LOAD_EXTENSION` and skips it — why macOS built and the deploy went out anyway). Broke Human CI, Benchmark, CodeQL, evaluation, M3 smoke ubuntu. | guard + `SQLITE_CORE=1` on both sqlite-vec TUs | `-fsyntax-only` without SQLite passes; preprocessed store TU has 0 `sqlite3ext.h`/`sqlite3_api` tokens; Memory 361/361, dpo 130/130, sqlite_vec 15/15 |
| 2 | **M3 Closed-Loop Smoke red on macOS since 07-26** (issue #288): daemon SIGSEGV in `sqlite3_step` during gateway chat. Reproduced under ASan on non-prod ports: main-thread `hu_reflection_query_unsurfaced` (init_proposer tick) vs gateway-worker `hu_agent_turn` on ONE memory connection. Apple libsqlite3 is `THREADSAFE=2`; Linux is serialized — macOS-only by construction. **Prod shares that connection between :3006 and the service loop.** | `sqlite3_config(SQLITE_CONFIG_SERIALIZED)` first in `main()`; engine opens with `SQLITE_OPEN_FULLMUTEX` (create + quarantine_and_reopen) | `sqlite3_db_mutex(db) != NULL` pinned (sqlite_integrity 14/14); full live-fire (stub MLX + daemon + M3 driver + adapter swap) 19/19, exit 0, under ASan where it aborted at chat #3 |
| 3 | **Doctor blind**: "16 ok, 0 errors" while eval-nightly was dark 25 days, mlx-server had 25 crash reports before 03:00, the daemon log was 41 MB unrotated, the cursor had lagged chat.db two weeks (the 09-01 replay), and the served adapter had never been human-rated. | 5 checks: `eval_freshness`, `serving_stability`, `log_hygiene`, `imessage_cursor`, `blind_ab_gate` (ctx-injectable, no spawning under HU_IS_TEST; chat.db read behind `chatdb_cursor_repo`) | 14 unit tests; live run: **3 errors** on the real machine (gate 25.1 d, 25 crash reports, gate can't vouch for v6-orpo) |
| 4 | **Product gate not watchdog-covered**: `com.human.eval-nightly` is calendar-only (04:05), `runs = 0` since load, last artifact 08-08; the watchdog (Task 6) covered humanness/doctor/retrain/drift only. Also the daily `evaluation.yml` freshness gate had failed since ~08-10 because the committed proxy verdict was 07-27. | watchdog `eval` job (marker: dated line in nightly-eval.log) | `test_nightly_watchdog.sh` 15/15 |
| 5 | **No log rotation, no timestamps, no levels**: launchd appends stderr forever (530k lines); every line `[component] msg`. | `scripts/rotate-logs.sh` (copy-truncate >20 MB, gzip, keep 5; watchdog `logrotate` job) + `log.h` fallback prints `YYYY-MM-DDTHH:MM:SS LEVEL [component] msg`, `$HU_LOG_LEVEL` filters | watchdog tests (5 new cases); `log_format` 3/3 |
| 6 | **Two gate files, two truths**: `~/.human/blind_ab_gate.json` (C promotion gate) said human n=40 0.225 @07-29; `docs/evaluation/blind_ab_gate.json` (CI) said n=12 0.500 @07-26 with proxy 07-27 FAIL. Neither recorded which adapter was measured; prod serves v6-orpo (08-02), rated by nobody. | repo human half synced from the measured record via the sanctioned writer; proxy half ported from the 08-03 nightly refresh that only ever landed on the unpushed local main; both copies annotated `human.arm.adapter = seth-glm-air-v5-…` from provenance; `score.py --arm-adapter/--arm-note` records it going forward | `test_score.py` + `test_blind_ab_gate.py` 39/39; doctor `blind_ab_gate` now says "measured v5, serving v6 — not human-rated" instead of PASS |
| 7 | **Stale CLI**: `~/.local/bin/human` was the May-17 build (39 subcommands vs 48); anything shelling to bare `human` ran a 3.5-month-old schema. | installer symlinks `human` → `human-daemon` | post-install `human version` == daemon SHA |
| 8 | **Feed jobs ran a dirty dev checkout**: `daily_feed_process.sh` used `$PROJECT_DIR/build/human` — the main checkout, 46 commits behind, every 5 min. | prefer `~/.local/bin/human-daemon` | script diff |
| 9 | **Dead crontab**: two lines every 5 min / daily at a path deleted in May. | removed (backup `~/.human/backfill/crontab.bak-*`) | `crontab -l` empty |

## Still open (not code)

- **Allowlist** (`channels.imessage.allow_from` = 1 handle since May): reactive product has ~0 exposure; the cycle-5 human sheet cannot be drawn from reactive replies until this is opened. Product decision.
- **Serving adapter v6-orpo has no human verdict.** The doctor now says so every run. Rate it or hold it.
- **20 unmerged local branches / 7 dirty worktrees** (May → Sep): triage, not blind merge — listed in the session report.
- **Live-priority admission control** (Task 10) is reopened in the sota-e2e plan after the threading change crash-looped :8741 at 02:20–02:58; the crash loop itself was reverted by that session.
- **ASan in production**: the deployed daemon is the `dev` (ASan) preset; that is the established install path and unchanged here, but it costs RSS and speed. Worth a deliberate decision.
