---
title: "HuLa — Human Language for agent programs (IR, execution, emergence)"
created: 2026-03-22
status: closed
last_audit: 2026-05-25
---

## Summary

**HuLa** is a small JSON tree IR for structured tool orchestration inside the agent runtime. Programs have a `name`, `version`, and `root` node. Nodes use opcodes (`call`, `seq`, `par`, `branch`, `loop`, `delegate`, `emit`) with ids, tool names, JSON args, and optional children. The executor (`hu_hula_exec_*` in `src/agent/hula.c`) runs programs with policy checks, observer events, and a JSON execution trace.

This document is the **canonical field-level spec** for authors and LLM compilers.

## JSON schema (conceptual)

Top-level object:

| Field     | Type   | Required | Notes                          |
| --------- | ------ | -------- | ------------------------------ |
| `name`    | string | no       | Logical program name           |
| `version` | number | no       | Default `1` (`HU_HULA_VERSION`) |
| `root`    | object | yes      | Root node                      |

Every node:

| Field        | Type   | Required | Notes                                                |
| ------------ | ------ | -------- | ---------------------------------------------------- |
| `op`         | string | yes      | `call`, `seq`, `par`, `branch`, `loop`, `delegate`, `emit` |
| `id`         | string | yes      | Unique within program; used for results and `$refs` |
| `tool`       | string | for `call` | Must match a registered tool name                |
| `args`       | object | for `call` | Serialized as JSON string internally; **string values** may include `$node_id`, expanded from prior **done** outputs (longest id match; capped per value, currently 256 KiB) |
| `children`   | array  | for `seq`/`par` | Ordered for `seq`; concurrent for `par`      |
| `goal`       | string | for `delegate` | Sub-agent goal text                          |
| `model`      | string | no       | Optional delegate model override                   |
| `pred`       | string | branch/loop | `success`, `failure`, `contains`, `not_contains`, `always` |
| `match`      | string | no       | Substring for `contains` / `not_contains`          |
| `then`/`else`| object | branch shorthand | Alternative to `children`                 |
| `body`       | object | loop     | Loop body node                                       |
| `max_iter`   | number | loop     | Upper bound; default estimate uses `10` if unset     |
| `emit_key` / `emit_value` | strings | emit | Static or `$node_id` substitution        |

## Execution semantics (summary)

- **`seq`**: children run in order; stops on first failing `call` (failure propagates).
- **`par`**: all children run; aggregation follows executor rules for parent result.
- **`branch`**: evaluates predicate against prior sibling/parent context; runs one arm.
- **`loop`**: bounded iterations over `body`; uses `max_iter` when set.
- **`delegate`**: with `hu_agent_pool_t` + `hu_spawn_config_t` (non-test Unix), spawns a child agent; optional **child program** is embedded from `children` as JSON in an extended system prompt. The main turn wires pool + `hu_spawn_config_apply_parent_agent` into `hu_hula_exec_set_spawn` for native HuLa, the trivial IR path, and `hu_hula_compiler_chat_compile_execute` (stack `hu_spawn_config_t` must remain valid until `hu_hula_exec_run` returns). Without pool/config, returns the goal string as a stub success result (`HU_IS_TEST` always stubs delegate execution).
- **`emit`**: produces a string value, resolving `$id` placeholders from prior node outputs.
- **`call` args**: after JSON parse, the executor walks string values and expands `$node_id` the same way as `emit_value` (referenced node must be **done** before the `call` runs, for example under `seq`). A substitution ends at a **boundary**: the next character must not be an id-continuation character (`[A-Za-z0-9_]`); otherwise a longer id is required (use an explicit delimiter such as `.` between a ref and literal text).

Policy (`hu_security_policy_t`) is applied before each tool `call`. Risk is combined into `hu_hula_estimate_cost` (`max_tool_risk`).

## Agent integration

| Mode | Trigger | Code paths |
| ---- | ------- | ---------- |
| **Config** | `"hula": true` under `agent` in `config.json` (default **on** after config merge) | `hu_agent_config_t.hula_enabled` → `agent_turn.c`, `prompt.c` |
| **Native text** | Model returns `<hula_program>...</hula_program>` with no tool calls | `hu_hula_extract_program_from_text`, execute, strip tags from assistant text (`agent_turn.c`, non-test) |
| **LLM compiler** | `hula_enabled` and ≥3 tool calls in one turn | `hu_hula_compiler_build_prompt` → provider `chat` with `json_object` → `hu_hula_compiler_parse_response` → validate → execute (non-test); skips DAG LLMCompiler when successful |
| **Trivial IR** | `hula_enabled` and tool calls without compiler path | Builds `par` or single `call` program (`agent_turn.c`) |

When `hula_program_protocol` is set on `hu_prompt_config_t`, the system prompt includes the `<hula_program>` embedding convention (`prompt.c`).

## Provider JSON mode, parsing fallbacks, and platforms

- **Structured output**: The in-turn compiler path calls the configured provider `chat` with a JSON-shaped response when the provider supports it (e.g. OpenAI-compatible `response_format` / `json_object`). Exact wire format depends on the provider implementation in `src/providers/`.
- **Parsing**: `hu_hula_compiler_parse_response` still accepts normal assistant text: markdown fenced JSON code blocks and brace-balanced extraction, so a model that ignores JSON mode can still succeed if the reply contains valid HuLa JSON.
- **Delegate / spawn**: `hu_hula_exec_set_spawn` uses `hu_spawn_config_t` filled by `hu_spawn_config_apply_parent_agent` (mirrors `agent_spawn` inheritance; does not copy API keys). The spawn template must remain valid for the whole `hu_hula_exec_run` call (callers use stack storage in `agent_turn.c`). Real child processes require POSIX (`fork` / gateway paths); on non-Unix builds or in `HU_IS_TEST`, delegate behavior stubs as documented in `hula.c`.
- **Smoke check**: After `cmake --build build`, run `./scripts/hula-smoke.sh` to validate and execute a tiny inline program via the CLI.

## Traces and emergence

- **`hu_hula_trace_persist`**: writes a JSON file under `trace_dir` or `~/.human/hula_traces/` (POSIX). Callers may pass optional `program_json` / `program_json_len` to embed the source program for **`human hula replay`**. The agent (`agent_turn.c`) and LLM compiler (`hula_compiler.c`) pass **`hu_hula_to_json`** output when persisting so traces are replayable even without the original chat text. In `HU_IS_TEST`, passing `NULL` for `trace_dir` is a no-op; tests pass an explicit temp directory.
- **`hu_hula_emergence_scan`**: reads `*.json` traces, extracts ordered `call` tools from the `trace` array, counts n-grams (e.g. `echo|grep`), returns patterns meeting `min_occurrences`.
- **`hu_hula_emergence_promote`**: materializes a pattern as `*.skill.json` plus `*_HULA.md` under `skills_dir` or `~/.human/skills/`, with a `seq` of `call` nodes (empty args).

## References

- **Human guide (operators & prompt authors):** [`docs/guides/hula.md`](../guides/hula.md)
- Types and APIs: `include/human/agent/hula.h`, `include/human/agent/hula_compiler.h`, `include/human/agent/hula_emergence.h`, `include/human/agent/spawn.h` (`hu_spawn_config_apply_parent_agent`)
- Implementation: `src/agent/hula.c`, `hula_compiler.c`, `hula_emergence.c`, `agent_turn.c`, `prompt.c`, `spawn.c`
- Tests: `tests/test_hula.c`, `tests/test_prompt.c`, `tests/test_config_extended.c`
- Examples: `examples/README.md`, `examples/hula_*.json`
