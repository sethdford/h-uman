# Sprint 10 Backlog — HuLa Platform

## Sprint Metadata

| Field | Value |
|---|---|
| Sprint number | 10 |
| Goal | Ship minimum viable HuLa bindings (Python + TypeScript), an examples gallery, an MCP bridge, and SDK v0.2.0 so an external developer can write and run a HuLa program end-to-end |
| Dates | 2026-05-17 → 2026-05-28 |
| Scrum-master | TBD |
| Branch | `sprint-10-hula-platform` |
| Working directory | `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula` |
| Base SHA | `ea02b08e` |
| Risk tier | LOW-MEDIUM (additive surface area; no vtable or IR changes) |
| Budget | ~$20 |

---

## User Stories (priority order)

---

### US-10.1 (P0): SDK v0.2.0 surface — context handles and error string accessors

**As a** language-binding author,
**I want** `hula_sdk.h` to expose a `hu_hula_ctx_t` context handle and a `hu_hula_error_string()` accessor,
**so that** Python and TypeScript bindings can manage lifetime and surface human-readable error messages without reimplementing that logic.

**Acceptance criteria:**

- AC-10.1.1: GIVEN the header `include/human/hula_sdk.h` is included, WHEN a binding author calls `hu_hula_ctx_create(alloc)` and `hu_hula_ctx_destroy(ctx)`, THEN the calls compile without warnings under `-Wall -Wextra -Wpedantic` and return `HU_OK` / free cleanly under ASan.
- AC-10.1.2: GIVEN any `hu_error_t` value (including `HU_OK`, `HU_ERR_INVALID_ARGUMENT`, `HU_ERR_OUT_OF_MEMORY`, and one unknown value), WHEN `hu_hula_error_string(err)` is called, THEN it returns a non-NULL, NUL-terminated ASCII string that is distinct for each known code and is `"HU_ERR_UNKNOWN"` for unknown values.
- AC-10.1.3: GIVEN an existing v0.1.0 caller that uses `hu_hula_sdk_call`, `hu_hula_sdk_sequence`, or `hu_hula_sdk_run_json`, WHEN compiled against the v0.2.0 header, THEN it compiles without modification (no breaking change).
- AC-10.1.4: GIVEN the header, WHEN the version macros are read, THEN `HU_HULA_SDK_VERSION_MINOR` equals `2` and `HU_HULA_SDK_VERSION_STRING` equals `"0.2.0"`.
- AC-10.1.5: A file `bindings/sdk-changelog.md` exists documenting every addition in v0.2.0 with the rationale for each symbol.
- AC-10.1.6: Full test suite (`./build/human_tests`) passes with 0 failures, 0 ASan errors after the change.

**Estimate:** XS
**Priority:** P0
**Dependencies:** none
**Risk tier:** LOW
**Test seam:** New unit tests in `tests/test_hula_sdk_v2.c` — references `hu_hula_ctx_create`, `hu_hula_ctx_destroy`, and `hu_hula_error_string` directly (satisfies `test-references-production-symbol` rule).
**Out of scope:** Async context, thread-safety guarantees, error localization.
**DoD:** Build clean under `cmake --preset dev`; full suite green; `/verify` PASS; `bindings/sdk-changelog.md` committed.

---

### US-10.2 (P0): Python ctypes bindings (`bindings/python/`)

**As a** Python developer,
**I want** to `pip install -e .` a `hula` package and write a HuLa program in Python using a clean API,
**so that** I can orchestrate AI tools with HuLa's typed IR without writing C.

**Acceptance criteria:**

- AC-10.2.1: GIVEN the repo is cloned and `libhuman.so` / `libhuman.dylib` is built, WHEN a developer runs `pip install -e bindings/python/` from the repo root, THEN the command exits 0 and `import hula` succeeds in the installed environment.
- AC-10.2.2: GIVEN the installed package, WHEN the following code runs end-to-end against a stub tool registered via the C SDK:
  ```python
  from hula import HulaContext, Program
  ctx = HulaContext()
  result = ctx.run_json('{"version":1,"name":"t","root":{"op":"call","id":"r","tool":"echo","args":{"msg":"hi"}}}')
  assert result is not None
  ```
  THEN it executes without exception.
