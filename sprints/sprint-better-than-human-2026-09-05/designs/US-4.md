# Design for US-4: Re-run when-to-speak MIR/FIR against the real `proactive_decisions` log

Status: READY
Sprint: sprint-better-than-human-2026-09-05
Story: US-4 (P1), measurement-only, no C changes expected.

## Verified findings (the load-bearing facts this design rests on)

1. **`proactive_decisions` has real rows.** Queried `~/.human/memory.db` read-only,
   aggregates only:
   - `COUNT(*) = 87`, `MIN(ts)`..`MAX(ts)` = 2026-09-02 13:09:26 → 2026-09-05 19:09:00
     (~3.3 days), `COUNT(DISTINCT contact) = 3`.
   - `GROUP BY decision, sent`: `decline/sent=0 → 70`, `send/sent=0 → 15`,
     `send/sent=1 → 2`.
   - `GROUP BY trigger`: `init_proposer_llm → 78`, `proactive_send → 9`.
   - AC-4.1 is satisfiable as-is: `load_decisions()` only falls back when the table is
     **empty for the window**; with 87 rows since 2026-09-02 and the default
     `--days 90`, `decisions_source` will read `proactive_decisions`.
   - AC-4.2 is very likely to fire for **FIR specifically**: today's `compute_fir`
     treats every row with `decision=='send'` as one eligible send, giving a pool of
     at most 17 (15+2) rows before any "Seth already engaged" exclusion — under the
     default `--min-n 30`. (After the dedup fix below, the eligible pool for FIR
     shrinks further to the 2 confirmed `sent=1` events, which is certain to refuse.)
     MIR's `n` depends on chat.db positive-moment count in the same window, which
     this design does not query (out of scope — DB-4 only asked for
     `proactive_decisions` aggregates); it may or may not clear 30. **AC-4.2's
     refusal path must be exercised and its exact `mir.n`/`fir.n` recorded as the
     story's result even if only one of the two refuses** — the script already
     returns per-metric `n` in its refusal message, so both numbers are captured in
     one run regardless of which (or both) trip the gate.

2. **There is no shared id/foreign key between a FIRED proposal row and its later
   send-outcome row** — confirmed by reading the schema
   (`src/memory/repos/proactive_decisions_repo_sqlite.c`'s `CREATE TABLE`: no
   proposal_id/parent column) — matching the C5 report. **But there is a reliable
   de-facto join key: `(contact, ts)`.** Verified by reading both call sites:
   - `src/agent/init_proposer.c:1278` calls `init_proposer_record_decision(...,
     now_unix)` for the PROPOSAL row (`trigger="init_proposer_llm"`, `sent=0`
     always), and this is the **only** call site of `init_proposer_record_decision`
     that fires — all 5 call sites (`init_proposer.c:1087,1100,1141,1153,1278`) live
     strictly inside `hu_init_proposer_tick_with_provider_ex` (`init_proposer.c:1058`
     to EOF at 1284); `hu_init_proposer_tick_with_provider` (813–1057, no `_ex`)
     contains **zero** calls to `init_proposer_record_decision` (see finding 3 below
     — this matters).
   - `src/daemon/daemon_proactive.c:923` calls
     `hu_proactive_decisions_repo_record(..., now, ...)` for the OUTCOME row
     (`trigger="proactive_send"`) from `daemon_proactive_record_decision`, called at
     `daemon_proactive.c:970` (decline/`send_failed`, on a channel-send error) and
     `:983` (send/`sent=1`, on confirmed delivery).
   - In the caller, `src/daemon.c:1531`–`1671`: both the proposal-producing call
     (`hu_init_proposer_tick_with_provider_ex(..., (int64_t)now, ...)` at `:1534`)
     and the outcome-producing call (`hu_daemon_proactive_send_and_record(...,
     (int64_t)now, ...)` at `:1670`) pass the **same `now` local** with no
     reassignment of `now` anywhere between those two lines (grepped `now =` /
     `now=` over daemon.c:1531–1671 — zero hits). So for one contact's proactive
     attempt in one tick, the proposal row and its outcome row are **`ts`-identical
     by construction**, not by luck of clock resolution — this is the join key.
   - This is a *practical* key (not a schema-enforced one): two genuinely distinct
     ticks for the same contact landing on the same integer unix-second `ts` would
     collide. Given the daemon's tick cadence (minutes, not sub-second), this is
     documented as an accepted, low-probability limitation, not treated as solid
     ground for anything beyond this diagnostic script.

