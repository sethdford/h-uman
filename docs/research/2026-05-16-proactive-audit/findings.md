---
title: Findings
date: 2026-05-16
status: audit
---

# Proactive Messaging Incident — Unified Audit Findings
**Date:** 2026-05-16
**Severity:** P0 — confirmed family-recipient data leak; 21 broken outbound messages shipped
**Status:** Daemon stopped, LaunchAgent unloaded, fleet audit complete, fix work pending

## 1. Incident Summary

Between an unknown earlier date and ~20:28 PT on 2026-05-16, the h-uman proactive
messaging system shipped **21 broken text messages** to three family members:

| Recipient        | Send count | Worst messages                                                                                          |
| ---              | ---        | ---                                                                                                     |
| Mindy Ford Gibson| 10         | F25 confession-parrot, full multi-line debug-buffer dump, "how'd it go with the loan?" ×4 back-to-back  |
| Betty Ford       | 7          | F25 confession-parrot (identical 60-char fragment), "funny looking dogs" ×3                             |
| Annie Ford       | 4          | F25 confession-parrot (identical to Mindy's), Replay MCP repeats                                        |

The byte-identical confession fragment reaching all three recipients confirms a
cross-contact data leak — not three independent triggers.

## 2. Severity Counts (de-duped across 4 audits)

| Audit                  | CRITICAL | HIGH | MED | LOW |
| ---                    | ---      | ---  | --- | --- |
| Proactive paths        | 5        | 7    | 6   | 2   |
| Conversational quality | 3        | 3    | 1   | 0   |
| Memory integrity       | 3        | 4    | 3   | 2   |
| Cross-contact privacy  | 3        | 5    | 2   | 0   |
| **Total (de-duped)**   | **12**   | **16** | **10** | **4** |

Privacy audit returned `RESULT_security-reviewer=BLOCKED`. This is a hard
blocker for re-enabling the daemon.

## 3. Root Architectural Finding

This is not "a handful of bugs". The codebase was built with implicit
single-user-single-conversation assumptions, and contact scoping was bolted on
inconsistently. **Eight distinct tables have no `contact_id` column at all**
(goals, life_threads, held_contradictions, life_narration_events, hula_tasks,
prompt_patches, learning_signals, ab_selections). Their content is queried
globally and injected into whatever contact's prompt is currently active.

Layered on top of that, there are **three distinct outbound paths** with wildly
different quality:

| Path | Where                              | LLM? | Persona? | Channel overlay? | Status |
| ---  | ---                                | ---  | ---      | ---              | ---    |
| A    | agent_stream.c (reactive convo)    | ✓    | ✓        | ✓                | ok     |
| B    | daemon_proactive.c (proactive LLM) | ✓    | partial  | ✗                | leaks  |
| C    | F25/F30/humanness (raw snprintf)   | ✗    | ✗        | ✗                | broken |

All 21 broken messages came from Path C. Path B has data-source contamination
from the same memory tables Path A uses safely.

A fourth layer of failure: **memory degradation is intentionally applied to
raw stored text** ([memory_degradation.c]) — designed to mimic human
forgetting, but actively corrupting the very strings about to be shipped
verbatim.

## 4. Unified Prioritized Backlog

Fix order is dictated by blast radius, not severity. Phase 1 ships first
because it makes re-enabling safe even if subsequent fixes have bugs.

### Phase 1 — Stop the bleeding (BEFORE re-enable)

| ID | Severity | File:Line | Fix |
| --- | --- | --- | --- |
| P1-1 | CRITICAL | persona schema | Add `proactive.master_enabled` global kill switch; default OFF; daemon refuses to send if false |
| P1-2 | CRITICAL | [daemon.c:975-984](../../../src/daemon.c) | Remove all 3 fallback contact-match branches; strict `contact_id == contact_id` only |
| P1-3 | CRITICAL | [daemon.c:1008](../../../src/daemon.c) | Reject `m->topic` containing: first-person pronouns, emotion keywords, `(last:`, `\n`, format specifiers |
| P1-4 | CRITICAL | [daemon.c:7917-7925](../../../src/daemon.c) | Key `replay:latest` per-contact (`replay:<contact_id>:latest`); remove process-global static fallback |
| P1-5 | CRITICAL | [proactive.c:732,736](../../../src/agent/proactive.c) | F30 templates must go through LLM rephrasing OR have an outbound sanity filter |
| P1-6 | HIGH | [rate_limit.c:83-106](../../../src/channels/rate_limit.c) | Wire channel rate limiter into proactive send path (currently only on inbound) |
| P1-7 | HIGH | [daemon.c:1808](../../../src/daemon.c), [daemon.c:951](../../../src/daemon.c) | Replace fixed `[8]` ring buffers with heap-backed per-contact-per-date dedup |
| P1-8 | HIGH | [self_awareness.c:140-191](../../../src/context/self_awareness.c) | Wire topic-repeat suppression directive into the proactive prompt (currently dead code) |

### Phase 2 — Memory pipeline integrity

| ID | Severity | File:Line | Fix |
| --- | --- | --- | --- |
| P2-1 | CRITICAL | [daemon.c:8178-8183](../../../src/daemon.c) | Remove `combined` raw-fallback for topic; if extraction fails, use emotion keyword or skip |
| P2-2 | CRITICAL | `emotional_state.c:181-188` (src/context/ at audit time; since relocated by the DDD refactor) | Replace 60-char window with LLM-extracted noun phrase OR emotion keyword |
| P2-3 | CRITICAL | [daemon.c:9976-9984](../../../src/daemon.c) | Inner-thought store must store extracted topic, not raw 127-byte memcpy; this is a prompt-injection vector |
| P2-4 | HIGH | [proactive.c:552-574](../../../src/agent/proactive.c) | `hu_proactive_build_starter` must LLM-rephrase memory content before prompt injection |
| P2-5 | HIGH | [daemon_proactive.c:227-241](../../../src/daemon_proactive.c) | `hu_daemon_build_callback_context` must not inject raw `content` bytes |
| P2-6 | HIGH | [fact_extract.c:14-71](../../../src/memory/fact_extract.c) | Heuristic fact patterns that capture first-person confessions ("i am a", "when i'm") must store paraphrased third-person facts, not raw substrings |
| P2-7 | HIGH | [conversation.c:2420](../../../src/context/conversation.c) | Expand stopword list: add `hey, doing, you, me, my, feel, feeling, lost, lonely, sad, ok, omg`; reject single-word topics under 5 chars |
| P2-8 | HIGH | [humanness.c:437-439](../../../src/humanness.c) | Curiosity template ("How is the %s going?") must take a structured noun phrase, not raw substring |
| P2-9 | MED | [superhuman.c:957 vs :1807](../../../src/memory/superhuman.c), [emotional_moments.c:27 vs :209](../../../src/memory/emotional_moments.c) | Remove duplicate function definitions; collapse to single impl |
| P2-10 | MED | [sqlite.c:186](../../../src/memory/engines/sqlite.c) vs `emotional_state.c:126` (src/context/ at audit time; since relocated) | Resolve `mood_log` schema conflict (silently broken right now) |
| P2-11 | MED | [memory_degradation.c] | Stop applying degradation to content that is later injected into outbound prompts; degradation is a UX-of-recall concept, not a content-corruption tool |

### Phase 3 — Contact scoping (every table)

| ID | Severity | Table / File:Line | Fix |
| --- | --- | --- | --- |
| P3-1 | CRITICAL | `goals` ([goals.c:44](../../../src/agent/goals.c)) | Add `contact_id` column + WHERE clause to all queries |
| P3-2 | CRITICAL | `life_threads` ([authentic.c:251](../../../src/context/authentic.c)) | Same |
| P3-3 | CRITICAL | `held_contradictions` ([authentic.c:622](../../../src/context/authentic.c)) | Same; add UNIQUE(contact_id, topic) |
| P3-4 | HIGH | `life_narration_events` ([authentic.c:726](../../../src/context/authentic.c)) | Add `source_contact_id`; filter unsent-events by target contact |
| P3-5 | HIGH | `scheduler_jobs` ([scheduler.c:451-456](../../../src/agent/scheduler.c)) | Schema has contact_id but dispatch SELECT doesn't bind it; fix the WHERE clause |
| P3-6 | HIGH | `prompt_patches` + `learning_signals` + `strategy_weights` ([self_improve.c:57](../../../src/intelligence/self_improve.c)) | Add contact_id OR mark global-by-design explicitly |
| P3-7 | HIGH | `hula_tasks` ([task_store.c:35](../../../src/agent/task_store.c)) | Add contact_id + WHERE clause |
| P3-8 | HIGH | world_model contact lookup ([world_model.c:1251](../../../src/agent/world_model.c)) | Remove case-insensitive display-name fallback; contact_id is the only allowed key |
| P3-9 | MED | `ab_selections` ([ab_response.c:63](../../../src/agent/ab_response.c)) | Add contact_id |
| P3-10 | HIGH | [daemon.c:1338](../../../src/daemon.c), [proactive.c:772](../../../src/agent/proactive.c) | Move contact_id from application-code filter to SQL WHERE clause (prefix-match risk) |

### Phase 4 — Repeat / dedup / rate-limit (close the spam loop)

| ID | Severity | Table | Fix |
| --- | --- | --- | --- |
| P4-1 | CRITICAL | `topic_baselines` ([sqlite.c:128](../../../src/memory/engines/sqlite.c)) | Add `last_checkin_sent_at`; gate F30 SELECT by it |
| P4-2 | HIGH | `micro_moments` | Add UNIQUE(contact_id, fact) + `last_surfaced` |
| P4-3 | HIGH | `growth` events | Add `celebrated_at` |
| P4-4 | HIGH | `delayed_followup` ([proactive.c:784](../../../src/agent/proactive.c)) | Mark consumed only on confirmed send, not on retrieve |
| P4-5 | MED | `inside_jokes`, `avoidance_patterns` | Add `last_surfaced`; suppress if used in last N days |
| P4-6 | HIGH | per-contact daily/weekly send cap | Max N proactive sends per contact per 24h, hard cap |

### Phase 5 — Hygiene / GC (long-term)

| ID | Severity | Table | Fix |
| --- | --- | --- | --- |
| P5-1 | HIGH | `topic_baselines` | DELETE WHERE last_mentioned < now-365d AND mention_count<3 |
| P5-2 | MED | `mood_log`, `contact_style_evolution`, `temporal_events`, `pattern_observations` | Retention policy on each |

### Phase 6 — Conversational quality (the "human-level" gap)

| ID | Severity | File | Fix |
| --- | --- | --- | --- |
| P6-1 | HIGH | [daemon_proactive.c:275](../../../src/daemon_proactive.c) | Apply channel overlay (formality, length, emoji_usage) to proactive prompts |
| P6-2 | HIGH | [daemon_proactive.c:275](../../../src/daemon_proactive.c) | Include `relationship_type` and `dunbar_layer` in proactive prompts (currently invisible to model) |
| P6-3 | HIGH | [daemon.c:1219-1236](../../../src/daemon.c) | Check emotional tone of last received message before firing proactive trigger |
| P6-4 | MED | [humanness.c:268-284](../../../src/humanness.c) | Replace "I'm here." / "I hear you." literals with LLM call routed through persona |
| P6-5 | HIGH | [agent_stream.c:563](../../../src/agent/agent_stream.c) | Move absolute-rules block to a shared module so proactive path inherits it |

## 5. Fleet Implementation Plan

**Worktree strategy:** one worktree per phase. Phase 1 must complete before
Phase 2 starts because Phase 1 is what lets us re-enable the daemon safely.
Phases 2–4 can fan out in parallel after Phase 1 lands.

**Per-task gate:** failing regression test first (TDD), implementation, `/verify`,
critic review, merge to parent worktree.

**Hard rule (project):** never delete a worktree before merging
(`.claude/rules/worktree-merge-before-cleanup.md`).

**Re-enable criteria:**
- Phase 1 + Phase 3 all green
- Full suite (10,000+ tests, 0 ASan)
- Critic pass with no CRITICAL findings outstanding
- `proactive.master_enabled` explicitly flipped to true by user

## 6. Files Touched (high level)

- `src/daemon.c` (heavy — most proactive paths)
- `src/daemon_proactive.c` (overlay + relationship_type injection)
- `src/agent/proactive.c`, `src/agent/proactive_ext.c`
- `src/agent/goals.c`, `src/agent/scheduler.c`, `src/agent/task_store.c`, `src/agent/world_model.c`, `src/agent/ab_response.c`
- `src/context/emotional_state.c`, `src/context/authentic.c`, `src/context/conversation.c`, `src/context/self_awareness.c`
- `src/memory/superhuman.c`, `src/memory/emotional_moments.c`, `src/memory/engines/sqlite.c`, `src/memory/fact_extract.c`
- `src/intelligence/self_improve.c`, `src/intelligence/online_learning.c`
- `src/humanness.c`
- `include/human/persona.h` (master_enabled flag)
- `tests/test_*.c` (regression tests pinning each bug)