- AC-10.2.3: GIVEN the Python package, WHEN `pytest bindings/python/tests/` is run, THEN all tests pass (minimum 5 tests covering: parse valid JSON, parse invalid JSON raises exception, run single CALL node, run SEQ node, error string accessor returns non-empty string for `HU_ERR_INVALID_ARGUMENT`).
- AC-10.2.4: GIVEN `bindings/python/examples/hello_hula.py`, WHEN run with `python bindings/python/examples/hello_hula.py`, THEN it prints a non-empty result to stdout and exits 0.
- AC-10.2.5: GIVEN a JSON program with a malformed opcode, WHEN `ctx.run_json(bad_json)` is called, THEN a `HulaError` exception is raised (not a segfault or silent failure).
- AC-10.2.6: Full C test suite remains green (binding is additive; no C source modified except the SDK header per US-10.1).

**Estimate:** M
**Priority:** P0
**Dependencies:** US-10.1 (needs context handle and error string accessor)
**Risk tier:** LOW (ctypes; no C source changes)
**Test seam:** `bindings/python/tests/test_hula.py` references `hu_hula_ctx_create`, `hu_hula_sdk_run_json` via ctypes (verifiable by `check-test-references.sh` equivalent for Python).
**Out of scope:** PyPI publishing, async/await support, numpy/pandas integration, Python 2.
**DoD:** `pip install -e .` exits 0; `pytest bindings/python/tests/` all pass; `/verify` PASS; example runs end-to-end.

---

### US-10.3 (P1): TypeScript/Node bindings (`bindings/typescript/`)

**As a** TypeScript developer,
**I want** to `npm install` a `hula` package and call `runJson(program)` from TypeScript with full type annotations,
**so that** I can integrate HuLa orchestration into a Node.js or browser-bundled application without a C toolchain at runtime.

**Acceptance criteria:**

- AC-10.3.1: GIVEN the repo is cloned and the shared library is built, WHEN a developer runs `npm install` inside `bindings/typescript/`, THEN the command exits 0 and `import { HulaContext } from 'hula'` compiles under `tsc --strict`.
- AC-10.3.2: GIVEN the installed package, WHEN the following TypeScript runs:
  ```typescript
  import { HulaContext } from 'hula';
  const ctx = new HulaContext();
  const result = await ctx.runJson('{"version":1,"name":"t","root":{"op":"call","id":"r","tool":"echo","args":{"msg":"hi"}}}');
  expect(result).toBeTruthy();
  ```
  THEN vitest reports the test as passing.
- AC-10.3.3: GIVEN `bindings/typescript/`, WHEN `npm test` is run, THEN all vitest tests pass (minimum 5 tests mirroring the Python AC-10.2.3 set).
- AC-10.3.4: GIVEN `bindings/typescript/examples/hello_hula.ts`, WHEN run with `npx tsx bindings/typescript/examples/hello_hula.ts`, THEN it prints a non-empty result to stdout and exits 0.
- AC-10.3.5: GIVEN a malformed JSON program, WHEN `ctx.runJson(bad)` is awaited, THEN it rejects with a `HulaError` instance whose `.code` property is a string matching a known `hu_error_t` name.
- AC-10.3.6: `tsc --noEmit --strict` in `bindings/typescript/` exits 0 (no type errors).

**Estimate:** M
**Priority:** P1
**Dependencies:** US-10.1 (context handle and error string accessor)
**Risk tier:** LOW (N-API or ffi-napi wrapping; no C source changes)
**Test seam:** `bindings/typescript/src/index.ts` wraps the same three SDK entry points as the Python bindings; test file imports from the package (not from a local re-implementation).
**Out of scope:** WASM build target, browser bundle, npm publish, Deno support, ESM/CJS dual-build beyond what `package.json` `exports` map provides.
**DoD:** `npm install` exits 0; `npm test` all pass; `tsc --noEmit` clean; example runs; `/verify` PASS.

---

### US-10.4 (P1): HuLa examples gallery (`examples/hula/`)

**As a** developer evaluating HuLa,
**I want** five self-contained, runnable HuLa programs with READMEs,
**so that** I can understand the orchestration patterns without reading the C source.

**Acceptance criteria:**

