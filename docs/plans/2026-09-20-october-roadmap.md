# October 2026 roadmap — from indistinguishable to better

Successor to `2026-09-19-beyond-sota.md` (items A–K stay the program; this file
adds what that sweep did not cover and reorders around what is actually
blocking). Written 2026-09-20. Papers below were fetched and verified
2026-09-20; the A–K sweep covered Jul–Sep and is not repeated here.

## The finding that sets the order

`better-than-human` (2026-09-06) defines four criteria. **Three of the four are
mimicry-shaped** — match Seth's specificity, match his timing, be
indistinguishable — and **only one has ever been measured**:

| Criterion | State on 2026-09-20 |
|---|---|
| detection ≤ 0.35, n≥40 | **0.225 PASS** — but measured 2026-07-29, now **53 days old** |
| specificity ≥ Seth's own | last offline A/B (+26%) result file absent from `results/`; live unreadable at ~7 replies/day |
| prospective-memory F1 > 0.65 | no reading; deployed 09-19 |
| timing within rater preference | never measured |

The refresh sheet is **0/48 answered**, 5 sent, 4 rows skipped after three asks.
Items 6, 8, H and K all terminate on that sheet. **The program is not
idea-limited or compute-limited. It is limited by 48 ratings.**

Indistinguishability is, on the best-powered evidence we have, *done*: 0.225 is
below chance — raters pick Seth **less often than guessing**. Continuing to
optimise mimicry optimises a solved criterion. The axis where a machine can
genuinely *exceed* a person is **calibrated proactive reliability**: Seth
forgets his friend's interview; h-uman need not. That axis is in the codebase
and is not measured.

## State audit, 2026-09-20 (query, not changelog)

Four things recorded as deployed are not running. None of this is visible from
the plan, the commit log or merge status — only from the database.

| Recorded | Measured |
|---|---|
| Prospective memory deployed 09-19; gate `fired=1 > 0` | `fired=1` is **0** (461 unfired, 1163 pruned at `fired=2`). Gate has not cleared |
| K-verified admission + evidence provenance merged 09-19 | `source` still `extractor:v1`, **0 of 725** insights carry `evidence_ids`. The new path has not run |
| Insight nightly at 05:05 | last ran **09-19 05:10**; did not run 09-20. `wiki-nightly-error.log` written within the hour |
| `HU_PROACTIVE_CONTEXTUAL=on` (memory) | live plist reads **`off`** |

And one nobody has looked at — `proactive_decisions`, 380 rows, newest
2026-09-19 21:03:

```
decline  254   (llm_negative 219, parse_error 19, send_failed 16)
send     125   ->  sent=1 on only 10.  115 decided "send" and nothing went out
defer      1
```

**92% of proactive send-decisions never reach a human.** An open loop with no
metric on it.

## Correction, 2026-09-20 (same day, before any code changed)

Two of the five items below were misdiagnosed on first writing, and one number
quoted here was not trustworthy. All three were caught by running the queries
this document recommends. Recorded rather than silently edited, because the
*shape* of the error is the point:

1. **"115 proactive send-decisions never sent" was a row-type conflation.**
   `init_proposer_llm` rows are proposals and carry `sent=0` **by design**; only
   `trigger='proactive_send'` rows record delivery. Both call sites document
   this in comments I had not read when I wrote the claim.
2. **The FDR/MNR metric I proposed building already exists** —
   `scripts/eval_when_to_speak.py`, since 09-05, critic-reviewed, with the
   recipient-engagement ground truth I described as our unfair advantage.
3. **The only MIR/FIR reading on record (09-02: MIR 0.613 / FIR 0.670) is a
   population conflation and must not be quoted.** It ran on the `fallback`
   source, which merges `proactive_sends` (25 genuinely proactive check-ins in
   90d) with `production_outcomes` (432 rows = ALL production replies,
   reactive included) and marks every row `sent=True`. FIR over that population
   measures "of all sends, how many got no reply" — not "of proactive
   interruptions". Same shape as the `starts_lowercase` 17.3%-vs-8.9% trap.

**What is actually true about the superhuman axis today:** genuinely proactive
sends number **10** (decision log) to **25** (`proactive_sends`) over 90 days,
against the script's `min_n=30`. Run today it prints

```
REFUSE: insufficient n (MIR n=921, FIR n=10, min_n=30) — not writing a result.
resolved_events=354   fir_dropped_pre_send=89   fir_send_failed=16
```

The metric is not broken — **it correctly refuses**, which is the
`no-number-without-a-measurement` contract working. The binding constraint is
that the system barely speaks first: `HU_PROACTIVE_CONTEXTUAL=off`, and 89 of
115 fires die before reaching a channel. **You cannot measure calibration on a
system that does not act.** O3 (attributing the 89) is therefore the
prerequisite for O2, not a parallel item.

## The items

