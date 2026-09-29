---
title: Item 4 — sleep-time consolidation into a Seth-wiki
status: built 2026-09-13 (deterministic v1, HU_WIKI_HEAD gated off)
date: 2026-09-06
---

> **Built 2026-09-13.** `scripts/consolidate_wiki.py` (pages + lint, pytest
> `tests/test_consolidate_wiki.py`), `src/memory/wiki_page.c` + the loader block in
> `src/agent/memory_loader.c` behind `HU_WIKI_HEAD` (`tests/test_wiki_page.c`),
> `scripts/launchd/ai.human.wiki-nightly.plist` (05:20). Deviations from the design
> below, on purpose: (1) **no model call** — the page is a deterministic select /
> dedupe / rank over the rows, so provenance and no-invention hold by construction
> and the lint is a check, not a hope; a local-GLM rewrite is a follow-up only if
> the token-Jaccard dedupe proves too weak. (2) **graph state heads are not on the
> page yet** — the live graph is too noisy for a page that is read verbatim ("user
> works_at a job with back-to-back meetings", "lives_in couch"); the *now* section
> uses the authored persona identity instead, and graph heads return once the
> state-first read path's extraction-noise ceiling is addressed. (3) `reflection_runs`
> summaries are global, not per contact, so they are not an input. Gate unchanged.

# Item 4 — sleep-time consolidation into a linted Seth-wiki

**Why.** The prompt carries ~17 KB per turn and the parts that make replies
specific (memory recall, personal model, insights, graph state) are assembled
from raw stores at request time. A nightly consolidation pass that compiles
those stores into short, linted pages — one per contact plus one for self —
lets the head read a *budgeted slice of a page* instead of a pile of rows.
Research shape: sleep-time compute (Letta), Knowledge Compounding 2604.11243
(84.6% token savings when queries cluster), Karpathy's LLM-wiki. The GPU
hours freed by the 09-04 reflection fix (~11 h/day) are the budget.

**Inputs (all local, all already populated).**
- `contact_insights` (item 3 extractor, 70 rows) and `prospective_memories`
  (item 5, 175 triggers) — per contact.
- `reflection_runs.summary` (post-09-04 runs are coherent third-person contact
  summaries).
- graph state view (`hu_graph_state_resolve`) for WORKS_AT / LIVES_IN heads
  with history.
- `evolved_opinions` ONLY after filtering out repo-ops rows (the table is
  polluted with "CI pipeline failed…" entries — see the 2026-09-06 recon).
- `seth.json` `life_events`, `style_rules` as the self page's spine.

**Output.** `~/.human/wiki/<contact_id>.md` and `~/.human/wiki/self.md`,
each ≤ 2 KB, sections: *now* (state heads), *open threads* (from prospective),
*what I remember* (insights), *register with them* (overlay + measured
style). Every line carries a provenance tag (`[ins:123]`, `[refl:…]`,
`[graph:…]`) so a wrong fact can be traced and retired.

**Lint before publish (refuse, don't fallback — no-number rule).**
- no line without provenance; no entity absent from graph/insights (no
  invention); no cloud model used; page size cap; the self page must agree
  with `seth.json` core identity (employer/city) or the run fails loudly.

**Wiring.** A new loader supplement reads the contact's page slice (budget
~1.2 KB) behind `HU_WIKI_HEAD` off|shadow|live, appended beside the insights
block. When live, the raw memory recall budget drops by the same bytes so
total prompt size goes DOWN — that is the gate.

**Job.** `scripts/consolidate_wiki.py` (local GLM), launchd
`ai.human.wiki-nightly` at 05:20 (after the 04:00 LoRA nightly, before the
06:00 turn). Exit non-zero and write nothing on any lint failure.

**Gate.** Prompt bytes per turn down (prompt_budget snapshot), specificity
(`scripts/specificity_score.py`) flat or up, LUAR twin flat, EI flat. Then
the n=40 human gate (item 8).

**Do not.** Do not read `general_lessons` (topic-count noise) or unfiltered
`opinions`; do not let the page replace the persona head; do not promote to
live on a green suite.
