---
plan: docs/plans/2026-03-22-hula-language.md
auditor: group-6-hula-platform-strategic
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Canonical field-level spec for HuLa — a small JSON tree IR for structured tool
orchestration inside the agent runtime. Defines opcodes (call/seq/par/branch/
loop/delegate/emit), execution semantics, agent integration modes (native text,
LLM compiler, trivial IR), traces, and emergence/skill promotion.

## Key Claims (from the plan)
- Claim 1: Executor lives at `hu_hula_exec_*` in `src/agent/hula.c`.
- Claim 2: Compiler path `hu_hula_compiler_*` in `src/agent/hula_compiler.c`.
- Claim 3: Emergence promotion in `src/agent/hula_emergence.c`.
- Claim 4: Agent integration in `agent_turn.c` for native text, LLM-compiler, and
  trivial IR modes.
- Claim 5: System prompt embeds `<hula_program>` convention via `prompt.c`.
- Claim 6: Tests in `tests/test_hula.c`, examples in `examples/hula_*.json`,
  smoke script `scripts/hula-smoke.sh`.

## Evidence

### Implemented? (code exists)
- `src/agent/hula.c` (2329 LOC) — executor + JSON parse/serialize
- `src/agent/hula_compiler.c` (676 LOC) — LLM compiler path
- `src/agent/hula_emergence.c` (476 LOC) — emergence scan + promote
- `src/agent/hula_lite.c`, `src/agent/hula_analytics.c`, `src/agent/hula_compiler_examples.c` (additional surface)
- `include/human/agent/hula.h`, `hula_compiler.h`, `hula_emergence.h`, `spawn.h` — all present
- `include/human/hula_sdk.h:67-70` — `HU_HULA_SDK_VERSION_STRING "0.1.0"`,
  major/minor/patch macros as CLAUDE.md claims
- `include/human/hula_sdk.h:82,123,185` — ergonomic helpers
  `hu_hula_sdk_call`, `hu_hula_sdk_sequence`, `hu_hula_sdk_run_json`
  (inline wrappers over core API)
- `examples/hula_minimal.json`, `hula_arg_refs.json`, `hula_loop_retry.json`,
  `hula_parallel_fetch.json`, `hula_research_pipeline.json` — example bank
- `scripts/hula-smoke.sh` — smoke script as documented

### Proven? (tests exist)
- `tests/test_hula.c` (1945 LOC) — 69 `HU_RUN_TEST` entries covering
  parse/validate/execute for every opcode, branch predicates, loop bounds,
  delegate stubs, emit/$ref substitution
- `tests/test_hula_golden.c` (80 LOC) — 5 golden roundtrip tests
- Plan also cites `tests/test_prompt.c`, `tests/test_config_extended.c` —
  both files exist (covering prompt embedding and config wiring)

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c:5064` — native text path: `hula_enabled` &&
  no tool calls && `hu_hula_extract_program_from_text` →
  `hu_hula_exec_init_full` → `hu_hula_exec_run` → `hu_hula_trace_persist`
- `src/agent/agent_turn.c:7095` — LLM-compiler path: `hula_enabled` &&
  `tc_count >= 3` → `hu_hula_compiler_chat_compile_execute`
- `src/agent/agent_turn.c:7527` — trivial IR path: `tc_count >= 1`
  branching to `hu_hula_exec_init_full`/`run`/trace persistence
- `src/agent/agent_turn.c:697` — `agent_turn_hula_exec_bind_spawn` calls
  `hu_hula_exec_set_spawn` for delegate sub-agent spawning
- All three execution modes serialize program JSON via `hu_hula_to_json`
  and persist replayable traces

## Gaps
- SDK ergonomic helpers in `include/human/hula_sdk.h`
  (`hu_hula_sdk_call`, `hu_hula_sdk_sequence`, `hu_hula_sdk_run_json`)
  are defined but have **zero callers** in `tests/` or `examples/` — they
  ship the surface without exercising it. SDK version is 0.1.0 (pre-1.0,
  consistent with M5 status).
- HuLa SDK has no language bindings (Python/Node) and no public docs site —
  these are M5 Phase 5.2-5.4 items, not part of this spec doc.

## Notes
- This plan is the **canonical spec**, not a roadmap; M5 in
  `2026-04-11-strategic-missions.md` is the productization roadmap.
- Cross-references: `docs/guides/hula.md` (operator/author guide) exists.
- Plan's status frontmatter is `implemented` and matches reality.
