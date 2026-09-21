# Better-than-human execution pass — 2026-09-20 (evening)

Executes the October roadmap (`2026-09-20-october-roadmap.md`) items that do
not depend on human raters, plus the persona-completeness gaps found by the
same-day validator audit. Scope was set autonomously from the instruction
"fix it all and make it better than human"; assumptions are listed so they can
be reversed.

## State that set the order (measured tonight, not recalled)

| Item | Measured |
|---|---|
| Wiki nightly 05:20 | exit 120, `ENOSPC` on `.self.md.tmp`; disk now 250 GB free (transient). Re-ran 19:25, exit 0, 11 pages refreshed |
| Prospective memory | `fired=0` 472, `fired=2` 1163, `fired=1` **0**; **0** open triggers had a matching inbound since the 09-19 deploy — waiting on traffic, not broken |
| Insight extractor | `source=extractor:v2:k3:a3`, 16 rows carry `evidence_ids` — v2 IS running (the morning audit line is stale) |
| `eval_when_to_speak.py` | referenced by zero plists; refuses at FIR n=10 < 30 |
| Specificity (criterion 2) | **FAIL, dated tonight**: daemon 0.973 vs Seth 2.136 specific tokens per reply (`results/specificity-2026-09-20.json`, deterministic scorer, daemon-last 300) |
| Persona `seth.json` | valid; `core.principles` empty; 21 optional sections absent, incl. every timing one (`chronotype`, `timezone`, `time_overlays`, `daily_routine`) |
| Proactive gates | `HU_PROACTIVE_CONTEXTUAL=off`, `HU_PROACTIVE_REACHABILITY=shadow` |

## Sub-projects, in execution order

**P0 Pipeline health** — done (wiki re-run). Not done: deleting the two May
`memory.db` backups (7.2 GB) — destructive, left for the operator.

**P1 Every criterion gets a dated number (O2 + O4)**
- P1a `scripts/launchd/ai.human.better-than-human-nightly.plist` runs
  `eval_when_to_speak.py` and the new `eval_prospective_memory.py` at 05:35
  (after insight 05:05 and wiki 05:20). A REFUSE is a dated number.
- P1b `specificity_score.py --out` persisted (above). The scorer reaches back
  months at `--daemon-last 300` because the daemon writes ~7 replies/day; the
  number is honest about the population it measures.
- P1c `scripts/eval_prospective_memory.py` (+ pytest): open triggers, cues
  seen since deploy, fired, precision/recall; refuses rates below `--min-n 30`
  but always prints counts. Reads `memory.db` only.
- Not done: timing column on the 0/48 refresh sheet — changing a sheet in
  flight invalidates its rows; add it to the NEXT sheet.

**P2 Persona timing sections from measurement, not invention**
`scripts/persona_timing_from_chatdb.py`: hour-of-day histogram of Seth's own
outbound texts (chat.db read-only, 180 d, local tz) → `timezone`,
`chronotype` (loader enum), `time_overlays` text quoting the measured shares,
`daily_routine` weekday/weekend blocks with `availability` from measured
share. `--write` backs up to `seth.json.bak-timing-<ts>` first; voice blocks
untouched. Gate: `human persona validate seth`. The daemon loads the persona
at start, so a kickstart is required for it to take effect.

**P3 `core.principles`** — derived from the 18 existing `values` by the local
model, written only if the ENFORCING proxy gate (`eval_blinded_ab.py`,
fail_under 45, max_regression 5) does not regress. Revert on regression and
record the number. Assumption: filling an empty field is not "another voice
round" if it is gated.

**P4 O5 contestability** — inbound opt-out phrase → `contact_suppressions`
row → daemon-side proactive pre-filter (next to the reachability filter)
skips the contact → count on doctor. Tests pin: phrase detected within one
turn; suppressed contact never proposed; non-opt-out text does not suppress.

## Left to the operator (decisions, not work)
1. **O1** — 48 ratings; nothing here refreshes the 53-day-old human verdict.
2. `HU_PROACTIVE_CONTEXTUAL` off→on and reachability shadow→live change who
   gets texted. The roadmap says "decide on purpose"; not flipped here.
3. Delete `~/.human/memory.db.pre-orphan-cleanup-*` / `.backup-pre-cleanup-*`.
