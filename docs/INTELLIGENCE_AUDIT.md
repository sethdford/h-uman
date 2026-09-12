---
title: Intelligence/Learning Subsystem Audit
---
> **SUPERSEDED (2026-09-10).** Historical snapshot. The 2026-09-10 full code review re-checked this document: most cited `file:line` locations have moved and several findings are fixed (e.g. the intelligence cycle runs from the daemon every 6 h at `src/daemon.c`). Do not act on this file without re-verifying against the current tree.

# Intelligence/Learning Subsystem Audit

**Date:** 2026-03-15  
**Scope:** End-to-end data flow verification for all intelligence/learning paths.

---

## 1. Feedback → Reflection → Self-Improvement Path

| Step                                                         | Status      | Evidence                                                                   |
| ------------------------------------------------------------ | ----------- | -------------------------------------------------------------------------- |
| `hu_feedback_classify` called                                | **WIRED**   | `src/daemon.c:6411` (gated by `HU_ENABLE_SKILLS`)                          |
| Classified feedback → `behavioral_feedback`                  | **WIRED**   | `hu_feedback_record()` at daemon.c:6414 writes to table                    |
| `hu_reflection_weekly` reads `behavioral_feedback`           | **WIRED**   | `src/intelligence/reflection.c:243-259`                                    |
| `hu_reflection_weekly` writes `self_evaluations`             | **WIRED**   | `src/intelligence/reflection.c:299-311`                                    |
| `hu_reflection_weekly` triggered                             | **FLOWING** | Daemon: Sunday 3 AM (`daemon.c:2050-2056`)                                 |
| `hu_self_improve_apply_reflections` reads `self_evaluations` | **WIRED**   | `src/intelligence/self_improve.c:92-100`                                   |
| `apply_reflections` writes `prompt_patches`                  | **WIRED**   | `src/intelligence/self_improve.c:103-127`                                  |
| `apply_reflections` triggered                                | **BROKEN**  | Only from `cycle.c:97` — daemon **never** runs `hu_intelligence_run_cycle` |
| `hu_self_improve_get_prompt_patches` in agent_turn           | **WIRED**   | `src/agent/agent_turn.c:582-590`                                           |
| Patches injected into system prompt                          | **WIRED**   | `agent_turn.c:574-588` → `intelligence_ctx`                                |

**Verdict: BROKEN** — Daemon writes feedback → reflection → self_evaluations, but **never** runs the intelligence cycle. So `apply_reflections` never runs, `prompt_patches` are never created from reflections, and the agent never receives learned behaviors from weekly reflection. The chain only completes when `human feed learn` or `human research-agent` is run manually.

---

## 2. Online Learning → Strategy Weights → Prompt Path

| Step                                             | Status    | Evidence                                                          |
| ------------------------------------------------ | --------- | ----------------------------------------------------------------- |
| `hu_online_learning_record` called               | **WIRED** | `agent_turn.c:1713` (after tool execution), `cycle.c:158`         |
| Signals → `learning_signals` table               | **WIRED** | `online_learning.c:57-88`                                         |
| Signals → strategy weights (EMA)                 | **WIRED** | `online_learning.c:90-120` (auto-updates `strategy_weights`)      |
| `hu_online_learning_build_context` reads weights | **WIRED** | `online_learning.c:241-243`                                       |
| Context injected into agent prompt               | **WIRED** | `agent_turn.c:630-636` → `intelligence_ctx`                       |
| Tables created                                   | **WIRED** | `hu_online_learning_init_tables` — **not** called from agent_turn |

**Verdict: WIRED (with caveat)** — Flow is correct. `learning_signals` and `strategy_weights` are created by `hu_online_learning_init_tables`, which is called from `cycle.c:119` when the cycle runs. If the cycle has never run, tables may not exist; `build_context` would fail and return empty. Strategy names come from tool names (`tool:shell`, etc.) and cycle-injected names (`research_findings`, `feed_monitoring`).

---

## 3. Tool Outcome → Tool Preferences → Agent Path

| Step                                         | Status    | Evidence                                        |
| -------------------------------------------- | --------- | ----------------------------------------------- |
| `hu_self_improve_record_tool_outcome` called | **WIRED** | `agent_turn.c:1717` (after each tool execution) |
| Data → `tool_prefs` table                    | **WIRED** | `self_improve.c:152-196`                        |
| `hu_self_improve_get_tool_prefs_prompt`      | **WIRED** | `self_improve.c:286-318`                        |
| Called in agent_turn                         | **WIRED** | `agent_turn.c:594-600`                          |
| Injected into prompt                         | **WIRED** | Appended to `intelligence_ctx`                  |

