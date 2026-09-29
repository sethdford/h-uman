# scripts/second_opinion — the second-opinion lane

A model from a **different family** than the one that serves prod (Gemma 4 31B,
local, not GLM-4.5-Air) checks h-uman's own memory and replies overnight, so the
curator's verifier and the blind-A/B judge stop grading their own work. Spec:
`docs/superpowers/specs/2026-09-29-second-opinion-lane-design.md`. Nothing here
is trained on and nothing it produces reaches a live reply (spec §2).

## Jobs

Run nightly (or ad hoc) via `run_nightly.py`, which serves the local model,
runs the requested jobs against it, and always stops the model afterward:

- **`audit`** (`audit.py`) — re-checks kept curator notes (`contact_insights`
  rows) against *only* the messages they cite. Verdict is `supported` /
  `unsupported` / `unclear` (a note whose evidence can't be fully resolved is
  recorded once as `no_evidence` and excluded from every rate). This is a
  second opinion, not truth — `audit_sheet.py` / `audit_score.py` turn it into
  a measured wrong-acceptance rate against a human check (see Calibration
  below).
- **`gold`** (`gold.py`) — two things, both against real conversations:
  - Where Seth actually replied, a **critique** of h-uman's reply against
    Seth's real one (`gaps`, `missing`, `severity`).
  - Where h-uman replied and Seth did not, a **reference reply** in Seth's
    voice, stored **unrated**. Nothing is exported until a human rates it good
    (see `gold_rate.py` / `gold_export.py`).
