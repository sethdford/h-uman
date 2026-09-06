# Persona evolution: event-induced register shifts (gap #9)

Source: `docs/research/2026-09-02-august-2026-sota-gap-analysis.md`, gap #9 —
"model event-induced register shifts with BFI-Adapt as the check. Gate:
directional fidelity on the events that actually happened to Seth this
summer."

Tooling: `scripts/eval_persona_evolution.py` (read-only against
`~/Library/Messages/chat.db`, `mode=ro&immutable=1`, never writes message
text anywhere). Tests: `scripts/test_eval_persona_evolution.py` (hermetic,
synthetic strings only, no chat.db).

## 1. What "the events that actually happened to Seth this summer" are

Two events, sourced from `~/.human/personas/seth.json` and its dated
backups (all under `~/.human/personas/`, never committed to this repo):

| Event | Best-supported date | Confidence | Citations |
|---|---|---|---|
| Move to St. Petersburg, FL | **2026-07-01** | LOW — not pinned to ±7 days | `seth.json:70` core.identity ("...before moving to Florida"); `seth.json:47-50` `life_events[0]` (`"moved to the waterfront place in st pete"`, `as_of: 2026-07-28`, `state: completed`); `seth.json.bak-fact-refresh2-20260726-045551` (04:55:51 on 07-26) — first snapshot naming Florida ("as of July 2026"); `seth.json.bak-fact-refresh-20260726-044442` (04:44:42, 11 min earlier, same day) — still no Florida/St. Petersburg mention |
| Employer change: Vanguard → Raymond James | **2026-07-26** | LOW — not pinned to ±7 days | `seth.json:70` ("...Chief Architect at Raymond James...started summer 2026, after leaving Vanguard"); `seth.json:51-54` `life_events[1]` (`"left vanguard for raymond james"`, `as_of: 2026-07-28`, `state: completed`); `seth.json.bak-fact-refresh2-20260726-045551` (04:55:51) — "Recently left ... Vanguard (summer 2026) and is in transition to a new role", Raymond James **not yet named**; `seth.json.bak.pre-typing-quirks-removal-20260726-081742` (08:17:42, ~3h20m later, same day) — Raymond James now named |

**Why these are not ±7-day dates.** The only real-world anchors in the
persona text are "summer 2026" and "as of July 2026" — month-to-quarter
windows, not days. The more specific dates above (2026-07-01, 2026-07-26)
are the moments the persona's *own fact record* flipped from the pre-event
to the post-event description — evidence of when the assistant found out
and updated the JSON, not evidence of when the event happened to Seth. Per
the task's instruction, we used the best-supported single-day point
estimate and flagged the low confidence explicitly (`EVENTS[...]["confidence"]`
and `["note"]` in the script; also carried into every JSON report it
prints). The job-change date is the tighter of the two: the persona's
record flip is bounded to a 3h20m window on 2026-07-26, versus a
~1-month bound for the move.

## 2. Measurement

Nine per-message axes, each a pure function of one message string
(`scripts/eval_persona_evolution.py`, testable with synthetic strings):

| Axis | Function | Definition |
|---|---|---|
| `length_chars` | `msg_length_chars` | character length |
| `lowercase_start_rate` | `starts_lowercase` | first alphabetic char is lowercase (messages with no alphabetic char excluded from the rate, not counted as 0) |
| `no_terminal_punct_rate` | `terminal_punctuation` | fraction whose final significant char has no `.?!…` |
| `question_rate` | `terminal_punctuation` | fraction ending in `?` |
| `exclamation_rate` | `terminal_punctuation` | fraction ending in `!` |
| `emoji_rate` | `has_emoji` | fraction containing ≥1 emoji (Unicode category `So` or U+1F000–U+1FAFF, matching `scripts/persona_style_card.py`'s heuristic) |
| `formality_contractions_per_100_words` | `contractions_per_100_words` | count of `\w+['’](t\|re\|ve\|ll\|d\|s\|m)` tokens per 100 words (matches both the ASCII apostrophe and the U+2019 curly apostrophe iOS autocorrect actually emits — a real 977-message sample was 142 curly vs. 18 straight; the ASCII-only regex undercounted contractions ~8x during development, see the script's `_APOS_CHARS` comment and `test_contractions_per_100_words_curly_apostrophe`) |
| `formality_first_person_plural_per_100_words` | `first_person_plural_per_100_words` | count of `we/us/our/ours/ourselves` tokens per 100 words |
| `warmth_hits_per_100_words` | `warmth_hits_per_100_words` | count of hits against a fixed 14-word lexicon (`WARMTH_LEXICON` in the script: love, miss, thank, thanks, appreciate, proud, glad, sweet, dear, hug, xoxo, grateful, care, sorry) per 100 words |

Each axis is reported as `(mean, 95% bootstrap CI, n)` per window
(`bootstrap_ci`: 2000 resamples, fixed seed=42, percentile method — no
scipy dependency). `delta_report` computes `post.mean - pre.mean` per axis
and a `moved_beyond_ci` flag (post mean falls outside the pre window's own
CI, or vice versa) — a conservative, dependency-free "did this move more
than the window's own sampling noise" heuristic, not a formal hypothesis
test.

### Refusal contract

If either window (pre or post) for an event has `n < min_n` (default 100),
that event's entry is marked `status: INSUFFICIENT_DATA` with the exact
counts and reason, `overall_status` becomes `INSUFFICIENT_DATA`, the
process exits 1, and **nothing is written to `--out`**, regardless of
whether other events or `--full-range` succeeded. This follows
`.claude/rules/no-number-without-a-measurement.md`: a mean computed from an
empty or near-empty window is not a weak measurement, it is not a
measurement.

## 3. What we actually got: chat.db retention makes the before/after gate unmeasurable today

Running the tool for real (`docs/plans/2026-09-02-persona-evolution/results-2026-09-02.json`,
`_meta` block) surfaced a hard blocker: **`~/Library/Messages/chat.db` on
this machine retains outbound (`is_from_me=1`) messages only from
2026-08-03 onward** (977 total messages, all in 2026-08 and 2026-09; `min(date)`
= 2026-08-03T18:22:20). This is a local Messages "Keep Messages" retention
setting, not a texting-volume artifact — zero messages of any kind (not
just zero after filtering) exist before 2026-08-03.

Consequence: **both events' pre-event windows have n=0.** The move event's
post-window (2026-07-01..07-31) is also n=0 — entirely before the data
floor. The job event's post-window (2026-07-26..08-25) has n=585 (partial:
only 2026-08-03..08-24 actually has data, the rest is truncated by the same
floor), but its pre-window is n=0. Per the refusal contract, the script
correctly refused both events (exit 1, wrote nothing) rather than reporting
a mean over an empty window.

**The directional-fidelity gate cannot be measured today with this
machine's local chat.db.** This is the honest headline finding of this
work, not a tooling failure — the tool did exactly what it should: refuse
loudly instead of emitting `n=0` statistics dressed as a measurement.

What *did* succeed: `--full-range` (no before/after split, just an
aggregate over everything currently in the DB, n=977, 2026-08-03..2026-09-02)
computed cleanly. That is used in §4 below as a "what does Seth's texting
look like right now" cross-check against the persona's baked-in numbers —
it is **not** a before/after comparison, and cannot answer "is the persona
closer to pre- or post-event Seth" (that requires pre-event data this
machine does not have).