**Verdict: WIRED** — Full path works. `tool_prefs` table is created by `hu_self_improve_init_tables` (cycle.c or cli.c). If never initialized, SELECT would fail and agent would skip tool prefs gracefully.

---

## 4. Experience → Prompt Path

| Step                                          | Status      | Evidence                |
| --------------------------------------------- | ----------- | ----------------------- |
| `hu_experience_record` called from agent_turn | **MISSING** | No grep in `src/agent/` |
| `hu_experience_recall_similar` called         | **MISSING** | No grep in `src/agent/` |
| `hu_experience_build_prompt` called           | **MISSING** | No grep in `src/agent/` |

**Verdict: MISSING** — Experience module exists (`src/intelligence/experience.c`), has tests, but is **not wired** into agent_turn. No recording, no recall, no prompt injection.

---

## 5. World Model → Agent Decision Path

| Step                                              | Status      | Evidence                                                             |
| ------------------------------------------------- | ----------- | -------------------------------------------------------------------- |
| `hu_world_simulate` called before agent decisions | **MISSING** | No calls from agent_turn                                             |
| `hu_world_evaluate_options` called before agent   | **MISSING** | No calls from agent_turn                                             |
| `hu_world_record_outcome` called                  | **WIRED**   | `cycle.c:141` when actioning findings                                |
| `hu_world_simulate` / `evaluate_options` used     | **WIRED**   | Only in `world_model.c` (counterfactual, evaluate_options) and tests |

**Verdict: MISSING** — World model is used in the intelligence cycle to **record** outcomes of actioned findings. It is **not** used to simulate or evaluate options before the agent makes decisions. `hu_world_simulate` and `hu_world_evaluate_options` exist but are never called from the agent path.

---

## 6. Value Learning → Prompt Path

| Step                           | Status    | Evidence                                                                                   |
| ------------------------------ | --------- | ------------------------------------------------------------------------------------------ |
| `hu_value_build_prompt` called | **WIRED** | `agent_turn.c:648`                                                                         |
| Output in system prompt        | **WIRED** | `agent_turn.c:648-656` → `intelligence_ctx`                                                |
| Value learning populated       | **WIRED** | `hu_value_learn_from_correction` / `hu_value_learn_from_approval` — need to verify callers |

**Verdict: WIRED** — Value context is built and injected. Values are learned from corrections/approvals; callers would need separate audit.

---

## 7. Intelligence Cycle (cycle.c) → Everything

| Step                                        | Status      | Evidence                                                                                                                                                                                                 |
| ------------------------------------------- | ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `hu_intelligence_run_cycle` called from CLI | **WIRED**   | `cli.c:645` (after research-agent), `cli_commands.c:1122` (`feed learn`)                                                                                                                                 |
| Called from daemon                          | **MISSING** | No grep in `daemon.c`                                                                                                                                                                                    |
| Tables created by cycle                     | **WIRED**   | `apply_reflections`, `init_tables` for self_improve, online_learning                                                                                                                                     |
| Tables in sqlite.c schema                   | **Partial** | `behavioral_feedback`, `self_evaluations`, `research_findings`, `general_lessons` exist. `prompt_patches`, `tool_prefs`, `learning_signals`, `strategy_weights` are created by engine init, not sqlite.c |

**Verdict: BROKEN for daemon** — The intelligence cycle is never triggered by the daemon. It only runs when:

1. `human research-agent` (CLI) — parses response, stores findings, runs cycle
2. `human feed learn` (CLI) — runs cycle on existing DB

Daemon cron runs agent jobs but does **not** call `hu_findings_parse_and_store` or `hu_intelligence_run_cycle` after research-agent responses.

---

## 8. Research Pipeline → Findings → Action

| Step                   | Status       | Evidence                                                                          |
| ---------------------- | ------------ | --------------------------------------------------------------------------------- |
| Research agent on cron | **WIRED**    | `main.c:801` — `hu_cron_add_agent_job` with `hu_research_cron_expression()`       |
| Cron runs in daemon    | **MISSING**  | `hu_service_run_agent_cron` runs agent jobs but does **not** parse/store findings |
| Findings stored        | **CLI only** | `cli.c:642` — `hu_findings_parse_and_store` only in CLI research-agent path       |
| Cycle actions findings | **WIRED**    | `cycle.c:108-181` — reads `research_findings`, marks actioned                     |

**Verdict: BROKEN for daemon** — When research-agent cron fires in daemon, `hu_agent_turn` runs, response is sent to channel, but `hu_findings_parse_and_store` is **never** called. Findings are never stored. Intelligence cycle never runs. Research pipeline only works when running `human research-agent` from CLI.