- AC-10.4.1: GIVEN `examples/hula/`, WHEN it is listed, THEN it contains exactly these five subdirectories: `01-simple-call/`, `02-branching/`, `03-error-recovery/`, `04-emergence-detection/`, `05-multi-step-pipeline/`. Each subdirectory contains a `program.json` (valid HuLa JSON) and a `README.md` (minimum 100 words, explains what the program demonstrates and how to run it).
- AC-10.4.2: GIVEN each `program.json`, WHEN parsed via `hu_hula_parse_json` in a test harness (see test seam), THEN `hu_hula_validate` returns `val.valid == true` — i.e., every example is structurally valid HuLa.
- AC-10.4.3: GIVEN `01-simple-call/program.json`, WHEN inspected, THEN its root node `op` is `"call"` and it has a non-empty `"tool"` field.
- AC-10.4.4: GIVEN `02-branching/program.json`, WHEN inspected, THEN it contains at least one node with `"op": "branch"` with `then` and `else` children.
- AC-10.4.5: GIVEN `03-error-recovery/program.json`, WHEN inspected, THEN it contains at least one `"op": "try"` node with a `catch` child.
- AC-10.4.6: GIVEN `04-emergence-detection/program.json`, WHEN inspected, THEN it contains at least one `"op": "verify"` node (the opcode used to assert a condition and trigger emergence tracing).
- AC-10.4.7: GIVEN `05-multi-step-pipeline/program.json`, WHEN inspected, THEN it contains both `"op": "seq"` and `"op": "par"` nodes (demonstrates mixed sequential/parallel orchestration).
- AC-10.4.8: A top-level `examples/hula/README.md` exists that links to all five examples and states which opcode(s) each one demonstrates.
- AC-10.4.9: Full C test suite remains green.

**Estimate:** S
**Priority:** P1
**Dependencies:** none (pure JSON + Markdown; builds on existing examples/hula/ directory)
**Risk tier:** LOW
**Test seam:** `tests/test_hula_examples.c` — iterates over the five program.json files, calls `hu_hula_parse_json` + `hu_hula_validate`, asserts `val.valid`. Satisfies `test-references-production-symbol` rule (references `hu_hula_parse_json` and `hu_hula_validate`).
**Out of scope:** Runnable examples that need a live model provider (all examples use stub/echo tools). No hosted playground.
**DoD:** All five examples present and valid; `tests/test_hula_examples.c` passes; `/verify` PASS.

---

### US-10.5 (P1): MCP bridge — HuLa CALL nodes can invoke MCP server tools

**As a** HuLa program author,
**I want** to write a HuLa CALL node that targets a tool exposed by an MCP server (prefixed `mcp__<server>__<tool>`),
**so that** HuLa programs can orchestrate the growing ecosystem of MCP-compatible tools without reimplementing them.

**Acceptance criteria:**

- AC-10.5.1: GIVEN a `hu_mcp_client_t` connected to a stub MCP server (test-only, no real process), WHEN `hu_hula_mcp_bridge_tool_list(client, alloc, &tools, &count)` is called, THEN it returns a `hu_tool_t` array where each tool's `name()` returns a string of the form `"mcp__<server>__<tool_name>"` and `count >= 1`.
- AC-10.5.2: GIVEN the bridge tool array from AC-10.5.1 registered with a `hu_hula_exec_t`, WHEN a HuLa program with a CALL node targeting `"mcp__stub__echo"` is executed, THEN `hu_hula_exec_run` returns `HU_OK` and the result output matches the stub's echo response.
- AC-10.5.3: GIVEN the bridge and an MCP server that returns an error for a specific tool call, WHEN the HuLa executor runs that CALL node, THEN the node result has `ok == false` and the error is surfaced in the trace (not silently swallowed).
- AC-10.5.4: GIVEN `bindings/python/examples/mcp_bridge.py`, WHEN run with Python against the stub MCP server, THEN it executes a HuLa program that invokes one MCP tool and prints the result to stdout without error.
- AC-10.5.5: The bridge function `hu_hula_mcp_bridge_tool_list` is declared in `include/human/hula_mcp_bridge.h` (new header) and implemented in `src/agent/hula_mcp_bridge.c` (new file). No existing `src/mcp/` or `src/agent/hula.c` files are modified except to register the bridge header include in `hula_sdk.h`.
- AC-10.5.6: Unit tests in `tests/test_hula_mcp_bridge.c` cover: list returns correct name format, invoke succeeds with stub, invoke propagates error from stub. Full suite green.