### Fixing the data gap (for whoever runs this again)

- If Messages > Settings > General > "Keep Messages" is set to "30 Days"
  (consistent with the observed floor), setting it to "1 Year" or "Forever"
  and waiting is the only way to get a real July pre-event window on this
  machine going forward.
- A machine or backup with longer retention (a laptop that syncs history
  further back, or a Time Machine snapshot of `chat.db` from before
  2026-08-03) could be pointed at with `--db`, subject to the same
  read-only contract.
- Absent either, the earliest this gate becomes measurable against a fresh
  event is once nightly logging (§5) has accumulated 30+ days of history
  *before* the next event — i.e. this is a "start logging now, gate the
  next event" tool, not a retroactive one, until a longer-retention data
  source is found.

## 3b. Second store: inventory and the re-measurement (2026-09-03)

Follow-up to §3. Every local store that might hold older Seth-authored
outbound text with timestamps was inventoried **read-only** (`mode=ro&immutable=1`
for SQLite; plain reads for JSONL; counts, key names and dates only — no
message text, handles or names were copied). Verdict per store:

| Store | Rows | Seth-authored outbound | Earliest..latest | How authorship is known (cite) | Verdict |
|---|---|---|---|---|---|
| `~/Library/Messages/chat.db` `message` | — | 977 (921 after the len≥2 frame) | 2026-08-03..2026-09-02 | `is_from_me=1`, `associated_message_type=0` | primary (§3) |
| `~/.human/memory.db` `messages` | 3,039 (user 2,384 / assistant 655) | **0** | 2026-03-11..2026-09-02 | writer is `src/daemon.c:11408-11421`: inbound batch saved as `"user"`, the daemon's generated `response` saved as `"assistant"`. Seth's own typed texts are never inserted (only writer: `src/memory/engines/sqlite.c:1358`). | rejected — daemon output |
| `memory.db` `dpo_pairs` | 1,141 (7 `source` tags) | **0** | 2026-05-10..2026-09-03 | `outbound_edit`: chosen = the *contact's* follow-up user(N+2) from `messages` (`include/human/ml/dpo_miner.h:4-6`); `auto_correction`: chosen = user correction (`src/ml/training_data_extractor.c:486-489`); `implicit_feedback`/`arena`/`generated_v2`: daemon or simulated output | rejected |
| `memory.db` `production_outcomes` | 453 | **0** | 2026-05-24..2026-09-02 | `chosen` = text the daemon sent (`message_ref` is the daemon's send) | rejected |
| `memory.db` `training_data` | 22 | unknown | 2026-04-03 | no provenance column | rejected |
| `~/.human/logs/eval-archive/ground_truth-backup-20260725-113527.jsonl` | 690 | 690 | 2026-06-25..2026-07-24 | `seth_reply` = `reply["is_from_me"]` (`scripts/extract_imessage_pairs.py:297-315`) | usable; strict subset of `training_pairs` (0 rows not in it) |
| `eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl` | 1,302 | 1,302 | 2026-06-25..2026-07-24 | final `assistant` turn = `msg["is_from_me"]` (`extract_imessage_pairs.py:256-267`) | **used** (59 rows not in the repo copy) |
| same dir, `voice_training_pairs.jsonl` | 1,266 | 1,266 | same | same script, extra len≤280 / no-URL filter (`:354-358`) | redundant subset |
| `eval-archive/finetune-backup-20260725-114042/{train,test,valid}.jsonl` | 1,963 / 134 / 134 | assistant turns | no per-record timestamp | chat-format from `prepare-finetune.py`; timestamps stripped | rejected — cannot window |
| `data/imessage/training_pairs.jsonl` (repo, gitignored `.gitignore:141`) | 1,303 | 1,303 | 2026-06-26..2026-07-26 | as above (`:256-267`); mtime 2026-07-26 10:52 | **best — used** |
| `data/imessage/ground_truth.jsonl` / `voice_training_pairs.jsonl` | 689 / 1,264 | same | 2026-06-26..2026-07-26 | subsets of `training_pairs` | redundant |
| `~/.human/training-data/m3-corpus.jsonl` | 4,910 | 2,016 (`channel=imessage`, `role=assistant`) | 2026-04-19..2026-05-19 | `role = "assistant" if is_from_me` (`scripts/m3_extract_corpus.py:208`). **Caveat:** 203 further `role=assistant` rows have `channel=memory_db` — those are the daemon's replies (`:259`), mislabelled as Seth by the file's own docstring (`:13`). **Fixed 2026-09-05:** the extractor now emits `role: "daemon"` for memory_db assistant rows, so the downstream `role == "assistant"` filters (holdout split, counterfactuals, active probe) exclude them; the on-disk 05-19 corpus is unchanged | rejected — predates both windows; mixed provenance (fixed at source) |
| `m3-corpus-train.jsonl` / `m3-holdout-prompts.jsonl` | 2,016 / 100 | `reference` field | ≤2026-05-19 | derived from the above | rejected — date |
| `training-data/m3-alpaca-dpo-*`, `m3-counterfactuals`, `m3-rewrite-pairs`, `m3-combined-dpo`, `m3-dpo-rejections-*`, `m3-outcomes`, `dpo_finetune/`, `glm-v6-pref/`, `glm-v61-pref/`, `glm-v62-sft/`, `orpo_deliberation/`, `eval-archive/binoc-dpo-candidates.jsonl` | 2–535 each | **0** | no timestamps (or `ts` of the judge run) | `prompt/chosen/rejected`: chosen is a model output or a judged variant | rejected |
| `training-data/finetune/`, `mlx-chat/`, `glm-v5-data/`, `voice-ideal/` (`{train,valid,test}.jsonl`) | 2,220 / 22,557 / 1,202 / 1,421 train rows | assistant turns are Seth (`prepare-finetune.py`, `prepare-texting-data.py:56` from `is_from_me`) | none per record | timestamps stripped at export | rejected — cannot window |
| `~/.human/voice_corpus.jsonl` | 2,257 | 2,257 | none | `WHERE is_from_me=1` (`scripts/harvest_imessage_voice.py:93`) but only `{"text"}` is written | rejected — no timestamps |
| `~/blind_ab_run/seth_voice_corpus.json`, `seth_triples*.json`, `triples_*.json` | 1,068 / 40 / 400 / 160 | `seth_reply` | none | export_seth_triples.py from chat.db | rejected — no timestamps. (`~/.human/blind_ab_run/` does not exist.) |

**Chosen second store:** `data/imessage/training_pairs.jsonl` (the repo's
gitignored 2026-07-26 export) plus the 2026-07-25 backup of the same export
under `~/.human/logs/eval-archive/`. Both were cut from a 30-day-retention
chat.db on different days, so they overlap on 1,243 `(timestamp, sha256)`
keys and each holds rows the other lacks.

### What `--source` does (`scripts/eval_persona_evolution.py`)

- Accepts the two `extract_imessage_pairs.py` shapes only; any record
  without Seth provenance (DPO `prompt/chosen/rejected`, memory.db dumps,
  a training pair whose last turn is not the `assistant` turn) raises.
- Converts the export's **local-naive** timestamps (it used
  `datetime.fromtimestamp`) to the **UTC-naive** frame the chat.db path
  already uses, *before* windowing and de-dup. Without this the same
  message would carry two timestamps 4 h apart and never de-duplicate.