---

## 9. Tree-of-Thought

| Step                              | Status    | Evidence                                                                   |
| --------------------------------- | --------- | -------------------------------------------------------------------------- |
| ToT triggered from agent_turn     | **WIRED** | `agent_turn.c:1035-1071`                                                   |
| Condition                         | **WIRED** | `tree_of_thought_enabled && iter == 1 && msg_len > 200`                    |
| Wrapped in `#ifndef HU_IS_TEST`   | **WIRED** | Real LLM path runs in production                                           |
| Recursive depth in non-test path  | **WIRED** | `tree_of_thought.c:282-363` — `max_depth > 1` loop with `chat_with_system` |
| Best thought injected into prompt | **WIRED** | `agent_turn.c:1048-1067`                                                   |

**Verdict: WIRED** — ToT is fully wired. Recursive expansion works in production. Tests use mock; production uses real provider calls.

---

## 10. Skill System

| Step                                     | Status    | Evidence                                                                                  |
| ---------------------------------------- | --------- | ----------------------------------------------------------------------------------------- |
| **Skillforge** (file-based)              | **WIRED** | `agent_turn.c:711-740` — `hu_skillforge_list_skills`, injects into `skills_ctx`           |
| **Intelligence skills** (`skills` table) | **WIRED** | `daemon.c:3795-3832` — `hu_skill_match_triggers`, injects via `PHASE6_APPEND`             |
| Skills created from cycle                | **WIRED** | `cycle.c:422` — `hu_skill_insert` from research findings                                  |
| **learned_skills** table                 | **WIRED** | `skill_system.c` — different schema, used by meta_learning                                |
| Agent uses which skills?                 | **Split** | Daemon: `skills` table (intelligence). CLI/gateway: skillforge (files). **Disconnected.** |

**Verdict: SPLIT** — Two parallel systems:

- **skillforge**: Discovers `*.skill.json` from `~/.human/skills/`. Used in agent_turn for all paths. Not connected to intelligence cycle.
- **skills** (intelligence): SQLite `skills` table. Created by cycle from research findings. Used **only** in daemon via `hu_skill_match_triggers`. CLI agent_turn uses skillforge, not `skills`.

**Authoritative:** `skills` table for intelligence-created skills; `learned_skills` for meta-learning. Skillforge is a separate, file-based system. They are not unified.

---

## Summary Table

| Path                                | WIRED | PROVEN (tests) | FLOWING (prod)                  | BROKEN                    | MISSING           |
| ----------------------------------- | ----- | -------------- | ------------------------------- | ------------------------- | ----------------- |
| Feedback → Reflection → Patches     | ✓     | ✓              | Daemon: partial (no apply)      | Daemon never runs cycle   | —                 |
| Online Learning → Prompt            | ✓     | ✓              | When cycle has run              | —                         | —                 |
| Tool Prefs → Prompt                 | ✓     | ✓              | When init_tables run            | —                         | —                 |
| Experience → Prompt                 | ✓     | ✓              | —                               | —                         | Agent integration |
| World Model → Decisions             | ✓     | ✓              | —                               | —                         | Agent integration |
| Value Learning → Prompt             | ✓     | ✓              | ✓                               | —                         | —                 |
| Intelligence Cycle                  | ✓     | ✓              | CLI only                        | Daemon never runs         | —                 |
| Research → Findings → Action        | ✓     | ✓              | CLI only                        | Daemon cron doesn't store | —                 |
| Tree-of-Thought                     | ✓     | ✓              | ✓                               | —                         | —                 |
| Skills (intelligence vs skillforge) | ✓     | ✓              | Daemon: skills; CLI: skillforge | Split, not unified        | —                 |

---

## Recommended Fixes

1. **Daemon: Run intelligence cycle periodically** — Add a daemon cron/schedule (e.g., hourly) to call `hu_intelligence_run_cycle` so `apply_reflections` runs and prompt_patches are created.
2. **Daemon: Parse research-agent output** — In `hu_service_run_agent_cron`, when job name is `research-agent`, call `hu_findings_parse_and_store` and `hu_intelligence_run_cycle` on the response before sending to channel.
3. **Wire experience into agent_turn** — Call `hu_experience_record` after tool use, `hu_experience_build_prompt` when building system prompt.
4. **Wire world model into agent** — Call `hu_world_evaluate_options` (or similar) before tool selection for complex decisions (optional, higher effort).
5. **Ensure init_tables before agent_turn** — Call `hu_self_improve_init_tables` and `hu_online_learning_init_tables` during agent/bootstrap init so tables exist even if cycle has never run.
