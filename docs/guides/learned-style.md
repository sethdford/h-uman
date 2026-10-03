---
title: Learned Style Profile — nightly learner, drift check and runtime gate
created: 2026-10-01
status: operator-facing
---

# Learned Style Profile

Hand-written persona style rules ("5-15 words", "MAX 15 words", "Be brief")
are replaced by values **learned from Seth's own sent iMessages**, per contact
and per situation. They adapt over time and are self-checked every night.

Two parts share one file:

| Part | What | Where |
|---|---|---|
| A (Python) | nightly learner + drift self-check | `scripts/learned_style_profile.py`, `scripts/learned_style_drift.py`, `scripts/learned_style_nightly.sh` |
| B (C) | runtime loader, lookup, prompt line | see [Runtime (Part B)](#runtime-part-b) |

The hand-written persona JSON is **never modified**. On 2026-09-06 an
automatic style re-analysis gutted the persona, so the learned values live in
a separate file that the runtime reads on top of the persona.

## Gates

| Variable | Values | Default | Effect |
|---|---|---|---|
| `HU_LEARNED_STYLE_LEARN` | `0` or anything else | on | `0` makes the nightly wrapper a no-op. Writing the file changes nothing that is sent: only Part B reads it, behind its own gate. |
| `HU_LEARNED_STYLE` | `off` / `shadow` / `live` | `off` | Part B's runtime gate (below). |

## The file

`~/.human/personas/<persona>.learned-style.json`, mode 0600, written atomically
(temp file + rename). The persona dir is `$HU_PERSONA_DIR`, else
`$HU_STATE_DIR/personas`, else `~/.human/personas`, the same order as
`hu_persona_base_dir`. memory.db and the logs dir default under
`$HU_STATE_DIR` (or `~/.human`) too. Schema `learned-style/v2` (v2 only adds
fields to every stats node; every v1 field is computed exactly as in v1, pinned
by a golden test, so a v1 reader that ignores unknown keys keeps working):

```
{
  "schema": "learned-style/v2", "persona": "seth",
  "generated_at": "2026-10-02T05:10:00Z", "window_days": 180, "half_life_days": 21,
  "global": { stats },
  "contacts": { "<handle>": { "overall": { stats },
                              "buckets": { "shape:question": { stats }, ... } } }
}
```

`stats` holds `n`, `n_eff`, `len_p25` / `len_p50` / `len_p90` (reply length in
UTF-8 **bytes**, summed over the turn's bubbles),
`bubbles_p50`, `lower_start_rate`, `emoji_rate`, `end_punct_rate`,
`latency_p50_s` (or null) and `shrunk`, plus the [v2 behaviour
fields](#v2-behaviour-fields). Every leaf is a number, a boolean or
null, except the three metadata strings `schema`, `persona` and
`generated_at`. A test walks every leaf of a generated file, v2 fields
included, and fails on anything else.

Buckets: `shape:question|story|casual` (shape of the inbound burst being
answered, joined in time order with `\n`; Part B classifies the same joined
text). Walking back from the contact's last bubble, a bubble joins the burst
only if it is at most 10 minutes before the next bubble kept and within 6
hours of the reply; the walk stops at the first that is not, and never goes
past Seth's previous send. An unanswered "dinner sunday?" on Monday therefore
does not make Wednesday's "lol" a question. The daemon batches one poll's
consecutive messages plus one re-poll and defines no time constant, so the
10-minute gap is a review ruling, not a measured daemon value), `time:day|evening|late` (local time of the reply), `pace:rapid`
(inbound under 120 s after Seth's previous send, and a reply within 120 s).
Part B only reads the `shape:*` buckets in v1.

## Learner (Part A)

```bash
python3 scripts/learned_style_profile.py --persona seth --dry-run   # counts only, writes nothing
python3 scripts/learned_style_profile.py --persona seth             # write
python3 scripts/learned_style_profile.py --persona seth --no-cap    # reseed, skip the change cap
```

Overrides (used by the tests): `--persona-dir`, `--out-dir`, `--chat-db`,
`--memory-db`, `--log-dir`, `--now`, `--tz utc`. `--max-ambiguous-frac`
(default 0.05) sets the attribution refusal below. `--extra-history <jsonl>`
adds older history ([below](#older-history---extra-history)).

**What is a sample.** A reply *turn* is one or more consecutive `is_from_me`
bubbles, each within 90 s of the one before. It is a sample when the message
immediately before it is the contact's, sent at most 6 hours earlier, in a
1:1 chat with a persona contact. That inbound message is the one it answers.
A Seth follow-up with no inbound message in between is not a reply and is
skipped. Length is the sum of the bubbles' UTF-8 bytes; attachment-only turns
are skipped.

**Only Seth's own texts.** chat.db marks the daemon's sends `is_from_me` too.
Attribution reuses `eval_conversation_quality.attribute()` (memory.db
`outbound_sends` provenance plus the assistant-row match). A turn with any
h-uman or ambiguous bubble is dropped. Learning from the twin's own sends
would feed its habits back into its style. For that reason memory.db is
required: if it cannot be read, the learner refuses. Owner self-test handles
(persona contact `relationship: "test"`) are excluded too.

**The attribution's blind spot, and its guard.** Before the first
`outbound_sends` record, a send with no assistant row within 15 minutes is
labelled Seth's; after it, any send provenance does not claim is labelled
Seth's. A twin reply from a path that writes neither would therefore be
learned as Seth's, and the drift check would not see it, because it uses the
same attribution. Every run logs `ambiguous_n`, `sent_n`, `ambiguous_frac`,
`huuman_n` and `exact_unmatched` (outbound_sends records that never resolved
to a delivered message), and the learner refuses when `ambiguous_frac`
exceeds `--max-ambiguous-frac`. `ambiguous` (h-uman was active nearby but the
delivered text matches nothing it logged) is the observable symptom of that
leak; a rising `exact_unmatched` says provenance and chat.db have drifted
apart.

**Privacy.** Message text exists only in memory inside
`samples_from_timeline()`, where it is reduced to numbers. Nothing with text
is written, logged or printed. The run log and stdout carry counts only, never
a handle.

**Shape rule** (identical in Part B, same test vectors in both suites),
applied to the joined inbound burst. Trim C whitespace, then: `question` if the text contains `?`; else `story` if it is
at least 140 bytes, or at least 80 bytes with at least 2 runs of `.`/`!`;
else `casual`. Empty or missing text is `casual`.

**Weighting and shrinkage.** Recency weight `0.5 ** (age_days / 21)` over a
180-day window; `n_eff` is the sum of weights. Quantiles are weighted (the
smallest value whose cumulative weight reaches q). Every value field is
shrunk toward its parent, `(n_eff * v + 8 * parent) / (n_eff + 8)`: bucket →
contact overall (itself shrunk) → global. `n` and `n_eff` are never shrunk.
`shrunk` is true on every contact and bucket entry (the formula always
applies) and false on `global`.

**Thresholds.** A bucket with `n < 3` is omitted; a contact with `n < 5` is
omitted, and Part B falls back to global for them.

### Guardrails

- **Per-run change cap.** Each value may move at most `max(30% of the previous
  value, an absolute floor)` per run, compared field by field with the same
  node in the previous file. The floor is 10 bytes for lengths (contract),
  0.05 for rates, 0.5 for `bubbles_p50` and 60 s for latency. The last three
  are not in the contract: without a floor, a rate that was 0.0 could never
  move again. A clamped value is counted in the log line (`clamped_n`,
  `clamped_fields`). There is no cap on the first run or with `--no-cap`.
  After clamping the quantiles are kept ordered (p25 ≤ p50 ≤ p90) by lowering
  the higher one, never by raising p50 or p90 past their own cap. With an
  ordered previous file the clamped values are already ordered and every
  field is within its cap (tested over 2,000 random cases). Ruling: only for
  an unordered (hand-edited or corrupt) previous file can no assignment meet
  both, and then ordering wins over the lower quantile's downward cap.
- **History.** Before each write the previous file is copied to
  `~/.human/personas/learned-style-history/<persona>.<UTC stamp>.json` (0600);
  the newest 14 are kept.
- **Run log.** One JSON line per run in `~/.human/logs/learned-style.jsonl`:
  `status`, `samples`, `contacts`, `contacts_omitted`, `buckets`, `global_n`,
  `clamped_n`, `clamped_fields`, `max_rel_change` (largest pre-clamp relative
  move), `first_run`, `shape_changed_by_burst_n` (replies whose shape from
  the last inbound bubble alone differs from the burst's shape: how much the
  burst rule moves bucketing), and the attribution counts above. Counts only.
- **Refusal (exit 2, nothing written, previous file untouched).** More than
  `--max-ambiguous-frac` (5%) of sends ambiguously attributed; global
  `n < 50`; more than 50% of the previous file's contacts would disappear;
  persona file, chat.db, memory.db or the `--extra-history` file unreadable. A refusal still appends its
  log line (not under `--dry-run`).
- **`--dry-run`** prints one counts-only JSON line, writes nothing (no file,
  history or log), and exits 2 when a real run would refuse.

## v2 behaviour fields

v1 learns how long Seth's replies are. v2 learns the rest of how he texts, per
contact and situation, so runtime consumers can replace static constants
(director reply delay, tapback bands, double-text and proactive cadence) with
his own observed ranges. Code: `scripts/learned_style_v2.py`. Every stats node
(global, contact `overall`, every bucket) carries these fields. Each group has
its own `n` / `n_eff` where its sample differs from the reply samples, and is
shrunk toward its parent with that group's own `n_eff` (same formula, K = 8).
A group with no data at a level takes its parent's value and reports `n = 0`;
consumers must read the group's `n`.

| Field(s) | Sample (`n` key) | Definition |
|---|---|---|
| `latency_p25_s`, `latency_p75_s`, `latency_p90_s` | reply samples (`n`) | Weighted quantiles of seconds from the contact's last bubble to Seth's first reply bubble. Gaps over the 6 h pairing window are not replies. With v1's `latency_p50_s` they condition on the `time:*` buckets like every field. |
| `bubbles_p90` | reply samples (`n`) | Weighted p90 of bubbles per reply turn (v1 has p50). |
| `inter_bubble_gap_s_p50` | `inter_bubble_gap_n` | Median seconds between consecutive bubbles of a turn (turns with ≥ 2 bubbles). |
| `double_text_rate` | `double_text_n` | Share of Seth's reply turns followed by another Seth turn (> 90 s later, so not a bubble) within **2 h** (inclusive) with no message from the contact in between. Unknown (excluded) when the follow-up is not attributed to Seth, or the 2 h have not passed yet. Disjoint from initiation by construction: 2 h < the 6 h thread gap, so an outbound after a long silence is a thread start, never a double text. |
| `double_text_gap_s_p50` | `double_text_gap_n` | Median gap of those double texts. |
| `tapback_only_rate`, `tapback_with_text_rate` | `tapback_n` | Share of Seth's responses to an inbound burst (every from-me event after the burst, before the contact's next message, within 6 h) that were only a tapback (types 2000–2006, 2006 = custom emoji) on their message, or a tapback plus text/media. Every 2000–3999 row (tapbacks, 2007 stickers, 3xxx removals) is a reaction row: it never opens or answers a burst and is never a sent message; only 2000–2006 are learned as tapbacks. |
| `tapback_types{love,like,dislike,laugh,emphasize,question,emoji}`, `self_reaction_rate` | `reaction_n` | Share of Seth's tapbacks of each kind, and the share placed on his own message. |
| `voice_memo_rate`, `gif_rate`, `share_rate` | `modality_n` | Share of Seth's message responses (tapback-only responses excluded) containing an audio message (`is_audio_message` or an `audio/*` attachment), a GIF (`image/gif` or a GIF balloon), or a link/media share (URL, URL balloon, any other attachment). |
| `initiation_rate_per_week`, `initiation_share` | `initiation_n` | Thread starts: the first message after ≥ 6 h of silence either way. Rate = recency-weighted Seth starts per week of observed time (the exposure is the recency-weighted length of each observed span, so a 3-month gap between corpus and chat.db is not counted as silence). Share = Seth starts / (Seth + contact starts). Contact `overall` and `time:*` buckets only (a start answers nothing, so it has no shape or pace); absent from `shape:*` and `pace:*`. |

**Twin contamination.** Text and media sends use the v1 attribution labels, so
an h-uman or ambiguous send excludes its unit.

*Tapback provenance.* The learner reads memory.db `outbound_sends` rows with
`kind = 'tapback'`. A row may claim a from-me tapback for that contact above its
`prior_max_rowid` boundary, dated from 5 minutes before the record to 30 s after
it (one-sided: the record is written once delivery is confirmed, so the bot's
row precedes it; a tapback well after the record is a later one). Claiming is
one-to-one, nearest in time first: each tapback is claimed by at most one row
and each row claims at most one tapback, so two quick reacts claim two distinct
tapbacks. A row with no boundary (`prior_max_rowid` -1 or NULL: a group-chat
target, or chat.db unreadable at send time) claims nothing, since by time alone
it could take Seth's own tapback. A response unit holding a claimed tapback is
the bot's and leaves the tapback sample. Text attribution ignores `tapback`
rows (with no text they would otherwise claim the nearest Seth text). The run
log carries `tapback_provenance_rows_n`, `tapback_provenance_no_boundary_n`,
`tapback_provenance_excluded_n` and `tapback_provenance_excluded_share`
(aggregate counts only). The daemon writes these rows once PR #611
(`feat/tapback-provenance`) lands; before that the counts are 0 and the
fallback below does the work. **Known gap:** a tapback tier that reports
failure but whose tapback actually landed writes no row, so that tapback is
still attributed to Seth (the daemon-activity window below is the only guard
for it).

Until PR #611 lands the twin's **tapbacks write no provenance** (`src/daemon.c`
tapback-only path: no `outbound_sends` row, no assistant row), so a from-me
tapback cannot be attributed directly. The daemon
does save every inbound batch it handles as memory.db `messages` rows, so a
response unit with any daemon trace for that contact (messages of any role,
`proactive_sends`, `outbound_sends`) within 15 minutes of the burst or the
response is left out of the tapback sample. The exclusion applies to text and
tapback responses alike; dropping only the tapback ones would bias
`tapback_only_rate` down. The run log counts the exclusions
(`tapback_daemon_near_n`). Expect that count to be large while h-uman serves a
contact: the tapback fields then come mostly from periods the daemon was not
handling that contact.

**Per-run cap.** The v1 cap (30% relative) applies to every v2 value with these
floors: latency 60 s, `bubbles_p90` 0.5, `inter_bubble_gap_s_p50` 10 s,
`double_text_gap_s_p50` 600 s, `initiation_rate_per_week` 0.25, every rate and
`tapback_types` share 0.05. `n`/`n_eff` fields are counts and never capped. A
v1 file is still read as the previous file (cap and history); its nodes have
no v2 fields, so the first v2 run is uncapped for them. New quantiles are kept
ordered around v1's (`latency_p25 ≤ p50 ≤ p75 ≤ p90`, `bubbles_p90 ≥ p50`)
without ever moving a v1 field.

**Bounded to the global prior.** After shrinkage every contact node (`overall`
and buckets) is bounded to the global value: probabilities (`double_text_rate`,
`tapback_*_rate`, `self_reaction_rate`, the modality rates,
`initiation_share`) move at most 25% relative or 0.05 absolute, whichever is
larger; times (`latency_p25/p75/p90_s`, `inter_bubble_gap_s_p50`,
`double_text_gap_s_p50`) at most 25% relative; `bubbles_p90` and
`initiation_rate_per_week` 25% relative with the per-run floors (0.5, 0.25).
Latency and bubble quantiles are then re-ordered around v1's unbounded
`latency_p50_s` / `bubbles_p50`; ordering wins over the bound
(`prior_order_overridden_n`). `tapback_types` is a distribution: a contact's
mix is pulled toward the global mix until their total-variation distance is
≤ 0.25, then renormalised so it sums to 1 (global too).

*Bound vs per-run cap.* After the per-run cap the bound is re-applied to the
capped global, restricted to each value's night-to-night interval (mixes: TV
≤ 0.1 from the previous night's mix, which replaces the per-share cap), so the
written file satisfies both. Only when the previous night itself lies outside
the bound can the two not meet; then the per-run cap wins and
`prior_bound_overridden_n` counts it. `max_rel_change` stays the largest
pre-clamp move; `max_rel_change_written` is the largest move actually
written. Run log also: `prior_clamped_n`, `mix_capped_n`, `post_cap_*`.

**Provenance block.** The file carries `provenance`: `schema`,
`generated_at`, `window` (`start`, `end`, `days`, `half_life_days`) and
`sources` (chat.db and `--extra-history` sample counts, overlap rows dropped,
contacts, global n, response/tapback units, tapback provenance rows and
exclusions, initiation starts). Counts and fixed-format UTC timestamps only;
pinned by `tests/fixtures/learned_style_v2_provenance_golden.json`
(regenerate with `UPDATE_GOLDEN=1` only for an intended change).

**Run-log counts (also printed by `--dry-run`).** `response_units_n`,
`tapback_units_n`, `tapback_daemon_near_n`, `reactions_n`, `modality_n`,
`double_text_n`, `initiation_starts_n`, `initiation_unknown_n` (from-me starts
not attributed to Seth), and the `extra_*` counts below. Numbers and booleans
only.

### Older history (`--extra-history`)

`~/.human/training-data/m3-corpus.jsonl` (written by
`scripts/m3_extract_corpus.py`; keys `channel`, `content`, `handle`, `role`,
`ts_ms`) holds real history older than chat.db's retention. What its roles
mean, from the generator:

- `imessage` / `user`: the contact.
- `imessage` / `assistant`: **any** `is_from_me` row. That includes the twin's
  own iMessage sends, so these rows are **not** all Seth's.
- `memory_db` / `assistant` (`daemon` since 2026-09-03): the twin's replies.
  Never learned from; used only as attribution evidence.

Handles are `sha256(handle)[:8]`; the learner maps persona contacts the same
way. Each corpus send is attributed exactly like a chat.db send
(`_label_send`), against the live memory.db assistant rows plus the corpus's
own `memory_db` twin rows (their `ts_ms` was parsed from a UTC string as local
time by the generator; the learner undoes that). Only `seth` sends are
learned. If the corpus's ambiguous share exceeds `--max-ambiguous-frac`, the
whole corpus is dropped for that run (`extra_refused_ambiguous: true`) and the
chat.db profile is written alone. Also dropped and counted: tapback-as-text
rows (`Loved “…”`, `extra_reaction_rows_dropped`), rows for non-persona or
empty handles (`extra_unmapped_dropped`), rows at or after chat.db's earliest
message (`extra_overlap_dropped`), and rows outside the 180-day window
(`extra_out_of_window`). The corpus's observed segment (initiation exposure
and the double-text censoring horizon) ends at chat.db's earliest message,
`chat_min_t`, even when the corpus runs past it, so the overlapping period is
counted once.

**chat.db-only fields.** The corpus has no tapbacks, attachments or chat ids,
so `tapback_*`, `tapback_types`, `self_reaction_rate`, `voice_memo_rate`,
`gif_rate` and `share_rate` come from chat.db only. The corpus feeds the v1
fields, the latency/bubble/gap quantiles, double texting and initiation.

Known limits of the corpus: content was PII-redacted by the generator
(`[phone]`, `[email]`, `[number]`), which shifts those replies' byte lengths;
group-chat inbound rows carry the sender's handle and cannot be told apart
from 1:1 rows; and with the 21-day half-life a row 140 days old weighs about
1% of a fresh one, so the corpus mostly matters for `n`, for contacts with
little recent data, and for initiation. The nightly wrapper does not pass
`--extra-history`; add it there only after a `--dry-run` on real data.

## Drift self-check (Part A)

```bash
python3 scripts/learned_style_drift.py --persona seth
```

Compares h-uman's own outbound reply lengths with Seth's, per contact:

- h-uman: memory.db `outbound_sends`, channel `imessage`, kind `text`/`reply`,
  last 30 days (`--days`). Only `length(cast(text as blob))` is selected; the
  text never reaches Python. Sends within 90 s are summed into one turn, the
  learner's own turn rule, so both sides measure the same unit.
- Seth: the learner's samples (same attribution, pairing and 180-day window),
  unweighted.

Per contact: two-sample KS statistic, both medians and their ratio
(h-uman / Seth), both n. A contact is **flagged** when both sides have
`n >= 20` and `KS > 0.35`. Report: `~/.human/logs/learned-style-drift-YYYYMMDD.json`
(0600), contacts as ordinals `c1, c2, ...`, never handles. Exit 0 no flags,
1 at least one flag, 2 a database was unreadable (nothing written). Flagging
only reports; it changes nothing.

## Operations

### Nightly (not installed by this PR)

`scripts/learned_style_nightly.sh` runs the learner then the drift check
(both always run), and is a no-op under `HU_LEARNED_STYLE_LEARN=0`. Exit:
the learner's code if non-zero, else the drift check's.

The existing 05:10 orchestrator is `~/Library/LaunchAgents/ai.human.insight-nightly.plist`
(the sleep-time curator chain). It is not tracked in the repo, so this PR does
not edit it. To install, the lead adds one step to its `ProgramArguments`
command, **after** `--retire-superseded` and **before** `--names`, joined with
`;` on both sides:

```
… insight_stream.py --retire-superseded --consistency-k 3; /bin/bash scripts/learned_style_nightly.sh; /opt/homebrew/bin/python3 scripts/insight_stream.py --names …
```

Why there: the step takes seconds and needs no model; the wide pass after
`--names` runs until its 07:30 deadline, so nothing placed after it would run.
With `;`, a learner refusal never blocks the curator steps. Back up the plist,
then `launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/ai.human.insight-nightly.plist`
and `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/ai.human.insight-nightly.plist`.

The launchd python must be able to read chat.db (Full Disk Access). Check the
first night: `tail -1 ~/.human/logs/learned-style.jsonl` must show
`"status": "written"`. A `refused_db_unreadable` line means the interpreter
lacks access.

### First run

1. `--dry-run` on real data; check `samples`, `contacts`, `global_n` and that
   no refusal flag is set.
2. Write once. There is no cap on the first run.
3. Read the drift report the next morning.

### Rollback

- Stop learning: `HU_LEARNED_STYLE_LEARN=0` in the plist environment, or remove
  the wrapper step from the plist.
- Restore a previous profile:
  `cp ~/.human/personas/learned-style-history/seth.<stamp>.json ~/.human/personas/seth.learned-style.json`
- Remove the profile entirely: delete `~/.human/personas/seth.learned-style.json`.
  Part B treats a missing file as absent, and the next run is a first run
  (no cap).
- Reseed after a deliberate change (e.g. the persona's contact list was cut by
  more than half, which the learner refuses): move the old file into the
  history directory, then run without it, or run with `--no-cap`.

### Tests

```bash
python3 -m pytest -q tests/test_learned_style_profile.py tests/test_learned_style_v2.py tests/test_learned_style_drift.py
```

`tests/test_learned_style_v2.py` pins the v1 fields against a golden file
produced by the v1 learner (`tests/fixtures/learned_style_v1_golden.json`).

Hermetic: synthetic chat.db / memory.db built in a temp dir, HOME pointed at
the temp dir, a fake interpreter for the wrapper. They run in CI in the
`capability-gate-check` job's pytest step.

## Runtime (Part B)

**Schema compatibility.** The learner now writes `learned-style/v2`. Every v1
field is unchanged, so the runtime loader should accept both
`learned-style/v1` and `learned-style/v2` (a prefix check on
`learned-style/v`). A loader that requires the exact string
`learned-style/v1` treats a v2 file as absent.

Unit note for the rendered line: "characters" in it (for example "usually
about 25 characters") are the learned `len_*` values, which are **bytes**. An
emoji counts as 4, an accented letter as 2.

<!-- Part B (C runtime: loader, lookup, prompt line, HU_LEARNED_STYLE gate,
     SHADOW→LIVE measurement and rollback) is documented here by its PR. -->

_To be written by the Part B PR._