- De-dup key = `(timestamp to the second, sha256(stripped text))`; chat.db
  rows win, export rows are dropped on collision, and per-source
  `rows/added/duplicates` are reported.
- Applies the export's own `MIN_REPLY_LENGTH=2` floor to chat.db rows so
  both windows share one sampling frame (56 one-character chat.db rows
  dropped). The export's *other* exclusion — a Seth message that opens a
  conversation window with no prior context is not exported
  (`extract_imessage_pairs.py:259-260`) — cannot be reproduced from
  chat.db and is carried as a caveat in the report.
- Every window now reports `coverage` (`first`, `last`, `covered_days`)
  so `n` is never read as "a full 30-day window".
- `--min-n` unchanged at 100.

Tests: 15 hermetic tests added to `scripts/test_eval_persona_evolution.py`
(synthetic JSONL fixtures under `tmp_path`, a fixed −4 h offset pinned via
`EXPORT_TZ`; 62 pass).

### Re-measurement (`results-2026-09-03.json`, `--event both`, two `--source`s)

Merge: chat.db 977 → 921 after frame; repo export 1,303 added; backup
1,302 → 59 added / 1,243 duplicates; **2,283** merged rows. Baseline
without `--source` still refuses (exit 1, n=0 pre-windows) — unchanged.

| Event | Pre window | Pre n | Pre coverage | Post window | Post n | Post coverage | Status |
|---|---|---|---|---|---|---|---|
| Move (2026-07-01) | 06-01..07-01 | **180** | 2026-06-25 13:27 → 06-30 16:44, **5.1 days** | 07-01..07-31 | 1,182 | 07-01 → 07-26, 25.6 days | OK by contract (n≥100) — **but read the coverage** |
| Job (2026-07-26) | 06-26..07-26 | **1,324** | 30.0 days | 07-26..08-25 | 555 | 07-26 → 08-24, 29.5 days, with a **hole 07-27..08-02** (13 export rows on 07-26, chat.db resumes 08-03) | OK |

Per-axis (`mean [95% CI]`, `moved_beyond_ci`):

| Axis | Move pre (n=180) | Move post (n=1,182) | Δ | moved | Job pre (n=1,324) | Job post (n=555) | Δ | moved |
|---|---|---|---|---|---|---|---|---|
| `length_chars` | 29.0 [25.2, 33.7] | 40.2 [37.6, 43.0] | +11.3 | yes | 38.8 [36.6, 41.4] | 35.6 [31.8, 39.9] | −3.2 | yes |
| `lowercase_start_rate` | 0.011 [0.000, 0.028] | 0.169 [0.148, 0.191] | +0.158 | yes | 0.152 [0.134, 0.172] | 0.064 [0.044, 0.086] | −0.088 | yes |
| `no_terminal_punct_rate` | 0.789 [0.728, 0.850] | 0.786 [0.762, 0.809] | −0.003 | no | 0.789 [0.767, 0.810] | 0.800 [0.766, 0.831] | +0.011 | no |
| `question_rate` | 0.117 [0.072, 0.167] | 0.090 [0.074, 0.107] | −0.027 | yes | 0.091 [0.076, 0.106] | 0.101 [0.076, 0.126] | +0.010 | no |
| `exclamation_rate` | 0.044 [0.017, 0.078] | 0.047 [0.035, 0.058] | +0.002 | no | 0.047 [0.035, 0.059] | 0.056 [0.038, 0.076] | +0.009 | no |
| `emoji_rate` | 0.106 [0.067, 0.156] | 0.045 [0.033, 0.057] | −0.061 | yes | 0.051 [0.040, 0.063] | 0.086 [0.063, 0.108] | +0.035 | yes |
| `formality_contractions_per_100_words` | 3.77 [2.45, 5.29] | 2.61 [2.20, 3.03] | −1.16 | yes | 2.75 [2.35, 3.17] | 2.98 [2.27, 3.83] | +0.23 | no |
| `formality_first_person_plural_per_100_words` | 0.74 [0.31, 1.29] | 0.55 [0.39, 0.72] | −0.19 | yes | 0.57 [0.42, 0.73] | 0.46 [0.24, 0.71] | −0.11 | no |
| `warmth_hits_per_100_words` | 1.29 [0.32, 2.75] | 0.85 [0.61, 1.15] | −0.43 | yes | 0.91 [0.63, 1.24] | 1.28 [0.81, 1.88] | +0.37 | yes |

