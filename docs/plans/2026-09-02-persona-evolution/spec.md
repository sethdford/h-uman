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
- No attempt was made to extend the measurement window past what
  `~/Library/Messages/chat.db` holds locally (no other backup or export was
  read; the hard no-go scoped this to the one file).
- `--min-n` was left at the task's specified 100; it was never an active
  design choice here since both events failed the gate by orders of
  magnitude (n=0), not marginally.

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
