---
title: "Skills Pack — Memory Craft + Epistemic Hygiene (12 new skills)"
created: 2026-05-10
status: deferred
parent: 2026-05-10-memory-roadmap-overview.md
risk: low
scope: skill-registry/, human-skills/
last_audit: 2026-05-25
---

# Skills Pack — Memory Craft + Epistemic Hygiene

## Goal

Add 12 new skills to `skill-registry/skills/` covering the gaps the existing 125 skills don't address: the craft of operating a long-term memory honestly, and the epistemic hygiene that makes memory trustworthy. Pure markdown PR — zero code risk, ships in parallel with W1.

## Motivation

Out of 125 existing skills, the cluster missing is the one that makes h-uman's memory + graph stack actually useful: how to curate a knowledge graph, how to forget on purpose, how to resolve contradictions, how to evaluate a source, how to surface uncertainty honestly. Skills are the agent's playbooks at decision points; without them, the bitemporal graph (W1), the multi-graph topology (W3), and the agent-writable persona (W5) won't get exercised the right way.

These twelve are the result of mapping the W1-W6 capabilities back to the cognitive operations they require.

## Catalog

### Memory craft (6 skills)

| Skill | One-line trigger | Connects to |
|-------|------------------|-------------|
| `knowledge-graph-curation` | When extracting entities or relations, before writing | W1 conflict resolver, W3 cross-edges |
| `memory-conflict-resolution` | When new fact contradicts old | W1 conflict resolver, W2 LLM resolver |
| `memory-hygiene` | When old memories accumulate or stop being relevant | W2 AutoDream pruning, W4 erasure |
| `case-based-reasoning` | When planning a task that resembles past tasks | W3 case-based-recall |
| `temporal-reasoning` | When user asks about time-windowed facts | W1 bitemporal queries |
| `community-summarization` | When user asks a "global" question | W2 community summaries |

### Epistemic hygiene (6 skills)

| Skill | One-line trigger | Connects to |
|-------|------------------|-------------|
| `source-criticism` | Before writing a fact extracted from web/email/tool output | W1 write-trust score |
| `self-correction-from-memory` | Before responding when a past mistake was recorded | W4 inline verifier |
| `confidence-calibration` | When asserting a fact | W4 hedging, W1 confidence field |
| `privacy-stewardship` | When deciding what to write down | W4 erasure, W1 quarantine |
| `memory-poisoning-watch` | When extraction comes from indirect-prompt source | W6 red-team, W1 trust score |
| `narrative-stitching` | When asked to summarize a multi-channel period | W2 life chapters, W3 cross-graph |

## Skill format

Each follows the existing `skill-registry/skills/<name>/SKILL.md` format that `human-skills` already indexes. Required frontmatter:

```yaml
---
name: <kebab-case-id>
description: >-
  <triggering description, like the using-superpowers skill>
metadata:
  category: memory-craft | epistemic-hygiene
  surfaces: [agent, daemon, autodream, planner, response]
  related_skills: [...]
---
```

Body sections (per existing convention):

- **When to use** — explicit trigger conditions
- **Procedure** — numbered steps
- **Anti-patterns** — what people get wrong
- **Connects to** — which W1–W6 modules call this skill
- **Examples** — concrete before / after

## File map

12 new directories under `skill-registry/skills/`:

```
skill-registry/skills/knowledge-graph-curation/SKILL.md
skill-registry/skills/memory-conflict-resolution/SKILL.md
skill-registry/skills/memory-hygiene/SKILL.md
skill-registry/skills/case-based-reasoning/SKILL.md
skill-registry/skills/temporal-reasoning/SKILL.md
skill-registry/skills/community-summarization/SKILL.md
skill-registry/skills/source-criticism/SKILL.md
skill-registry/skills/self-correction-from-memory/SKILL.md
skill-registry/skills/confidence-calibration/SKILL.md
skill-registry/skills/privacy-stewardship/SKILL.md
skill-registry/skills/memory-poisoning-watch/SKILL.md
skill-registry/skills/narrative-stitching/SKILL.md
```

Plus regenerated `human-skills/INDEX.md` (per `scripts/doc-fleet.sh`).

## Test strategy

- `scripts/doc-fleet.sh` validates frontmatter, terminology, links.
- `tests/test_skills_index.c` (existing) confirms each new skill is discoverable.
- Each skill's `Connects to` section is verified by grep against the relevant W1–W6 module file paths.

## Success criteria

- All 12 SKILL.md files validate per `scripts/doc-fleet.sh`.
- `human-skills` index regenerated and committed.
- Total skill count: 125 → 137.
- Each skill cites the specific W1–W6 spec file it operationalizes.

## Risks

| Risk | Mitigation |
|------|------------|
| Skills become wishlists, not playbooks | Each skill must include a "Procedure" section with numbered steps and at least one before/after example |
| Skills duplicate existing ones | Cross-reference `skill-registry/skills/` before adding; if overlap > 50%, extend existing skill instead |
| Frontmatter drift | `scripts/doc-fleet.sh` enforces; pre-commit hook should run it on `skill-registry/` changes |

## Open questions

1. Should the skills pack land before W1 implementation, in parallel, or after? Recommendation: parallel — skills pack is docs-only and informs W1 reviewers.
2. Should there be a 13th skill for **memory-aware planning** distinct from `case-based-reasoning`? Recommendation: keep them merged for v1; split if W3's case-based recall surfaces ambiguity.

## References

- Existing skills: `skill-registry/skills/`
- Index regen: `scripts/doc-fleet.sh`
- Standards: `docs/standards/ai/`