3. **A second, older proactive-send path exists that never reaches
   `proactive_decisions` at all** — `src/daemon.c:12119` calls
   `hu_init_proposer_tick_with_provider` (no `_ex`, the "Initiative Layer / T4
   delivery wire", dated 2026-05-25 in its comments) which sends directly via
   `ch->channel->vtable->send(...)` at `daemon.c:12216` and records outcomes only to
   a JSONL file (`hu_init_outcome_append` / `hu_init_outcome_resolve_pending`), never
   calling `hu_proactive_decisions_repo_record`. If this path is still reachable in
   production (gated on `config->initiative.target_handle` being non-empty and
   `dry_run=false` — **UNVERIFIED**: this design deliberately did not read the live
   `~/.human/config.json` to check, since that field can hold a real phone number and
   wasn't part of what this story asked DB-4 to check), any sends through it are
   **invisible to this measurement** — not a join problem, a coverage gap. This is
   flagged as a caveat on the eventual MIR/FIR numbers, not fixed here: AC-4.5
   forbids daemon.c changes, and de-duplicating/consolidating the two tick paths is
   a genuine C-level change out of scope for a measurement-only story. Recorded as
   an open follow-up in "Out of scope" below.

## Approach

Run `scripts/eval_when_to_speak.py` against the now-populated `proactive_decisions`
table, with two narrowly-scoped fixes to the script itself (both inside the
`proactive_decisions` branch of `load_decisions()` / `compute_fir()` — the
"source-detection" and adjacent decision-loading code the story's file-touch note
already anticipated touching):

1. **Join/dedup fix for FIR** (the core of this design). Today `compute_fir` treats
   every row with `decision=='send'` as one eligible send. Per finding 1, a single
   real delivered send appears as **two** rows sharing `(contact, ts)` — a proposal
   row (`decision=send, sent=0`) and an outcome row (`decision=send, sent=1`) — so
   today's loop **double-counts every confirmed send** in FIR's denominator and
   numerator. Symmetrically, a proposal that was FIRED but failed at send time
   produces a proposal row (`decision=send, sent=0`) plus an outcome row
   (`decision=decline, reason=send_failed, sent=0`) at the same `(contact, ts)`; only
   the proposal row passes today's `decision=='send'` filter, so a **never-delivered**
   message is wrongly admitted as an "eligible send" and — since nothing was ever
   delivered — will always register as a false interruption (no reply possible),
   inflating FIR's numerator for an event that never happened on the wire.

   Fix, entirely inside `eval_when_to_speak.py`:
   - `load_decisions()`'s `proactive_decisions` SELECT gains `trigger` (currently not
     selected): `SELECT ts, contact, decision, sent, trigger FROM proactive_decisions
     WHERE ts >= ? AND contact IS NOT NULL`.
   - A new `resolve_decision_events(rows)` groups rows by `(contact, ts)` and
     resolves each group to ONE logical event:
     - If a `trigger='proactive_send'` row is present, it is authoritative:
       `resolved_decision = row.decision`, `resolved_sent = bool(row.sent)`.
     - Else (only `trigger='init_proposer_llm'` row(s)): `resolved_sent = False`;
       `resolved_decision` = the proposer's own decision. A resolved `send` with no
       outcome row is counted separately as `fired_dropped_pre_send` (FIRED but
       skipped before any send attempt — validator/gate/dedup/rate-limit/sanitizer —
       see `daemon.c:1573`–`1666`'s skip branches) and is **not** fed into FIR.
   - `compute_fir`'s eligible population becomes resolved events where
     `resolved_decision == 'send' AND resolved_sent is True` — i.e. confirmed
     delivery, matching the metric's own docstring ("actual SEND decisions ...
     landed"). This shrinks today's un-deduplicated pool of ≤17 rows to the 2
     confirmed-`sent=1` events measured above — which will refuse under `--min-n 30`
     (see AC-4.2 handling above; this is the honest number, not a bug to work around).
   - `compute_mir`'s `has_send` check is an `any(...)` presence test over nearby
     decisions and is **not** affected by the duplicate-row issue (a duplicate can't
     make `any()` more true) — no change needed there. It should still key off the
     resolved decision value for consistency, but semantically MIR asks "did the
     policy engage at all near this positive moment," which a bare proposal already
     answers — so MIR intentionally keeps counting `resolved_decision=='send'`
     regardless of `resolved_sent` (a FIRED-then-dropped attempt still means the
     policy noticed and tried).
   - Output JSON gains two informational counters under `fir` (not part of the rate):
     `fir.dropped_pre_send` and `fir.send_failed`, so the evidence file states where
     rows went instead of silently discarding them
     (`.claude/rules/no-number-without-a-measurement.md`).

2. **`FIR_WINDOW_HOURS` module constant** (conflict-resolution item — see Conflicts).
   `--fir-window-hours` is currently only an inline argparse default (`24.0`); no
   module-level name exists (`grep -rn FIR_WINDOW_HOURS scripts/` finds only a
   docstring mention). Add `FIR_WINDOW_HOURS = 24.0` at module scope and default the
   argparse flag from it, so US-3's AC-3.1 (`from
   scripts.eval_when_to_speak import FIR_WINDOW_HOURS`, not re-derived) has something
   real to import. Zero behavior change (same default value).

3. **`--compare-baseline PATH` flag** for AC-4.4. Optional CLI arg; if given, loads
   the JSON US-3 commits to its evidence path, reads its `{n, rate}` fields, and adds
   to the output:
   `"comparison": {"available": true, "fir_rate": <fir.rate>, "seth_baseline_rate":
   <us3.rate>, "verdict": "fir_le_baseline"|"fir_gt_baseline"}`. If the flag is
   omitted, or the path doesn't exist, or US-3 itself refused (its file absent for
   that reason): `"comparison": {"available": false, "reason": "US-3 baseline not
   provided or US-3 refused"}`. This makes AC-4.4's three-way state explicit in the
   committed artifact rather than left to be inferred by a reader diffing two JSON
   files.

No changes to `daemon.c`, `init_proposer.c`, or `daemon_proactive.c` — the join/dedup
happens entirely downstream, in the Python evidence-generation script, over data
that's already being written today.

## Files touched

- `scripts/eval_when_to_speak.py` — edit: add `trigger` to the `proactive_decisions`
  SELECT; add `resolve_decision_events()`; rewrite `compute_fir`'s input to the
  resolved/deduped event list; add `fir.dropped_pre_send` / `fir.send_failed`
  counters; add module-level `FIR_WINDOW_HOURS` constant; add `--compare-baseline`
  flag + `comparison` output block. No change to `compute_mir`, `label_positives`,
  chat.db extraction, or the existing refusal/exit-code contract.
- `scripts/test_eval_when_to_speak.py` — **new** (none exists today — verified via
  `find . -iname '*test_eval_when_to_speak*'`, zero hits). Hermetic, no network, no
  real chat.db/memory.db access — follows the pattern in
  `scripts/test_eval_persona_evolution.py` (`import eval_when_to_speak as ewts`,
  build synthetic sqlite files under `tempfile`).
- `sprints/sprint-better-than-human-2026-09-05/evidence/us4-when-to-speak-<date>.json`
  — new: either the successful result (MIR + FIR + comparison + both diagnostic
  counters) or, per AC-4.2, a short text file/JSON stub recording the exact refusal
  (`mir.n`, `fir.n`, `min_n`) as the story's negative result.

## Data provenance / privacy

- Source: `proactive_decisions` in `~/.human/memory.db` (daemon-authored decision
  log; contains a `contact` TEXT column and a `message_ref` TEXT column — the latter
  is a ≤64-byte prefix of a draft/sent message, never the full body, per the repo
  header's own contract) + `~/Library/Messages/chat.db`, opened `mode=ro` (already
  the case), for reply-timing only.
- The committed evidence JSON carries **only** counts, rates, CI-free rate
  denominators, timestamps, and window parameters — no contact identifiers, no
  message text. Verified by reading `main()`'s `result` dict: it has no per-contact
  breakdown and never touches `message_ref` or the unused `contact_ref()` helper
  (defined at `eval_when_to_speak.py:62`, not called anywhere in the file today —
  pre-existing, not part of this story's fix, noted for completeness only).
- No new field this design adds (`dropped_pre_send`, `send_failed`, `comparison`)
  introduces contact-level or message-level data — all are aggregate counts/rates.
- This design did **not** read the raw text of any `message_ref` or `contact` value
  from `~/.human/memory.db`; only `COUNT`/`MIN`/`MAX`/`GROUP BY` aggregates were run
  (see Verified findings §1), per the dispatch instruction.

## Measurement and refusal conditions

- MIR and FIR each refuse independently is **not** how the script works today — it
  refuses the whole run if *either* `mir.n < min_n` or `fir.n < min_n` (single
  combined check, `eval_when_to_speak.py`'s `main()`). This design does not change
  that: AC-4.2 only requires that *when* the refusal fires, the story records the
  exact counts, which the script's own stderr message already contains
  (`REFUSE: insufficient n (MIR n=..., FIR n=..., min_n=...)`). No behavior change
  needed for the refusal path itself — capture its stderr verbatim into the evidence
  file/story notes.
- `--min-n` is **not** lowered to manufacture a number (AC-4.2, and
  `.claude/rules/no-number-without-a-measurement.md`). If FIR refuses under the
  post-dedup pool (2 confirmed sends, expected per finding 1), that refusal — not a
  synthetic FIR — is the story's committed result for FIR. MIR may independently
  clear or miss `min_n`; both are captured in one invocation regardless.
- `decisions_source` must read `proactive_decisions` (AC-4.1) — already true given
  87 rows in the default 90-day window; this is asserted by the hermetic test, not
  just implied by the resolved-data check.

## SHADOW policy shape

This story ships no new subsystem and flips nothing — there is no C-level
`OFF→SHADOW→LIVE` gate to design here (`.claude/rules/feature-gate-requires-measurement.md`
applies to subsystems that change what gets sent; this story changes nothing about
what the daemon sends). "SHADOW" for US-4 means: the committed MIR/FIR/comparison
numbers are diagnostic evidence only, read by humans and by future sprint planning —
no code path branches on them. If a future story uses these numbers to justify a
policy change (e.g., loosening the proactive gate because MIR is high), **that**
story is the one that must ship its own env-gated `OFF→SHADOW→LIVE` contract per the
existing rule; this story's output is an input to that decision, not an activation
of it.

## Risk analysis

- No `:8741` / service-loop restart (AC-4.6) — this story only reads two local
  sqlite files read-only and writes one JSON file; nothing touches the running
  daemon or model server.
- No second model loader — no LLM calls anywhere in this story.
- No private text committed — see Data provenance/privacy above.
- No ratchet movement: zero `src/*.c` files touched. `src/daemon.c` is currently
  exactly at its `MAX_BASELINE` ceiling (12313 LOC, verified in
  `scripts/check-file-size-ceiling.sh`) — this story adds no lines there, consistent
  with the "no growth" constraint.
- Residual risk (documented, not fixed here): the `(contact, ts)` join is a
  practical key, not a schema-enforced one (finding 2); and the older Initiative
  Layer send path bypasses `proactive_decisions` entirely (finding 3), so FIR/MIR
  computed here describe only the "unified" proactive path's behavior. This is
  disclosed in the evidence JSON's own `"decisions_source"` / a new
  `"known_limitations"` string array field (two fixed strings, no computation) so a
  reader of the committed artifact sees the caveat without re-deriving it from this
  design doc.

## Test seam

Hermetic, in `scripts/test_eval_when_to_speak.py` (new), building temp SQLite files
under `tempfile.TemporaryDirectory()` — no real `~/.human` or `~/Library/Messages`
paths touched, following `scripts/test_eval_persona_evolution.py`'s pattern:

1. **Source-line assertion**: synthetic `memory.db` with ≥1 `proactive_decisions` row
   in-window → `decisions_source == "proactive_decisions"`.
2. **Fallback-still-works assertion**: `proactive_decisions` table absent (or present
   but empty in-window) + synthetic `proactive_sends`/`production_outcomes` rows →
   `decisions_source == "fallback"` (regression guard: don't break the existing path
   while adding the join fix).
3. **Join/dedup — confirmed send**: two synthetic rows, same `(contact, ts)`, one
   `trigger='init_proposer_llm'/decision=send/sent=0`, one
   `trigger='proactive_send'/decision=send/sent=1` → resolves to exactly ONE eligible
   FIR event (not two), `resolved_sent=True`.
4. **Join/dedup — failed send**: same shape but the outcome row is
   `trigger='proactive_send'/decision=decline/reason=send_failed/sent=0` → the pair
   resolves to zero FIR-eligible events and increments `fir.send_failed`, not
   `fir.dropped_pre_send`.
5. **Join/dedup — fired-and-dropped**: only the proposal row exists (no outcome row
   at that `(contact, ts)`) with `decision=send` → zero FIR-eligible events,
   increments `fir.dropped_pre_send`.
6. **MIR unaffected by duplicates**: a duplicate-row scenario (as in case 3) still
   yields `mir.missed` computed correctly via presence-check, proving the `any()`
   contract holds after the SELECT gains `trigger`.
7. **Refusal path**: synthetic data below `--min-n` → exit code 2, stderr contains
   `REFUSE: insufficient n`, and **no** output file is written
   (`os.path.exists(out_path)` asserted False) — the non-vacuous, file-not-written
   half of AC-4.2 (`.claude/rules/no-number-without-a-measurement.md`: refuse means
   writing nothing, not writing a zero).
8. **`--compare-baseline` three states**: (a) flag omitted → `comparison.available
   == False`; (b) flag points at a valid US-3 JSON → `comparison.available == True`
   with the correct verdict string for a fabricated FIR-vs-baseline pair in both
   directions; (c) flag points at a missing/unreadable path → `comparison.available
   == False`, reason names the missing path, script does NOT crash.
9. **`FIR_WINDOW_HOURS` import**: `from eval_when_to_speak import FIR_WINDOW_HOURS`
   succeeds and equals `24.0`, and the argparse default for `--fir-window-hours`
   equals the same constant (no drift between the two).

All 9 are pure-Python, no subprocess, no live daemon — matches the story's own
"hermetic test builds a temp SQLite db" plan, extended to cover the join logic this
design adds.

## Conflicts

- **US-3 shares `scripts/eval_when_to_speak.py`.** US-3's AC-3.1 requires importing
  `FIR_WINDOW_HOURS` from this file rather than re-deriving it — verified that name
  does not exist yet (module-scope) anywhere in the script. This design adds it (see
  Approach §2) as part of US-4's edit, so US-3's implementer only ever *imports*, never
  *edits*, `eval_when_to_speak.py` — avoiding both stories editing the same file's
  same region concurrently. Sequencing recommendation: land US-4's
  `FIR_WINDOW_HOURS` constant + argparse rewire FIRST (it's a 2-line, behavior-neutral
  change), then US-3 can proceed independently of the rest of US-4's join-fix landing.
- **`src/daemon.c` is at its file-size ratchet ceiling** (12313 LOC == current
  `MAX_BASELINE`, verified). This story touches zero C files, so it neither risks nor
  needs to touch the ratchet. Called out only because finding 3 (the older Initiative
  Layer path) would be tempting to "just fix" by having it also log to
  `proactive_decisions` — that is explicitly OUT OF SCOPE (AC-4.5, and it would add
  lines to a file already at ceiling).
- No other sprint story touches `proactive_decisions_repo_sqlite.c`,
  `init_proposer.c`, or `daemon_proactive.c` this sprint (grepped `stories.md` for
  those three names outside US-4's own section — no other matches).

## Out of scope

- Any change to daemon proactive decision logic, the reply-delay model's `off`
  state, or any LIVE flip of anything (AC-4.5).
- Consolidating the two proactive-send code paths (finding 3) so the older
  Initiative Layer path also writes to `proactive_decisions` — real gap, real
  C-level fix, not measurement-only; flagged for a future story/chip, not attempted
  here.
- Verifying whether the older path (`daemon.c:12119`) is actually live in the
  current `~/.human/config.json` (`initiative.target_handle` set + `dry_run=false`)
  — this design deliberately did not read that file's live values; a future chip
  should check this before trusting MIR as a complete picture of daemon-side
  proactive coverage.
- Querying `chat.db` to pre-compute whether MIR will independently clear `--min-n`
  — left to the actual script run at execution time, per AC-4.2's own contract.
- `:8741` / service-loop restart or repoint (AC-4.6).

## Estimate

**S.** ~50–70 line diff to one existing Python file (join/dedup helper + constant +
one new CLI flag + output fields), one new hermetic test file (~9 cases, all
in-process sqlite), one evidence JSON (or refusal record). No new C files, no build,
no daemon restart. Slightly above the skeleton's original S-scope because the join
fix (Approach §1) turned out to be necessary for AC-4.3's numbers to be honest rather
than merely present — see Verified findings §2 for why "run the script as-is" was not
sufficient.