### How much of this to believe

1. **The move "pre" window is 5 days, not 30.** n=180 satisfies the
   contract, but every one of those rows is from 2026-06-25..06-30 (the
   export's own retention floor). Six of nine axes "moved", which is what
   a 5-day sample compared with a 25-day sample looks like when one axis
   is bursty (next point). Treat the move row as *not measured to the
   spec's intent*; the contract passed on `n`, not on coverage. The job
   row is the one that meets the design (30.0 vs 29.5 covered days).
2. **`lowercase_start_rate` is bursty week to week inside a single store**,
   so its between-window deltas are not event signal. Weekly rate (n) from
   the merged export, then chat.db, same length frame:
   06-22: 0.000 (133) · 06-29: 0.040 (104) · 07-06: 0.090 (340) ·
   07-13: **0.256** (520) · 07-20: 0.129 (265) ‖ 08-03: 0.038 (215) ·
   08-10: 0.042 (144) · 08-17: 0.122 (152) · 08-24: 0.021 (297) ·
   08-31: **0.357** (113). A 0.00→0.26→0.02→0.36 series is a
   device/autocapitalisation effect (the persona.c comment already notes
   "the phone autocapitalizes"), not a register shift; any gate on this
   axis must first condition on sending device, which chat.db does not
   record. This also explains §4's 4%-vs-17.3% persona contradiction: both
   numbers are "true" for whichever weeks they sampled.
3. **Store confound is bounded but not zero.** The pre windows are
   export-sampled (drops window-opening Seth messages) and the job post
   window is 98% chat.db-sampled. `no_terminal_punct_rate` (0.789 / 0.789
   / 0.800) and `exclamation_rate` agree across the seam, which argues the
   two stores measure the same thing on the stable axes; the axes that
   "moved" are the volatile ones.
4. **Dates are still LOW confidence** (§1). A ±7-day error on the job date
   moves ~200 rows between windows.

**Net for gap #9:** the job event now has a real before/after pair
(1,324 / 555, both ~30 days covered). Three axes moved beyond CI there —
`length_chars` (−3.2), `emoji_rate` (+0.035), `warmth_hits` (+0.37) — and
`lowercase_start_rate` moved but is disqualified by point 2. The
directional-fidelity gate (§5) can now be run against those three axes
for the job event; it should not be run for the move event on this data.

### 2026-09-05 follow-up: coverage floor, retention roll, and the gate

**Coverage is now part of the refusal contract.** `--min-covered-days`
(default 20) refuses any window whose first-to-last present message spans
fewer days, exactly as `--min-n` refuses on count. Re-running `--event both`
with the same two `--source`s now exits 1: the move event is
`INSUFFICIENT_DATA` (`pre covered_days=5.1 < min_covered_days=20.0`), the
job event is `OK`. The move row in the table above is therefore retired,
not caveated. Four tests pin the floor; 66 pass in that file.

**chat.db retention rolled two days between runs.** On 2026-09-03 the
oldest `is_from_me=1` row was 2026-08-03; on 2026-09-05 it is 2026-08-05.
The job post window lost the 08-03..08-04 rows (555 → 522), so its hole is
now 07-27..08-04 and `length_chars` Δ moved from −3.24 to −3.02, `emoji_rate`
+0.035 → +0.039, `warmth` +0.37 → +0.42 (same signs, same `moved` flags).
`results-2026-09-03.json` is kept as the 09-03 measurement;
`results-2026-09-05.json` is the job-only re-run under the coverage floor
and is what the gate below reads. This is the concrete cost of §3's data
gap: every day without the nightly log (§5) deletes a day of the post
window too.

**The directional-fidelity gate ran** (`scripts/eval_persona_evolution_gate.py`,
verdict in `gate-2026-09-05.json`, 10 hermetic tests). It never touches the
serving port: it pairs two *existing* generation files by prompt hash.
Matched prompt set = 34 contexts present in both
`~/blind_ab_run/triples_glm_v5.json` (generated 2026-07-25 with the v5
adapter, one day before the job date, on the 160-prompt blind-A/B set) and
`~/blind_ab_run/remeasure-2026-09-04/classifier_trials_adapterbound.json`
(generated 2026-09-04, first run with the v6 adapter actually bound on
:8741). `lowercase_start_rate` is excluded by default for the reason in
point 2 above; `--min-matched` is 30 (a separate, smaller denominator than
the human windows' `min_n=100`, stated explicitly, not a lowering of it).

| Axis (human moved beyond CI, job) | Human Δ | Generated pre (n=34) | Generated post (n=34) | Gen Δ | Sign match |
|---|---|---|---|---|---|
| `length_chars` | −3.02 | 54.1 [43.9, 65.5] | 35.8 [29.5, 43.2] | −18.3 | yes |
| `emoji_rate` | +0.039 | 0.000 [0, 0] | 0.000 [0, 0] | 0.000 | **no** |
| `warmth_hits_per_100_words` | +0.42 | 2.24 [0.80, 3.92] | 3.93 [1.44, 7.12] | +1.69 | yes |

**Verdict: FAIL** (2 of 3 gated axes match; the gate requires all). The
failing axis is informative rather than noisy: the persona emitted **zero
emoji in 68 generated replies across both snapshots**, while Seth's own
post-event rate is 8.6%. That is a fixed property of the prompt/adapter,
not a directional miss, and it is also visible in `seth.json:44` ("Emoji
sparingly — maybe 1 in 8") never surviving into output. Using the older
2026-06-01 snapshot (`triples_current.json`) as the pre side gives the same
verdict and the same two matches (length −3.5, warmth +1.09, emoji 0 → 0).

Caveats on this verdict, in order of weight:
1. n=34 matched prompts; the generated CIs are wide. Only `length_chars`
   moves beyond its own CI on the generated side.
2. The "post" generations are the v6 adapter that the 2026-09-04
   re-measurement found writes 86% lowercase / 0% punctuation, i.e. a model
   whose register is already known to be off on other axes.
3. The two generation files are different harness paths (blind-A/B triples
   vs classifier trials) that share prompts, not one script run twice.

### 2026-09-05 (later): the emoji gap — prompt fixed and wired, model still at zero

The gate's failing axis was investigated to root cause before anything was
edited. Three things were wrong, in layers:

1. **The prompt contradicted itself.** The production head (dumped with
   `tools/dump_prompt_head`, `HU_PERSONA_HEAD=live`, the mode the eval harness
   uses) carried six emoji directives from `~/.human/personas/seth.json`, four
   of them absolute negatives: `channel_overlays.imessage.emoji_usage: "none"`
   (set deliberately on 2026-07-28, backup `seth.json.bak.pre-emoji-none-…`),
   `anti_patterns` "NEVER … ZERO emoji", `voice_rhythm` "Emojis are RARE",
   `listening` "Words, not emojis" — against two soft "sparingly, maybe 1 in
   8" lines. Models weight absolute prohibitions over soft targets.
2. **The measured rule never reached production.** Style-card rule 2 (§7,
   "Emoji about 1 in 8 texts") is rendered inside the ABSOLUTE RULES block,
   and that block was appended only in `agent_stream.c`'s lean branch.
   Production runs the batch path with streaming off (`prompt.c` never
   appends it; `agent_turn.c`'s "guard tail" comment claimed it did). So the
   only emoji guidance any served reply ever saw was layer 1.
3. **The served adapter was trained against emoji.** In the v6 / v6.1
   preference corpora the *chosen* side has emoji in 0.5% of rows and the
   *rejected* side in 7.3% / 5.9% (`glm-v6-pref`, `glm-v61-pref`; 382 / 426
   rows). Chosen = Seth's real reply from the 2026-05/06 blind-A/B contexts,
   rejected = the gemma-era model's reply, which used emoji more than Seth
   did in that sample. ORPO learned "emoji ⇒ rejected" as a side effect.
   The SFT corpora are closer to Seth (5–7%); the export of his own texts is
   5.0% by this script's `has_emoji`, the 60-day card says 12.6%.

What was changed (commit `3754028b5` + the persona file):

- `seth.json` (backed up as `seth.json.bak.pre-emoji-measured-20260905-064428`):
  the prohibition in `anti_patterns` and the duplicate `communication_rules`
  line removed; the iMessage and unknown overlays, `emphasis_style`,
  `validation_style` and the style rule now all say the same measured thing:
  about 1 in 8 texts, a single emoji, usually at the end. The served head
  now carries exactly two emoji statements (overlay + rule 2), both "1 in 8".
- `hu_prompt_cap_with_tail` + `hu_agent_finalize_system_prompt`: every turn
  path (batch, streaming) and the dumper cap to budget keeping the guard
  tail and append the formality-aware ABSOLUTE RULES block as the final
  bytes, idempotently. 7 tests; full suite 14,270/14,270. Cost: on turns
  whose assembled prompt already exceeds 16,384 B, the cap now removes ~1.5
  KB more middle context to make room for the rules.

**Measured after the fix** (36 fresh trials on :8741, adapter bound, new
head, `gate-2026-09-05-post-prompt-fix.json`): emoji rate **0/36**, unchanged.
Gate still FAIL (length matches, emoji and now warmth do not; warmth's
generated Δ flipped sign on a 34-prompt sample with a CI spanning zero).
The served replies were 92% lowercase-start / 97% no terminal punctuation,
the v6 register collapse already on record.

Every generation snapshot on disk — base GLM-Air without an adapter (160),
v5 (160), gemma-v5 (130), v6 bound (36 + 36) — has an emoji rate of exactly
0.0. The daemon's own sent replies (memory.db) went 4.5% (May, n=200) →
0.3% (July, n=316) → 0.0% (Aug–Sep, n=42). So the prompt was necessary but
not sufficient: the instruction "about 1 in 8" is read as "rarely" and the
adapter was trained to avoid emoji outright.

**Next lever (not done here):** rebuild the preference corpus so emoji is
not a chosen/rejected discriminator — either draw the chosen side from the
full `training_pairs.jsonl` export (5% emoji) or drop rejected rows whose
only register difference from chosen is an emoji — then retrain, and re-run
this gate. A decode-time injector is the wrong fix (see the humanness-
injector incident).

### 2026-09-05/06: corpus builder fixed and a candidate retrained

**Builder fix** (`6d76d9813`). `scripts/build_v6_preference_corpus.py` now
strips emoji from the rejected side of every pair whose chosen side has none
(`neutralize_emoji_pairs`), keeps chosen-side emoji (Seth's real signal),
drops a pair that would become empty or identical rather than fabricating
one, refuses in `validate()` if the rejected emoji rate still exceeds chosen
by more than 0.02, and records before/after rates in the manifest. The same
rule is importable by `scripts/rebalance_preference_corpus.py --match-emoji`
so existing corpora get it at train time; `train-glm-adapter.sh` passes it by
default (`HU_TRAIN_MATCH_EMOJI=1`). Per-source audit of the live sources: the
discriminator came entirely from `generated_v2` (27 rejected-only rows of
217); arena (1/541) and cycle-4 (0/42) were neutral.

**Why the retrain used the existing v6.1 corpus, not a fresh build.** A fresh
build today would drop the 48 human-weighted pairs (the rated sheet has 0
rated rows — it is the unrated v6 sheet) and add ~385 arena rows (541 vs 156
in July). Training on that would change three things at once. Instead the
candidate trained on `glm-v61-pref` (463 pairs, the last human-curated
composition) with the train-time pass: casing/punctuation rebalanced to the
style card on both sides (lowercase-start 0.71 → 0.09 chosen, 0.17 → 0.09
rejected; terminal punct 0.24/0.50 → 0.18/0.18) and **25 rejected-only emoji
rows → 0** (chosen rate 0.005, rejected 0.000, 0 rows dropped).

**Retrain** (2026-09-05 08:56–09:21, `scripts/nightly-retrain.sh` candidate
stage with the new knobs `HU_RETRAIN_MLXTUNE_TRAINER=mlx_lm_lora
…MODE=orpo …BETA=0.05 HU_RETRAIN_SKIP_BASE=1`): the recipe that produced the
served adapter (`glm-v61-orpo-config.yaml`, 400 iters, lr 5e-6, beta 0.05,
rank 8, scale 2.0 verified in `adapter_config.json`). Serving was stopped for
25 minutes and restored healthy. ORPO validation margin went −0.151 → +0.039
with accuracy 0.00 → 0.82 by iteration 400 (the served v6 run never crossed
zero). Guard: REAL, 80/80 `lora_b` non-zero. Candidate:
`seth-glm-air-mlxtune-orpo-20260905-0856-20260905-085655`, **staged, not
promoted** (promotion stays `register_v6_adapter.py` + a human call).

**Measured**, both arms generated in the same offline harness (mlx_lm, fixed
prompt head, 37 prompts, temperature 0.7):

| | emoji | lowercase-start | no terminal punct | median chars |
|---|---|---|---|---|
| candidate (this retrain) | **1/37 (2.7%)** | 100% | 97% | 37 |
| serving v6 adapter, same harness | 0/37 | 100% | 95% | 69 |
| Seth, 60-day card | 12.6% | 8.6% | 81.7% | 27 |

Gate (`gate-2026-09-05-retrain-*-arm.json`, pre side = v5 07-25 generations,
35 matched prompts): candidate **FAIL** with 2/3 signs matching — length
(−, matches) and **emoji (0.000 → 0.029, matches for the first time)**;
warmth flips (2.17 → 0.29, human +0.42). Serving arm: 0/3 match.

Read this as: the corpus change removed the anti-emoji push and one emoji
appeared where none ever had, on 37 samples. That is a direction, not a
fixed gap — the positive signal in the corpus is 2 chosen rows out of 426,
and no amount of neutralising rejected rows adds more. LUAR (the promotion
metric) could not be computed: `authorship_gap.py` refused with 19 other
senders below its floor, another consequence of the chat.db retention roll.

**Two findings for whoever picks this up:**
1. The 100% lowercase-start in BOTH arms is not the adapter and not the
   prompt (the served head says "normal capitalization" three times). The
   base model with no adapter under the July harness prompt was also 100%
   lowercase (160/160), and the first-letter rebalance cannot fix chosen
   texts that are lowercase mid-sentence. Casing needs a corpus-content fix,
   not a first-letter transform.
2. The independent 2026-09-06 03:07 nightly (main checkout, SimPO stage)
   produced a real candidate too, but its base m3-outcomes training failed
   (rc=1) — see `~/.human/logs/nightly-retrain.log`.

### 2026-09-06: candidate promoted, measured on the served path

Promoted 07:45 on Seth's instruction via `scripts/m3_promote.py promote`
(live swap, `/v1/adapters/current` reports the candidate with 160 tensors
bound; authorship gate recorded as OVERRIDDEN/INCONCLUSIVE because LUAR
could not be computed), registered with parsed training evidence
(`register_v6_adapter.py`), and pinned in `~/.human/config.json` (backup
`config.json.bak-pre-promote-20260906-*`) so restarts keep it. Rollback:
`python3 scripts/m3_promote.py rollback --yes` and restore that backup.

**Served-path probe** (37 prompts through :8741, production head,
`gate-2026-09-06-promoted-served.json`):

| | before (v6, 2026-09-05) | promoted (2026-09-06) | Seth 60-day card |
|---|---|---|---|
| emoji rate | 0/36 (0%) | **7/37 (18.9%)** | 12.6% [10.5, 14.6] |
| lowercase-start | 92% | 68% | 8.6% |
| no terminal punct | 97% | 81% | 81.7% |
| median chars | 33 | 44 | 27 |

Gate: emoji (0.000 → 0.171) and warmth (+) now match the human direction;
length flips on a +0.7-char generated delta against a −3.0 human delta, so
the strict all-axes verdict is still FAIL. The offline harness had shown
only 1/37 emoji for the same adapter — the served path is the one that
counts, and the two harnesses do not agree on this axis.

Caveats: n=37, one probe; 18.9% overshoots Seth's 12.6% (upper CI 14.6);
lowercase-start is still 8x Seth's rate; no human blind-A/B rating and no
LUAR number for this adapter yet — the nightly authorship job measures it
from tonight. The running daemon keeps the previous adapter id only as a
provenance label until its next restart (the id is read at startup and
SIGHUP does not reload it); the server itself serves the new adapter now.

## 4. Persona comparison: does the prompt's style card match current (post-event) Seth?

The persona carries measured-style numbers in three places. File:line
citations:

1. `src/persona/persona.c:55-58` — a code comment: *"Rule 2 is MEASURED, not
   authored (`scripts/persona_style_card.py`, n=1488 typed msgs, 2026-07-12):
   starts_lowercase=4% (the phone autocapitalizes), no-terminal-punct=79%,
   ?-endings=9%."* These numbers do not appear in the emitted prompt text
   itself (`hu_rules_casual`, `persona.c:62-68`) — they're the justification
   for the qualitative rule "Most texts have no period... Question marks
   only when actually asking."
2. `~/.human/personas/seth.json` `style_rules` (prompt-visible, injected into
   every persona build): `:35` "Normal capitalization"; `:36` contractions
   guidance; `:37` *"...most texts have NO period... Question marks only
   when actually asking (about 1 in 12 texts)"* (8.3%); `:44` *"Emoji
   sparingly — maybe 1 in 8 messages"* (12.5%); `:117` (channel overlay)
   repeats the "1 in 12" question-rate claim.
3. `~/.human/personas/seth.style-card.json` — the raw output of
   `scripts/persona_style_card.py` (n=1739 typed + 116 reactions), not
   directly prompt-injected but the source data behind (1): `terminal_punctuation.none=0.787`,
   `case.starts_lowercase=0.173`, `question_ending_rate=0.099`,
   `exclamation_rate=0.064`, `emoji.message_rate=0.09`, `contraction_rate=0.177`,
   `length.median=27` (chars).

Measured now (`--full-range`, n=977, 2026-08-03..2026-09-02, entirely
post-both-events — see §3 caveat that we cannot split this into pre/post):

| Axis | Persona's baked-in value(s) | Measured now (mean, 95% CI, n=977) | Falls inside CI? |
|---|---|---|---|
| `no_terminal_punct_rate` | persona.c: 79%; style-card.json: 78.7% | 81.7% [79.2, 84.0] | **No** — 78.7-79% sits just below the CI floor (79.2) |
| `lowercase_start_rate` | persona.c: **4%**; style-card.json: **17.3%** | 8.6% [6.85, 10.4] | **No, on both sides** — persona.c's 4% is below the CI, style-card.json's 17.3% is above it. The two "measured" persona artifacts for this one axis disagree with each other by 4x and bracket the current measurement from opposite directions. |
| `question_rate` | seth.json: "1 in 12" = 8.3%; persona.c: 9%; style-card.json: 9.9% | 9.9% [8.09, 11.87] | **Yes**, all three |
| `emoji_rate` | seth.json: "1 in 8" = 12.5%; style-card.json: 9% | 12.6% [10.5, 14.6] | seth.json's 12.5% **yes**; style-card.json's 9% **no** (below CI floor) |
| `exclamation_rate` | style-card.json: 6.4% (no seth.json rule) | 3.9% [2.76, 5.12] | **No** — 6.4% is above the CI ceiling |
| `length_chars` | style-card.json: **median** 27 (not a mean; not directly comparable) | mean 34.0 [31.5, 36.7] | not comparable (different statistic) |
| `formality_contractions_per_100_words` | style-card.json `contraction_rate`=17.7% (fraction of *messages* with ≥1 contraction — a different unit than per-100-words) | 2.38 [1.89, 2.88] per 100 words | not directly comparable (unit mismatch); no per-100-words target exists to compare against |
| `formality_first_person_plural_per_100_words` | none authored | 0.41 [0.27, 0.58] per 100 words | new baseline, no prior claim |
| `warmth_hits_per_100_words` | none authored | 1.30 [0.95, 1.73] per 100 words | new baseline, no prior claim |

**We cannot answer "closer to pre- or post-event Seth"** for any axis — the
chat.db retention gap (§3) means no pre-event measurement exists to be
closer to. What we *can* say: on the two axes with the clearest current
disagreement (`no_terminal_punct_rate`, `lowercase_start_rate`), the
persona's baked-in numbers are already stale relative to *current* Seth,
independent of any event — `lowercase_start_rate` in particular has two
persona artifacts (persona.c's comment vs. `seth.style-card.json`) that
contradict each other 4x over, which predates and is orthogonal to the two
life events analyzed here and is worth fixing regardless of this gap's
outcome. `question_rate` is the one axis where every persona artifact and
the current measurement agree closely.

## 5. Nightly tracking design

Goal: accumulate the pre-event history this gate needs, so the *next* life
event (there will be one) is measurable, and give the style card a
by-window view instead of an all-time one.

- **What to log.** Once per night (cron/launchd, matching the existing
  `eval_fidelity_nightly` pattern), run:
  `python3 scripts/eval_persona_evolution.py --event none --full-range --window-days 30`
  and append the resulting JSON (one line, `full_range_summary` +
  `total_outbound_messages_in_range` + `range`) to
  `~/.human/logs/persona_evolution_nightly.jsonl`. This is OUTSIDE the repo
  (private texts), same convention as `seth.style-card.json`.
- **Why nightly, not on-event.** The gate needs a *pre*-event baseline that
  already exists by the time an event is noticed (events are usually
  noticed after the fact, from a persona fact-refresh edit, per §1). A
  nightly rolling log means "the 30 days before day X" is always
  answerable in arrears, instead of needing 30 days of *future* waiting
  once an event is spotted.
- **Re-deriving the style card by window, not over all time.**
  `scripts/persona_style_card.py` currently measures over a single
  `--days` lookback from "now" and overwrites `seth.style-card.json`
  wholesale. To support gap #9's gate, change the derivation to:
  1. Keep a *rolling* card (today's behavior, `--days 180` or whatever the
     current default is) for the prompt-facing style rules — this is "what
     does normal Seth sound like most of the time," which should stay
     stable and not chase every week's noise.
  2. Add a *windowed* card keyed by `(start, end)` — exactly the pre/post
     windows this script computes — written to
     `~/.human/personas/style_card_windows/<event>_<pre|post>.json`. This
     is what the directional-fidelity gate reads, not the rolling card.
  3. On a life-event add/edit to `seth.json.life_events` (i.e. whenever a
     fact-refresh touches that array), trigger a one-time computation of
     both windows for the new event from the nightly JSONL log (§ above),
     if 30 days of pre-event log history exist; otherwise record
     `status: INSUFFICIENT_DATA` the same way this script does, so the gate
     result is never silently absent — it's explicitly blocked, with a
     reason, until enough history accumulates.
- **The gate itself** ("directional fidelity"): for every axis in §2 whose
  `moved_beyond_ci` is `True` for a real event (both windows n≥100), take a
  fixed matched-prompt set (same prompts sent to the model before and after
  the event, e.g. the existing blind-A/B corpus's prompt half), generate a
  reply with the current adapter/persona for each prompt, and score the
  SAME axis on the generated replies. PASS requires the generated-reply
  delta's *sign* to match the measured human delta's sign, for every axis
  that moved beyond CI (a magnitude match is not required — this is a
  directional check, per the gap's own wording). This generation-and-score
  half is not implemented by this deliverable; it plugs into the existing
  `eval_fidelity_nightly.py` / `eval_shape_classifier.py` machinery once a
  real before/after pair exists to gate against.
- **Where nightly numbers surface.** Append `overall_status` and the
  per-event `n`s to the same day's row consumed by
  `docs/standards/quality/` dashboards or wherever `eval_fidelity_nightly`
  currently reports (`~/.human/logs/`), so a human sees "gate: blocked,
  insufficient pre-event data (n=0 for event X)" rather than the check
  simply not appearing.

## 6. What was NOT done

- The gate's second half (generate on a matched prompt set, score the same
  axis, compare delta sign) is designed in §5 but not implemented — there
  is no real before/after pair to gate against yet (§3).
- No change was made to `scripts/persona_style_card.py`, `seth.json`, or
  any persona artifact. The `lowercase_start_rate` 4%-vs-17.3% contradiction
  found in §4 is flagged, not fixed, here.
- ~~No attempt was made to extend the measurement window past what
  `~/Library/Messages/chat.db` holds locally.~~ Done 2026-09-03 (§3b):
  every local store was inventoried read-only and the two
  `extract_imessage_pairs.py` snapshots were merged via `--source`.
- `--min-n` is still 100. `--min-covered-days` (default 20) was added
  2026-09-05, which retires the move event on this data.
- The directional-fidelity gate ran 2026-09-05 on existing generation files
  only (FAIL, emoji axis). It has not been run with a fresh, single-harness
  before/after generation pass, and cannot be until a pre-event adapter is
  served again or a nightly generation log exists.

## 7. Resolution (2026-09-03): the style card is now the single source

The §4 contradiction was wider than one axis. Every style number the prompt
could see was hard-coded in at least one place, and no two places agreed:

| axis | `src/persona/persona.c` comment (07-12) | v1 card (07-26) | `seth.json` rule | measured 60d (n=977, 07-05..09-03) |
|---|---|---|---|---|
| lowercase_start | 4% | 17.3% | "All lowercase … Use normal capitalization" | **8.6%** [6.9, 10.4] |
| no_terminal_punct | 79% | 78.7% | 81% | **81.7%** [79.2, 84.0] |
| question_rate | 9% | 9.9% | "1 in 12" | **9.9%** [8.1, 11.9] |
| emoji_rate | — | 9% | "1 in 8" | **12.6%** [10.5, 14.6] |
| exclamation_rate | — | 6.4% | — | **3.9%** [2.8, 5.1] |

Nothing in C ever read the v1 card; the prompt's casual rule 2 was a
string literal. What changed:

- `scripts/measure_style_card.py` derives the card (`style-card/v2`) from a
  configurable window of the user's own outbound texts (default 60 days,
  refuses below n=300, writes nothing on refusal) using the same per-message
  axis functions as this script, and writes
  `~/.human/personas/<persona>.style-card.json` with value + 95% CI + n +
  window per axis. Hermetic tests: `scripts/test_measure_style_card.py`.
- `src/persona/style_card.c` (`include/human/persona/style_card.h`) loads
  and validates the card; `hu_persona_build_absolute_rules_fmt` now renders
  the casual rule 2 from it. The compiled default (`hu_style_card_default`,
  pinned to the 2026-09-03 measurement) is used only when the card is
  missing or invalid, and logs once naming the script that fixes it.
  Tests: `tests/test_style_card.c` (card beats default; fallback pinned).
- `scripts/persona_style_card.py` (v1 shape) now writes
  `seth.style-report.json` so it can no longer overwrite the v2 card.
- Comments in the style governor that restated the frozen 79%/9% now point
  at the card; `HU_STYLE_GOV_PERIOD_STRIP_PCT` itself is unchanged.

Still authored, not derived: the numeric claims inside
`~/.human/personas/seth.json` ("1 in 8", "81%", "1 in 12") — a private
persona file, left for the owner to trim now that rule 2 carries the
measured values.

## 8. US-7: honest event-shift comparison + windowed re-derivation (2026-09-05)

**Side-by-side, not sequential:** the two events' `lowercase_start_rate` deltas move in
*opposite* directions — move event +0.158 (95% CI-crossing, `results-2026-09-03.json`),
job event −0.088 (95% CI-crossing, `results-2026-09-05.json`). Presented without
qualification these read as one persona-wide trend; they are not comparable at equal
confidence:

- **job**: pre n=1,324 (30.0 covered days), post n=522 (29.5 covered days) — both windows
  meet `--min-covered-days=20`. This delta is real, load-bearing, and is the one the
  directional-fidelity gate ran against (status: FAIL — `length_chars` sign-matches
  (human −3.01, gen −20.41); `emoji_rate` and `warmth_hits_per_100_words` do not (human
  +0.039/+0.42, gen 0.0/−0.72); `lowercase_start_rate` itself is excluded from the gate as
  a device-autocapitalization artifact, spec §3b point 2).
- **move**: pre n=180 spans only **5.1** of an intended 30 covered days
  (`--min-covered-days=20` refuses it as of the 2026-09-05 coverage-floor fix). Its +0.158
  delta is retired, not disproven — it is simply not backed by enough spread of days to
  rule out one or two atypical days dominating the mean. Treat every number attributed to
  the move event in this document (§3b, §4) as illustrative history, not a standing
  measurement.

**Corrected nightly command.** §5 above prescribes
`eval_persona_evolution.py --event none --full-range --window-days 30` as the rolling
nightly log command. **This does not do what it says**: `--window-days` is read only by
the event pre/post bucketer, never by `--full-range`, so that command reports the entire
`--start`..`--end` range (default since 2026-03-01), not a 30-day trailing window. The
corrected, verified command is:

    python3 scripts/eval_persona_evolution.py --event none --trailing-days 30 \
        --source data/imessage/training_pairs.jsonl \
        --source ~/.human/logs/eval-archive/imessage-corpus-backup-20260725-113543/training_pairs.jsonl

which reports `trailing_window_summary.coverage.covered_days` honestly (≈30 today,
bounded by the rolling chat.db retention floor — see below) instead of silently
mislabeling the full historical range as a 30-day window. Nightly *scheduling* of this
command remains unimplemented (§5's "what to log" was designed, never cronned) and is
unchanged by this story.

**chat.db retention floor is a moving target, verified live 2026-09-05 ~16:30 local:**
`min(is_from_me=1 date)` = **2026-08-06T14:42:13** (was 2026-08-03 on 2026-09-02 per §3,
2026-08-05 on 2026-09-05 morning per the follow-up above) — the floor has advanced 3 days
in 3 days. A `--trailing-days 60` run today reports `coverage.covered_days ≈ 30.1` from
chat.db alone (measured live, aggregate-only, this run) — `requested_days` (60) and
`coverage.covered_days` (~30) are two distinct report fields, never conflated (AC-7.3;
`scripts/test_eval_persona_evolution.py::test_trailing_days_reports_requested_days_and_true_coverage_separately`
pins this). The `--source` merge extends the *pre-event* windows for the two named events,
and it extends the *trailing* window's coverage too: `run()` filters `--source` rows to the
same `start_dt`/`end_dt` used for `fetch_outbound_messages` (`start_dt` already overridden
by `--trailing-days` before the merge runs), then feeds the merged `messages` into
`trailing_window_summary` — so a source export whose timestamps predate chat.db's retention
floor raises trailing-window coverage. Measured, not asserted: the same 60-day trailing
window reports `coverage.covered_days = 30.0` from chat.db alone
(`sprints/sprint-better-than-human-2026-09-05/evidence/us7-trailing-60d-chatdb-only-2026-09-05.json`)
but **59.8** once `--source` is added
(`sprints/sprint-better-than-human-2026-09-05/evidence/us7-trailing-60d-2026-09-05.json`,
both committed as evidence) — correcting this section's earlier claim that `--source` "does
not extend the trailing window." What `--source` does *not* do is raise the chat.db-only
retention floor itself: `min(is_from_me=1 date)` keeps advancing in an unmerged, chat.db-only
run regardless of any `--source` export. Recovering more pre-August history (AC-7.6) remains
the only way to raise coverage for a run with no matching `--source` export, and is
explicitly out of scope here.