**Estimate:** M
**Priority:** P1
**Dependencies:** US-10.1 (context handle), US-10.2 (Python example depends on Python bindings)
**Risk tier:** MEDIUM (new public header and source file; touches MCP client which is existing infrastructure)
**Test seam:** `tests/test_hula_mcp_bridge.c` references `hu_hula_mcp_bridge_tool_list` and `hu_hula_exec_run` (production symbols).
**Out of scope:** MCP server mode (h-uman acting as MCP server), MCP over HTTP transport (stdio only this sprint), auto-discovery of all MCP servers in config (manual registration only).
**DoD:** New header + source file compile clean; `tests/test_hula_mcp_bridge.c` passes; Python example runs; `/verify` PASS.

---

### US-10.6 (P2): CI integration — bindings build and test in CI matrix

**As a** maintainer,
**I want** the Python and TypeScript bindings to build and test in CI,
**so that** a broken binding is caught before merge and the "external developer" story is continuously verified.

**Acceptance criteria:**

- AC-10.6.1: GIVEN a PR that modifies any file under `bindings/`, WHEN CI runs, THEN a job named `bindings` (or equivalent) executes `pip install -e bindings/python/ && pytest bindings/python/tests/` and `npm --prefix bindings/typescript install && npm --prefix bindings/typescript test` in sequence, both of which must exit 0 for the job to pass.
- AC-10.6.2: GIVEN a PR that deliberately breaks the Python binding (e.g., removes `hu_hula_ctx_create` from the SDK header), WHEN CI runs, THEN the `bindings` job fails (exit non-zero), blocking merge.
- AC-10.6.3: GIVEN the CI job, WHEN it runs on both `ubuntu-latest` and `macos-latest` matrix entries, THEN both pass (bindings are platform-portable within the matrix).
- AC-10.6.4: The CI job runs only when files under `bindings/**` or `include/human/hula_sdk.h` change (path filter), keeping the default `ci.yml` fast path unaffected when unrelated files change.
- AC-10.6.5: GIVEN a clean main branch, WHEN the CI job runs on the `sprint-10-hula-platform` branch, THEN it passes end-to-end (green before merge).

**Estimate:** S
**Priority:** P2
**Dependencies:** US-10.2 (Python bindings), US-10.3 (TypeScript bindings)
**Risk tier:** LOW (CI config only; no C source or SDK changes)
**Test seam:** The CI workflow itself is the test — AC-10.6.2 provides the falsifiability check.
**Out of scope:** PyPI publish step, npm publish step, code signing, Windows runner.
**DoD:** New workflow (or modified `ci.yml`) committed; CI green on branch before sprint close; `/verify` PASS.

---

## Non-goals (this sprint)

- We will NOT publish to PyPI, npm, or any public package registry.
- We will NOT build a VS Code extension, language server, or syntax highlighter for HuLa.
- We will NOT refactor or change the semantics of any existing opcode in `src/agent/hula.c` or `include/human/agent/hula.h`.
- We will NOT implement h-uman acting as an MCP server (reserved for a future sprint per the note in `mcp.h`).
- We will NOT build a hosted docs site — `docs/hula/` Markdown files and an `examples/hula/` gallery are the deliverable.

---

## Open questions for stakeholder

1. **Python binding mechanism:** ctypes (pure Python, no build step, slower) vs cffi (requires a compile step but faster at runtime). The stories assume ctypes for simplicity; confirm before implementation starts.
2. **TypeScript binding mechanism:** N-API native addon (requires build step, faster) vs `ffi-napi` (pure JS, no build step, slightly slower). Confirm preferred tradeoff.
3. **Stub MCP server for tests (AC-10.5.1/10.5.2):** Does a suitable test-only stub already exist, or should US-10.5 include writing one? If writing one is out of scope, US-10.5 estimate should increase to L.
4. **`docs/hula/` website route:** The sprint commits Markdown only. Is a website route (`website/src/pages/hula/`) a P2 bonus or explicitly out of scope for sprint 10?

---

RESULT_product-owner=READY
