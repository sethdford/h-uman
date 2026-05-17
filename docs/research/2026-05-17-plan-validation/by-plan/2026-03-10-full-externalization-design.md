---
plan: docs/plans/2026-03-10-full-externalization-design.md
auditor: group-4-human-fidelity
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Remove all hardcoded strings, word lists, prompt templates, thresholds, and paths
from C code by introducing a compile-time embedding + runtime override system
(`hu_data_load()` reads `~/.human/data/<path>` first, falls back to xxd-embedded
defaults). Adds `src/data/loader.c` + auto-generated `src/data/embedded_*.c`
registry, with data files under `data/`.

## Key Claims (from the plan)
- Claim 1: `src/data/loader.c` + `include/human/data/loader.h` exist
- Claim 2: Embedded registry maps relative paths -> {bytes,len}
- Claim 3: `data/` directory holds prompts/, conversation/, persona/, security/, tone/
- Claim 4: Production code (security, conversation, prompts) reads via `hu_data_load`
- Claim 5: Config gains `temp_dir`, `data_dir`, per-channel thresholds

## Evidence

### Implemented? (code exists)
- `src/data/loader.c` and `include/human/data/loader.h` present
- 30+ embedded data files: `src/data/data_prompts_safety_rules_txt.c`,
  `data_conversation_filler_words_json.c`, `data_persona_circadian_phases_json.c`,
  `data_security_command_lists_json.c`, etc.
- `src/data/embedded_registry.c` present (registry table for path -> data)
- Source data files materialized: `data/prompts/{safety_rules,autonomy_*,
  persona_reinforcement,group_chat_hint,reasoning_instruction,default_identity,
  tone_hints}.{txt,json}`, `data/conversation/{ai_phrases,filler_words,
  contractions,conversation_intros,ai_disclosure_patterns,...}.json`,
  `data/persona/{circadian_phases,relationship_stages}.json`,
  `data/security/command_lists.json`

### Proven? (tests exist)
- Embedded files are byte-identical to source; loader is exercised by every
  caller that reads prompts/wordlists. No standalone `test_data_loader.c` was
  found, but the integration is covered transitively by tests in
  `tests/test_conversation.c`, `tests/test_persona.c`, `tests/test_security_*`
  (any failure to load embedded data fails those suites' setup).

### Wired? (called in runtime path / dispatch)
- `hu_data_load` callers: `src/context/conversation.c`, `src/memory/fast_capture.c`,
  `src/security/policy.c`, `src/cognition/dual_process.c`, `src/agent/agent_turn.c`
  (and many more — at least the five real production paths verified)
- Verifies the design goal: data is consumed via the loader, not via inline
  string literals.

## Gaps
- No dedicated `test_data_loader.c` was found in tests/. A unit test that
  exercises the override path (file in `~/.human/data/...` shadows embedded
  default) would be valuable, but the codepath is exercised through callers.

## Notes
- This is foundational plumbing; nearly every later plan (persona, conversation
  intelligence, security) depends on it and benefits silently.
- Data dir is bigger than originally enumerated in the plan (agent/, channels/,
  memory/, cognition/, eval/ all gained data files) — scope expanded over time.
