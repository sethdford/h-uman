---
title: Sleep-time curator — named, source-quoted memory for every 1:1 contact
date: 2026-09-27
status: draft (sections 1–4 approved in conversation; awaiting spec review)
---

# Sleep-time curator — design

Widen the existing nightly `scripts/insight_stream.py` from the ~15 persona
contacts to **every contact Seth actually texts 1:1**. It reads chat.db, writes
**named, source-quoted** notes into `contact_insights`, and gates the new rows
separately so existing live behavior is untouched. Afterwards (step C), mirror
the notes' names into graph.db as typed entities.

## 1. Why

Measured on 40 real 1:1 moments from the last 14 days (2026-09-27, 9 distinct contacts):

| Of 40 moments | Count |
|---|---|
| contact has live `contact_insights` | 15 |
| contact has graph entities | 15 (the same contacts) |
| contact is a persona contact with ≥ 20 daemon turns | 19 |
| **contact is outside the persona file (never curated)** | **21** |

- Graph grounding found nothing on 0/40 of these moments before the
  2026-09-27 fixes (#497, #515).
- At matched length the daemon reaches 45% of Seth's specificity (2026-09-22).
- The existing curator (`insight_stream.py`, `HU_INSIGHT_STREAM=live`, +26%
  specificity offline) works. The gap is **who it covers**, not how it extracts.

This targets two of the four `better-than-human` criteria
(`docs/plans/2026-09-20-october-roadmap.md`):
- **Specificity ≥ Seth's own.** Currently unmeasured.
- **Prospective memory.** `fired=1` is 0, and dated `plan` notes feed it.

Remembering is the one axis where a machine can exceed a human, rather than
only imitate one.

Research anchor: sleep-time compute ([arXiv 2504.13171](https://arxiv.org/abs/2504.13171))
reports about 5× less test-time compute at equal accuracy, and up to 13–18% accuracy
gains, from precomputing over context before queries arrive. Memory hallucinations
originate at extraction and propagate downstream (HaluMem,
[arXiv 2511.03506](https://arxiv.org/abs/2511.03506)), hence the evidence rules in §3–4.

## 2. Decisions

| # | Decision | By |
|---|---|---|
| D1 | Approach A: widen `insight_stream.py`; then C: mirror names into graph.db | Seth |
| D2 | Coverage: everyone texted 1:1 with real back-and-forth | Seth |
| D3 | Thresholds (≥10 from them, ≥5 from Seth, 30 days) + a `never_curate` list | Seth |
| D4 | Model: local GLM on :8741. K3 measured at ~55 min/item on this Mac (K3 spec §9) | measured |

## 3. Population and privacy (approved §1)

Rebuilt every night from chat.db, opened read-only (`mode=ro`).

**Eligible when all of the following hold:**
- The chat is 1:1 (exactly one handle; no group chats).
- In the last 30 days, the contact sent ≥ 10 messages and Seth sent ≥ 5.
- The handle is `+E.164` or an email, not a short code.
- Persona contacts are always eligible, so today's coverage is a subset.

**Hard exclusions, checked before any model call:**
1. The contact is in memory.db `contact_suppressions`, the live opt-out
   table. Their existing live insights are **retired**, not just frozen.
2. The contact is in `~/.human/curator_never.json`, a user-maintained list of
   handles, missing file = empty.
3. The contact is currently held by the opt-out gate.

**Data boundaries:**
- Extraction runs on local GLM only.
- Storage is memory.db only.
- The manifest is counts only: contacts scanned, eligible, excluded by reason,
  notes written and retired, and run time. It never contains text.

## 4. Reading chat.db (approved §2)

- **Reuse existing code:** `eval_conversation_quality.attribute()` for
  per-contact timelines with message authorship, and
  `blind_ab/imessage_text.decode_attributed_body` for decoding.
- **Window:** the last 80 turns within 30 days per contact, the same budget
  as today.
- **Filtering:** tapbacks are skipped (`associated_message_type = 0`), and
  attachments become placeholders like `[photo]`.
- **Only humans are evidence.** Contact messages and Seth's real messages are
  labelled `[t1]…[tN]` and can be cited. Daemon-authored messages are labelled
  `[d1]…[dN]`: they are shown for context but can **never** be cited.
  Otherwise a detail the model confabulated in a reply would be stored as a
  memory overnight and then repeated with confidence.
- **Schedule:** the existing `ai.human.insight-nightly` slot (05:10, after the
  02–05 retrain window). The run gets a hard stop at 07:30, and contacts it
  didn't reach are processed first on the next night.

## 5. Extraction (approved §3)

Keep today's structure: the note-to-self framing, the five kinds
(fact, thread, plan, preference, inside_ref), `evidence: [tN]`, and the
3-vote verification (`VERIFY_SYSTEM`, `--consistency-k 3`).

Changes:
1. **Names keep their capitals.** Replace "Lowercase, like a note to yourself"
   with casual lowercase style except that people, places and organisations
   keep their capitals. The old rule erased exactly the signal measured on
   2026-09-22: the daemon keeps mid-sentence proper nouns lowercase about 6×
   more consistently than Seth does.
2. **Structured names on every note:**
   `"names": [{"name": str, "type": "person|place|org|event"}]`.
3. **Human-only citations.** A note is dropped if its evidence is empty or
   includes any `d` index.
4. **Deterministic "was it said?" check.** Before model verification, every
   `names[].name` must appear verbatim, **word-bounded**, in the text of the
   notes' cited messages. Failures are dropped and counted.

## 6. Gating and rollout (approved §4)

Insight retrieval is already live, so un-gated new rows would activate
silently. To prevent that:
- Curator-written rows get `source = 'curator_wide'`.
- `hu_contact_insights_render` (`src/memory/repos/contact_insights_repo_sqlite.c`)
  renders `curator_wide` rows only when `HU_INSIGHT_WIDE` allows it:
  - `off` (default): excluded.
  - `shadow`: excluded, and logs `insight_wide shadow: N bytes contact_has=…`.
  - `live`: included.
- Existing persona rows (other sources) are rendered exactly as today.

**Manifest targets (nightly, counts only):**

| Metric | Now | Target to promote shadow → live |
|---|---|---|
| 1:1 moments whose contact has ≥ 1 live insight | 15/40 | **≥ 30/40** |
| notes with ≥ 1 named entity | unmeasured | **≥ 60%** |
| names rejected by the "was it said?" check | — | reported; **> 20% of names blocks the run** |
| 3-vote verification pass rate | reported | not worse than the persona stream |
| dated `plan` notes → new `prospective_memories` rows per week | 0 | **> 0** |

**Promotion to LIVE** needs a blind A/B on the length-matched specificity
scorer (`scripts/specificity_score.py` at `f5e3d402c`, now pushed as
`claude/specificity-instrument-and-entity-casing`), with detection
non-inferior. The 0.225 detection result is from 2026-07-29 and must be
re-measured.

## 7. Step C — mirror names into the graph (after A passes)

- Each verified note's `names` are upserted as **typed** entities
  (PERSON, PLACE, ORG, EVENT; never UNKNOWN) under the contact's `+E.164`, with
  provenance `insight:<id>`. The note text becomes a `RELATED_TO` relation's
  context.
- Gate: `HU_INSIGHT_GRAPH_MIRROR` (off / shadow / live, default off).
- Ship only if the `live: injected … fallback=…` log (#517) shows graph
  grounding contributing beyond the insights block.
- Related, still unmerged: `36d663051` ("stop minting placeholder entities that
  outrank real names") on the scorer branch.

## 8. Testing

Pytest in `tests/`, alongside the existing `tests/test_insight_stream_admission.py`
and `tests/test_insight_stream_keywords.py` (17 tests, not run in CI today;
the plan adds a CI step). The clock is injected.

- **Population:**
  - Threshold boundaries: 9/10 messages from the contact, 4/5 from Seth, 30-day edge.
  - Group chats and short codes are excluded.
  - A persona contact is always eligible.
- **Exclusions:** a suppressed contact is skipped **and** has its live rows
  retired; a `curator_never` handle is skipped; a missing never-file means an
  empty list.
- **Evidence:** a note citing a `d` index is dropped; empty evidence is
  dropped; daemon text never appears in any written note's evidence.
- **Name check:**
  - Verbatim, word-bounded pass: "Priya" is found in "priya's surgery…"
    case-insensitively.
  - "Al" does **not** match inside "Also".
  - An invented name is dropped and counted.
- **Gate (C side):**
  - `hu_contact_insights_render` with `HU_INSIGHT_WIDE` off / shadow / live
    against a fixture holding one persona row and one `curator_wide` row: the
    persona row is always rendered; the `curator_wide` row only in live.
  - A before/after contract on the same rows.
- **No silent success:** a dead :8741 or 0 eligible contacts exits non-zero
  and writes nothing (`no-number-without-a-measurement.md`).
- **Integration:** a stub GLM server plus a fixture chat.db, running end to
  end in dry-run.

## 9. Open items

1. Whether the curator should also backfill `names` for today's persona notes
   (a one-time run), or only apply to new notes.
2. How long the 30-day window keeps an insight alive when a contact goes
   quiet. Today's retirement is only via supersession.