- **`judge`** (`judge.py`) — runs the existing `scripts/blind_ab/synthetic_judge.py`
  against local Gemma over the latest detection-mode blind-A/B rating sheet,
  scores it with `scripts/blind_ab/score.py --rater synthetic`, and measures
  agreement (with Cohen's κ) against Seth's own ratings on the same items, when
  a human-rated sheet for the same run exists. Skipped (not an error) when
  there is no rating sheet, in `--dry-run`, or if the nightly `--deadline` has
  already passed.
- **`report`** — writes weekly counts-only JSON summaries for `audit` and
  `gold` to the reports directory (only run outside `--dry-run`).

`--jobs auto` (the default) runs `audit,gold` every day and adds `judge,report`
only on Sundays (`now_local.weekday() == 6`). Pass an explicit comma-separated
list (`--jobs audit`, `--jobs audit,gold,judge,report`) to run a subset.

## REQUIRED: the judge result never touches the human promotion key

`judge_pass` calls `scripts/blind_ab/score.py ... --rater synthetic`, which
writes a `"synthetic"` key into `~/.human/blind_ab_gate.json`. The `"human"`
key in that same file is **never** written by this lane, and the C LoRA
promotion gate reads **only** the `"human"` key
(`scripts/blind_ab/score.py`: *"human" is promotion-authoritative ... "synthetic"
records machine-judged runs under a separate key that never gates promotion*).
The Gemma judge is a between-rounds regression signal, never a certifier —
see Promotion gates below.

## Where output lives

- **Store** — `~/.human/second_opinion.db` (SQLite, created 0600; an
  already-existing file with a looser mode is tightened on open). Tables:
  `audits`, `critiques`, `reference_replies`, `runs`. Every row carries
  `backend` and `prompt_version`; audits/critiques/references are
  deduplicated per `(item, backend)` so a re-run never double-counts.
- **Nightly manifest** — `~/.human/logs/second-opinion-YYYYMMDD.json`
  (`-dryrun` suffix under `--dry-run`), written atomically. It holds **counts
  only** — job names, per-job attempted/error/verdict counts, backend name,
  elapsed seconds, exit reason. It never contains message text, note text, or
  handles.
- **Weekly reports** — `~/.human/logs/second-opinion-reports/audit-YYYYMMDD.json`
  and `gold-YYYYMMDD.json` (written by the `report` job), plus, on a `judge`
  run, `judge-YYYYMMDD/` (the judge's own `judged.csv` / `judge-results.json`,
  from `synthetic_judge.py` and `score.py`) and a `judge-YYYYMMDD.json`
  calibration summary when a human-rated sheet was available to compare
  against.

## Exit codes

- **0** — ok (including "another run holds the lock, did nothing" and "the
  scheduling window is closed").
- **2** — refused; nothing written. Malformed `--deadline`/`--jobs`, an
  unreadable `memory.db`/`chat.db`, a non-loopback Gemma URL, `--backend
  vertex` without ADC credentials, or a lock file that can't be opened.
- **3** — every attempted job raised, or every attempted item across all jobs
  failed. The manifest is still written in this case — it is the evidence
  that the run happened and what went wrong.

## Backends

- **`gemma` (default)** — `mlx-community/gemma-4-31b-it-4bit`, served locally
  by `mlx_lm.server` on `127.0.0.1:8743` (`backend.serve_gemma`). The runner
  starts it, waits up to 10 minutes for `/health`, and **always** stops it
  (success, failure, deadline, or `KeyboardInterrupt`) — it is never left
  running. It refuses to start if something is already answering on `:8743`
  (may be another session's spare server) rather than sharing or killing it.
  `GemmaBackend` itself refuses any non-loopback base URL.
- **`vertex` (opt-in, `--backend vertex`)** — `gemini-3.8-flash` via Vertex AI
  (ADC, project `johnb-2025`), thinking budget 0. **Sends message excerpts off
  the Mac** — this is the only path in this lane that does. It prints a notice
  to stderr naming the Cloud project on every use, and refuses immediately if
  ADC credentials are missing (`~/.config/gcloud/application_default_credentials.json`).
  The default (`gemma`) never sends message text off the Mac.

## Flags (`run_nightly.py`)

`--jobs` (default `auto`), `--deadline HH:MM` (24h local; stops the run
between items once passed — see the curator's rule via `insight_stream.
resolve_deadline`; if the deadline already passed by more than 12h the window
is treated as closed and nothing runs), `--dry-run` (calls the model but
writes no DB rows — `judge` is always skipped under `--dry-run` because it
would otherwise write message text to the reports dir and merge-write the gate
file), `--backend {gemma,vertex}`, `--audit-limit N` (default 25),
`--gold-limit N` (default 10), `--store PATH`, `--manifest-dir PATH`,
`--reports-dir PATH`, `--mem-db PATH`, `--chat-db PATH`,
`--blind-ab-root PATH` (default `~/blind_ab_run`, where the latest detection-
mode rating sheet is found), `--judge-run-dir PATH` (override sheet
auto-discovery), `--lock PATH` (default `~/.human/second_opinion.lock`; a
second concurrent run exits 0 immediately rather than racing the first).

## Promotion gates (from the spec)

- **Curator wrongly-accepted rate** (`audit_score.py`, after the one-time
  human calibration below): `HU_INSIGHT_WIDE` shadow → live requires an
  estimated wrong-accept rate **under 10%** *and* a 95%-joint-coverage
  (Bonferroni-corrected) confidence-interval upper bound **under 20%**,
  alongside the sleep-time curator spec's own §6 targets.
- **Gemma-vs-human judge agreement (κ)**, from the `judge` job's calibration:
  **κ ≥ 0.4** with **n ≥ 20** shared items lets the Gemma judge be used as a
  between-rounds regression signal only. Below that threshold — or with fewer
  than 20 shared items — it is a warning, never a certifier; it never gates
  LoRA promotion (see the human-key note above).
- Below any measurement floor (`n = 0` for a rate, `n < 20` for κ), the code
  reports `"not measured"` rather than a fabricated number — see `stats.
  NOT_MEASURED` and its use throughout `audit.py`, `audit_score.py`, and
  `judge.py`.

## Operator steps

1. **One-time model download** (~18 GB):
   ```bash
   ~/Documents/gemma-realtime-1/.venv312/bin/python -c \
     "from huggingface_hub import snapshot_download; snapshot_download('mlx-community/gemma-4-31b-it-4bit')"
   ```
2. **Smoke test** (manual, not CI — spec §7):
   ```bash
   cd scripts && python3 -m second_opinion.run_nightly --jobs audit --audit-limit 3
   ```
   Check for 3 new rows in the `audits` table, check the manifest at
   `~/.human/logs/second-opinion-YYYYMMDD.json`, then confirm nothing is
   listening on `:8743` afterward.
3. **launchd job** `ai.human.second-opinion`, daily at **07:40** (after the
   curator's 07:30 deadline), running:
   ```bash
   cd /Users/sethford/Projects/h-uman/scripts && /opt/homebrew/bin/python3 -m second_opinion.run_nightly --deadline 09:00
   ```
   Back up any existing plist before installing. This runs from the shared
   main checkout, like the curator.
4. **One-time 30-row human check**, after about 2 weeks of `audit` runs:
   ```bash
   python3 -m second_opinion.audit_sheet   # writes audit_check.csv + audit_check.key.json
   #   -> Seth fills the "supported" column (y/n) in audit_check.csv
   python3 -m second_opinion.audit_score ~/.human/second_opinion/audit_check.csv \
     --key ~/.human/second_opinion/audit_check.key.json
   ```
   `audit_sheet` refuses (exit 2, writes nothing) if there aren't yet 20
   resolvable `unsupported` and 10 resolvable `supported` audits to draw from.
5. **Rate reference replies, then export only the good ones**:
   ```bash
   python3 -m second_opinion.gold_rate --write rate.csv
   #   -> Seth fills the "good" column (y/n) in rate.csv
   python3 -m second_opinion.gold_rate --import rate.csv
   python3 -m second_opinion.gold_export out.csv   # only rated-good rows; unrated/rejected never exported
   ```
