---
title: `docs/plans/` — Plan Status Schema
status: closed
created: 2026-05-25
last_audit: 2026-05-25
---

# `docs/plans/` — Plan Status Schema

The 138-plan triage from 2026-05-25 found three structural problems:

1. **No canonical status vocabulary** — files used `status: complete`,
   `status: shipped`, `status: implemented`, `status: living`,
   `status: in-progress`, all meaning roughly "ongoing" or roughly
   "done." Future readers had to infer from content.
2. **26 of 138 files had no status frontmatter at all**, mostly recent
   (2026-05-12+) work where the author hadn't decided on a state yet.
3. **2 files were orphaned** — explicitly `status: obsolete` /
   `status: abandoned` but still in the live dir, where they
   competed for attention with the active plans.

This doc fixes (1) and (3). Per-file frontmatter normalization is
tracked as a separate cleanup PR; the canonical states below are the
schema that cleanup will apply.

## Canonical states (use ONLY these going forward)

| Status | Meaning | When to use |
|---|---|---|
| `closed` | Work shipped to main; no follow-up expected | When the plan's deliverables are merged AND the relevant tests/contracts are green AND no new sub-tasks are open. Add `last_audit: <date>` to record when this was last verified. |
| `active` | Work in flight right now | When a sprint, agent, or maintainer is actively touching the deliverables. Should have a recent commit referencing the plan within ~14 days. |
| `deferred` | Designed; explicitly on hold | When the plan is intentionally not being worked. Should name the BLOCKER and the EXPECTED unblock condition. Otherwise it's an `orphan` masquerading as `deferred`. |
| `superseded` | Replaced by a later plan | When a newer plan obsoletes this one. MUST link to the replacement in the frontmatter as `superseded_by: <path>`. |
| `archived` | Filed for posterity; no longer in scope | When the plan is interesting historical context but irrelevant to current work. Lives under `docs/plans/.archive/`. |

## NOT canonical (do not introduce)

- `complete` / `shipped` / `implemented` / `done` — all collapse to `closed`
- `in-progress` / `living` / `wip` / `draft` — all collapse to `active`
- `scoped` / `proposed` / `not-started` — all collapse to `deferred`
- `obsolete` / `abandoned` — both collapse to `archived` AND the file
  moves to `docs/plans/.archive/`

## Frontmatter shape (canonical)

```yaml
---
title: <plan title>
status: closed | active | deferred | superseded | archived
created: YYYY-MM-DD
last_audit: YYYY-MM-DD       # required for closed and active
owner: <subsystem / role>
parent: <parent plan path>   # optional — for nested plans
superseded_by: <path>        # required iff status: superseded
related:
  - <path>
---
```

## Archive policy

`docs/plans/.archive/` holds plans that are no longer in active
scope. The contents are not deleted because they document past
engineering decisions that may inform future ones. They are also
not auto-loaded by agents (every plan file in `docs/plans/` costs
context window for any agent that surveys the dir).

When you encounter a plan that should be archived:

```bash
git mv docs/plans/<old-plan>.md docs/plans/.archive/
```

…and add a one-line note to the commit message explaining why
(obsolete / superseded / abandoned / merged-into-other-plan).

## State of the audit (2026-05-25)

| State | Count | Notes |
|---|---|---|
| CLOSED | 72 | Already had `status: complete/shipped/implemented`. Normalize next PR. |
| SUPERSEDED | 6 | Already tagged. Normalize next PR. |
| DEFERRED | 12 | Already tagged in various forms. Normalize next PR. |
| ACTIVE | 7 | Already tagged. Normalize next PR. |
| UNCERTAIN | 26 | NO frontmatter — most recent work. Recommend per-file review + tag. |
| ORPHANED | 2 | **Archived this PR** — `2026-03-19-sota-agent-assessment.md` + `2026-05-11-init-13-kv-compression.md` |
| ADR | 6 | Architectural decisions in `adr/` — separate registry, no normalization needed |
| **Total** | **138** | |

## Related

- `~/.claude/rules/writing-rules.md` — the equivalent discipline for
  the `~/.claude/rules/*.md` files (audit cadence, sunset policy)
- `~/.claude/rules/audit-verify-before-allege.md` — the rule that
  prevents another false-positive audit from misclassifying plans