| # | Frontier (paper) | Status | What ships | Gate |
|---|---|---|---|---|
| **O1** | — (this is ours) | blocked | **Unblock the human gate.** 48 ratings, or a shortened sheet: cycle-4 showed 5/5 repeated-context rater consistency at n=40, so ~20 rows likely carries enough power to refresh a 53-day-old verdict. Nothing downstream is trustworthy until this clears | human verdict age < 45 days; doctor `blind_ab_gate` error clears |
| **O2** | **ProEvent** (2607.17701): first event-centric benchmark for tracking upcoming events *from IM chats*. LLMs systematically **over-fire** — DeepSeek-V3.2 FDR **96.5%** vs MNR **21.9%**; GPT-5.1 **26.7%** timetable accuracy; **human 94.4%** Event Success Rate | **ALREADY BUILT — and dark** | **Correction (2026-09-20, same day):** this item as first written proposed building FDR/MNR. It exists. `scripts/eval_when_to_speak.py` (Contract C5 Part B, 09-05, critic-reviewed) computes **MIR** (Missed-Intervention Rate = MNR) and **FIR** (False-Interruption Rate = FDR), against Seth's own initiation moments and the contact's actual engagement — strictly better than ProEvent's formulation, which has no real recipient. It has run **once**, on 09-02, is referenced by **zero** plists, and is on neither the nightly card nor doctor. The work is scheduling, not building | `when-to-speak-*.json` written nightly; MIR/FIR on the card |
| **O3** | — | live gap, correctly located | **Correction:** the original O3 ("115 send-decisions never sent") was a **row-type conflation**. `init_proposer_llm` rows are *proposals* and carry `sent=0` by design; only `trigger='proactive_send'` rows record delivery. The real gap is narrower and worse: of 115 FIRED proposals, **89 are `dropped_pre_send`** — killed by a validator/gate/dedup/rate-limit between `daemon.c:1573` and `:1666` — and **no row records which gate**. Those drops log to the service log and are invisible to the metric. Fix: write a `proactive_send`/`decline` row with a specific reason at each skip point | every FIRED proposal has either a delivery or an attributed decline; `fir_dropped_pre_send` → 0 |
| **O4** | **ProActor** (2605.24900, ACL 2026): timing-aware RL; proactiveness metrics quantifying timing/prediction alignment | not built | **Make criteria 2–4 measurable.** Re-run and *persist* the specificity A/B (the n=108 result file is gone); get a first prospective-F1 reading now that O2 gives the denominator; add a timing axis to the rater sheet (the 4th criterion has never been asked) | all four `better-than-human` criteria have a dated number, even a failing one |
| **O5** | **CHIIR 2026 workshop** (2608.18638): proactivity as *calibrated initiative* — when to act, what evidence justifies it, how communicated, **and how users correct, contest or refuse** | absent | **Contestability.** For a system texting real people as Seth, the recipient has no channel to decline proactive contact. The `unanswered_count` backoff governor is a one-way damper, not consent. Minimum: a recognised opt-out phrase that writes a per-contact suppression, and a nightly count of suppressed contacts | opt-out honoured within one turn; suppression count on the card |

## Order of work

1. **O1** — ask for the ratings. Everything else is downstream of a 53-day-old verdict.
2. **O3** — 115 unsent decisions, before adding proactive mechanism on top.
3. **O2** — FDR/MNR from existing rows; no instrumentation needed to start.
4. Fix the three not-running items from the audit (extractor missed 09-20;
   `evidence_ids` unwritten; `HU_PROACTIVE_CONTEXTUAL=off` — decide on purpose).
5. **O4**, then **O5**.

A–K continue underneath: C/D gates need the extractor actually running; E (insight
overuse) and F (stance) are already in shadow and need readings, not code.

## Papers outside the 09-19 sweep

Fetched 2026-09-20, not yet read in depth:

- **CloneMem** (2601.07023) — long-term memory benchmarking for *AI clones*, the closest published object to this system
- **STALE** (2605.06527) — can agents know when their memories are no longer valid; direct sibling to item D supersession
- **MemTrace** (2606.17328) — what final-accuracy metrics miss in long-term memory; relevant to every gate in this program that reads an endpoint
- **AdaMem** (2606.21144) — learning what to remember; extends item C admission
- **PersonaTree** (2606.04780) — structured lifecycle memory for person understanding
- **ProactBench** (2605.09228), **Always-On Agents survey** (2606.30306)

## Verification commands

Re-check the audit before trusting any line above — every claim here came from
these, and four of them contradicted the written record:

```bash
sqlite3 ~/.human/memory.db "SELECT fired, COUNT(*) FROM prospective_memories GROUP BY fired;"
sqlite3 ~/.human/memory.db "SELECT COUNT(*) FROM contact_insights WHERE evidence_ids IS NOT NULL;"
sqlite3 ~/.human/memory.db "SELECT DISTINCT source FROM contact_insights ORDER BY id DESC LIMIT 3;"
sqlite3 ~/.human/memory.db "SELECT decision, sent, COUNT(*) FROM proactive_decisions GROUP BY decision, sent;"
python3 -c "import plistlib,os;d=plistlib.load(open(os.path.expanduser('~/Library/LaunchAgents/ai.human.service-loop.plist'),'rb'));print({k:v for k,v in (d.get('EnvironmentVariables') or {}).items() if 'PROACT' in k or 'INSIGHT' in k})"
```

Note `proactive_decisions.ts` is **seconds**, not ms — `datetime(ts,'unixepoch')`.
Dividing by 1000 dates every row to 1970 and reads as a corrupt table.

## Do NOT

Everything on the 09-19 list still holds — another voice round, steering, Q6,
the base-swap *program* (one probe only), weight-space drift control on this
hardware. Adding for October:

- **Do not add a fifth memory mechanism before the fourth one is measured.**
  Five of eleven A–K items are memory. The specificity tell justified that; the
  gap now is readings, not mechanisms.
- **Do not promote anything on a 53-day-old human verdict.**
- **Do not treat "deployed" as "running."** Four of four checked today were not.

## Log

- 2026-09-20 — written. State audit found prospective `fired=1`=0, insight
  `evidence_ids`=0 with `source=extractor:v1`, extractor missed its 09-20 run,
  `HU_PROACTIVE_CONTEXTUAL=off` against a memory entry saying on, and 115 of 125
  proactive send-decisions never sent. ProEvent/ProActor/CHIIR fetched; the
  over-firing result (FDR 96.5% vs MNR 21.9%, human 94.4%) is what motivates O2.
