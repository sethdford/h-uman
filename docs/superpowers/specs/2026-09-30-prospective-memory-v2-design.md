---
title: Prospective memory v2 — one typed intention store, fire-time check, time cues
status: draft
date: 2026-09-30
---

# Prospective memory v2 — one typed intention store, fire-time check, time cues

## 1. Problem (measured 2026-09-30, read-only aggregates, origin/main b622a96d4)

h-uman texts as Seth, and one "better than human" criterion is keeping promises
and following up (prospective-memory F1 > 0.65). Today:

| Measurement | Value |
|---|---|
| `prospective_memories` rows | 1,932, all `trigger_type='keyword'`, 9 contacts |
| status | 349 open, **19 fired** (9 intentions), 1,163 pruned, **401 expired unfired** |
| fire log | 9 `[prospective] fired` lines, 09-21 → 09-29 (fixed 09-20 by #419/#431) |
| cued but not fired (nightly eval) | 11 of 20 cued rows |
| `commitments` | 169 pending, **0 ever followed up**; 22 with deadlines, all past due |
| `delayed_followups` | 10 rows, all past due, all unsent |
| `prospective_tasks` | 0 rows; schema only, nothing reads or writes it |

Code facts:

- **Writer:** the nightly curator `insight_stream.py --prospective` (`prospective_pass`,
  `:504`). It inserts one row per cue keyword (`:563`) and expires rows (`:574`).
- **Firer:** reactive path only, `hu_prospective_check_triggers` (`prospective.c:29`),
  from `daemon_reactive_prompt.c:1230`.
  - It matches whole words, renders `[PROSPECTIVE MEMORY: Remember to: …]`, and
    **marks `fired=1` at render time, before the reply exists** (`prospective.c:220`).
  - There is no fire-time check: an intention the thread already resolved is still
    injected.
- **Time path:** commitments and delayed follow-ups live in other tables, read by the
  proactive tick (`daemon.c:1073`, `:1524`).
  - `hu_superhuman_commitment_list_due` takes the 3 oldest due commitments globally
    (`LIMIT`, `superhuman.c:241`) before filtering by contact, so a few stale rows
    block everyone.
  - Nothing has ever been followed up.

## 2. State of the art (June–Sept 2026)

- **PM-Bench** (arXiv 2607.12385): Set-F1 over a scripted virtual week with time,
  event and channel cues, cancellations and reschedules. Best agent 65.1%. Recall and
  false alarms trade off.
- **TriggerBench** (2606.23459): the "always-remind" failure. 98% on positives, 44% on
  silent negatives where the risk is already resolved.
- **Remember, Verify, or Ask?** (2608.19564): score actions, not stated intent. Policy
  prompts cut wrongful persistence from 0.243 to 0.100.
- **PIS, typed intention store** (2609.01272): Form → Revise → Filter (rule and clock
  checks in code) → Decide (a judge on event cues, done-marking behind guards).
  PM-Bench Set-F1 **82.9%**, against 46–58% for RAG, Mem0 and Letta. A small model
  goes from 4% to 66%.

## 3. Goal and success criteria

One typed intention store with a PIS-style fire-time check. Keyword and time cues
fire at the right moment and never for a resolved intention. Shipped
OFF → SHADOW → LIVE.

- **Harness (no raters):** `scripts/pm_bench_local.py`, a scripted multi-day scenario
  set against the pure functions.
  - Set-F1 ≥ 0.80.
  - Silent-negative false alarms ≤ 5%.
  - Cross-day miss ≤ 10%.
- **Live SHADOW (7 days)** reports:
  - `would_fire`/day and resolved-before-cue rate;
  - cue→fire rate;
  - uptake: the share of surfaced items whose delivered reply contains the action's
    key terms;
  - misses where the flow exits before the prompt builder;
  - duplicate rate.
- **Promotion to LIVE:** harness pass, plus 30 SHADOW would-fires spot-checked blind
  (via the existing rating-sheet flow) with precision ≥ 0.8.

## 4. Design

### 4.1 One typed store

Migrate `prospective_memories` by adding columns only, with no destructive change:

| Column | Values |
|---|---|
| `cue_kind` | `keyword \| time \| after_event` |
| `due_at` | due time (ms) |
| `status` | `pending \| surfaced \| done \| canceled \| expired` |
| `surfaced_at` | ms |
| `attempts` | count |
| `outcome` | `used \| ignored \| suppressed` |
| `source` | `extractor \| promise_keeper \| followup` |

- Existing `fired` values map onto `status`: `0`→pending, `1`→done, `2`→canceled,
  `3`→expired.
- Pending `commitments` with a parsed deadline, and `delayed_followups`, are mirrored
  in as `cue_kind='time'` rows by a one-time backfill plus the existing writers.
  Items overdue by more than 14 days at backfill are imported as `expired`.

### 4.2 Filter (in code)

- **keyword:** whole-word match on the inbound text, as today.
- **time:** `due_at <= now` per contact. This replaces the global `LIMIT 3` with a
  per-contact due set capped at 1 per contact per day.
- Items past `due_at + grace` (default 3 days) expire instead of firing late.
- Group chats and self-chat are never eligible.

### 4.3 Decide (fire-time check)

- For each candidate, one call to the local model (GLM on :8741, thinking off). It sees
  the last 20 turns plus the intention and returns one of `fire | already_resolved |
  cancel | not_now`.
- `already_resolved` and `cancel` set status and never surface. `not_now` leaves the
  item pending.
- Parse failure or model error means `not_now`, counted in the log. The system fails
  toward silence.
- The directive is softened to: "If it fits naturally, you could bring up: …".
- **Done only after evidence.** Mark `surfaced` (and `surfaced_at`) at render time. Set
  `done` only after delivery, when the reply's content contains the action's key terms.
  Otherwise the item returns to pending, with at most 2 attempts before `expired`.
- **v1 has no "ask Seth to clarify" branch.** A message sent as Seth cannot hedge, so
  unclear cases stay silent.

### 4.4 Gates

- **`HU_PROSPECTIVE=off|shadow|live`** for the reactive keyword path, default **off**.
  - **off:** today's behavior, byte-identical: fire on match, mark `fired=1`.
  - **shadow:** today's behavior unchanged, plus the new Filter and Decide run and log
    counts only (`prospective shadow: candidates=%d fire=%d resolved=%d cancel=%d
    not_now=%d`).
  - **live:** the new path replaces the old.
- **`HU_PROSPECTIVE_TIME=off|shadow|live`** for time-cued follow-ups that *initiate* a
  message, default off. It stays in shadow until the reactive path has passed
  promotion. SHADOW logs counts of the would-send decisions.
- Both gates log one line on first run when off (silent-config rule).

### 4.5 Local harness (`scripts/pm_bench_local.py` + C unit tests)

- A scripted scenario set with a fixture DB and an injectable clock.
- For each scenario: a clean positive, an overloaded positive, a silent negative
  (resolved earlier in the thread), a cancellation, a reschedule, and a cross-day
  intention.
- It runs the pure C functions through a CLI probe (`human prospective probe --full`,
  same pattern as `memory ground --full`), with the Decide call replaced by a scripted
  judge in tests and the local model in the harness.
- It reports Set-F1, false alarms per step, cross-day miss and update miss, counts only,
  written 0600 under `~/.human/logs/`.

## 5. Out of scope

- Channel cues (calendar or email) and the clarify branch.
- Changing the curator's extraction prompt, beyond writing `cue_kind`/`due_at` when a
  date is present.
- Dedupe of paraphrased intentions, which is its own follow-up.

## 6. Rollout

1. Merge with both gates OFF. Deploy.
2. Run the backfill with a backup, and the harness.
3. Set `HU_PROSPECTIVE=shadow` for 7 days and read the SHADOW metrics.
4. Spot-check 30 would-fires. Set `HU_PROSPECTIVE=live` only if §3 passes.
5. Then `HU_PROSPECTIVE_TIME=shadow` for 7 days, with the same pattern before live.
