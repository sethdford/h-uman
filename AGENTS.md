# AGENTS.md — seaclaw Agent Engineering Protocol

This file defines the default working protocol for coding agents in this repository.
Scope: entire repository.

## 1) Project Snapshot (Read First)

seaclaw is a C11 autonomous AI assistant runtime optimized for:

- minimal binary size (282 KB core MinSizeRel)
- minimal memory footprint (target: < 5 MB peak RSS)
- zero dependencies beyond libc, optional SQLite and libcurl
- Zig reference implementation archived in `archive/zig-reference/`

Core architecture is **vtable-driven** and modular. All extension work is done by implementing
vtable structs and registering them in factory functions.

Key extension points:

- `src/providers/` (`sc_provider_t`) — AI model providers
- `src/channels/` (`sc_channel_t`) — messaging channels
- `src/tools/` (`sc_tool_t`) — tool execution surface
- `src/memory/` (`sc_memory_t`) — memory backends
- `src/observability/` (`sc_observer_t`) — observability hooks
- `src/runtime/` (`sc_runtime_t`) — execution environments
- `src/peripherals/` (`sc_peripheral_t`) — hardware boards (Arduino, STM32, RPi)

Current scale: **432 source + header files, ~64K lines of C, ~26K lines of tests, 1,946 tests**.

Build and test:

```bash
mkdir build && cd build
cmake .. -DSC_ENABLE_ALL_CHANNELS=ON    # configure
cmake --build . -j$(nproc)              # dev build
./seaclaw_tests                        # run all tests
cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON  # release build
```

## 2) Deep Architecture Observations (Why This Protocol Exists)

These codebase realities should drive every design decision:

1. **Vtable + factory architecture is the stability backbone**
   - Extension points are explicit and swappable via `void *ctx` + function pointer vtables.
   - Callers must OWN the implementing struct (local var or heap-alloc). Never return a vtable interface pointing to a temporary — the pointer will dangle.
   - Most features should be added via vtable implementation + factory registration, not cross-cutting rewrites.

2. **Binary size and memory are hard product constraints**
   - `cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON` is the release target. Every dependency and abstraction has a size cost.
   - Avoid adding unnecessary runtime allocations or large data tables without justification.
   - Current release binary: 282 KB (core) / 380 KB (full).

3. **Security-critical surfaces are first-class**
   - `src/gateway/gateway.c`, `src/security/`, `src/tools/`, `src/runtime/` carry high blast radius.
   - Defaults are secure-by-default (pairing, HTTPS-only, allowlists, AEAD encryption). Keep it that way.

4. **C11 is the baseline — strict standards compliance**
   - HTTP client: libcurl (`SC_ENABLE_CURL=ON`), conditionally compiled.
   - Child processes: `fork`/`exec` on POSIX, guarded by `SC_GATEWAY_POSIX`.
   - SQLite: linked via system or Homebrew paths.
   - All code compiles with `-Wall -Wextra -Wpedantic -Werror`.
   - Use `SC_IS_TEST` guards to bypass side effects (spawning, opening URLs, real hardware I/O).

5. **All 1,946+ tests must pass at zero ASan errors**
   - The test suite uses AddressSanitizer for leak and overflow detection.
   - Every allocation must be freed (`free()` or cleanup function).
   - Use `SC_IS_TEST` mock paths in tests — no network, no process spawning.

## 3) Engineering Principles (Normative)

These principles are mandatory. They are implementation constraints, not suggestions.

### 3.1 KISS

Required:

- Prefer straightforward control flow over meta-programming.
- Prefer explicit `#ifdef` branches and typed structs over hidden dynamic behavior.
- Keep error paths obvious and localized.

### 3.2 YAGNI

Required:

- Do not add config keys, vtable methods, or feature flags without a concrete caller.
- Do not introduce speculative abstractions.
- Keep unsupported paths explicit (`return SC_ERR_NOT_SUPPORTED`) rather than silent no-ops.

### 3.3 DRY + Rule of Three

Required:

- Duplicate small local logic when it preserves clarity.
- Extract shared helpers only after repeated, stable patterns (rule-of-three).
- When extracting, preserve module boundaries and avoid hidden coupling.

### 3.4 Fail Fast + Explicit Errors

Required:

- Prefer explicit errors for unsupported or unsafe states.
- Never silently broaden permissions or capabilities.
- In tests: `SC_IS_TEST` guards are acceptable to skip side effects (e.g., spawning browsers), but the guard must be explicit and documented.

### 3.5 Secure by Default + Least Privilege

Required:

- Deny-by-default for access and exposure boundaries.
- Never log secrets, raw tokens, or sensitive payloads.
- All outbound URLs must be HTTPS. HTTP is rejected at the tool layer.
- Keep network/filesystem/shell scope as narrow as possible.

### 3.6 Determinism + No Flaky Tests

Required:

- Tests must not spawn real network connections, open browsers, or depend on system state.
- Use `SC_IS_TEST` to bypass side effects (spawning, opening URLs, real hardware I/O).
- Tests must be reproducible across macOS and Linux.

## 4) Repository Map (High-Level)

```
src/
  main.c                CLI entrypoint and command routing
  agent/                agent loop, context, planner, compaction, dispatcher
  channels/             20 channel implementations (cli, telegram, discord, slack, ...)
  providers/            50+ AI provider implementations (9 core + 41 compatible services)
  tools/                47 tool implementations
  memory/               SQLite + markdown + LRU backends, embeddings, vector search
  security/             policy, pairing, secrets, sandbox backends (landlock, firejail, bwrap)
  runtime/              runtime adapters (native, docker, wasm, cloudflare)
  core/                 allocator, arena, error, json, http, string, slice
  observability/        log + metrics observers
  sse/                  SSE client
  websocket/            WebSocket client
  peripherals/          hardware peripherals (Arduino, STM32/Nucleo, RPi)
  config.c              schema + config loading/merging (~/.seaclaw/config.json)
  gateway/gateway.c     webhook/HTTP gateway server
  ...

include/seaclaw/       public C headers

tests/                 71 test files, 1,946 tests

asm/                   platform-specific assembly (aarch64, x86_64, generic C)

archive/zig-reference/ archived Zig source (build.zig, src/)
```

## 5) Risk Tiers by Path (Review Depth Contract)

- **Low risk**: docs, comments, test additions, minor formatting
- **Medium risk**: most `src/**` behavior changes without boundary/security impact
- **High risk**: `src/security/**`, `src/gateway/gateway.c`, `src/tools/**`, `src/runtime/`, config schema, vtable interfaces

When uncertain, classify as higher risk.

## 6) Agent Workflow (Required)

1. **Read before write** — inspect existing module, vtable wiring, and adjacent tests before editing.
2. **Define scope boundary** — one concern per change; avoid mixed feature+refactor+infra patches.
3. **Implement minimal patch** — apply KISS/YAGNI/DRY rule-of-three explicitly.
4. **Validate** — `cmake --build build && ./build/seaclaw_tests` must show 0 failures and 0 ASan errors.
5. **Document impact** — update comments/docs for behavior changes, risk, and side effects.

### 6.1 Code Naming Contract (Required)

Apply these naming rules consistently:

- All identifiers: `snake_case` for functions, variables, fields, modules, files.
- Types, structs, enums, unions: `sc_<name>_t` (e.g., `sc_provider_t`, `sc_channel_t`).
- Constants and macros: `SC_SCREAMING_SNAKE_CASE` (e.g., `SC_OK`, `SC_ERR_NOT_SUPPORTED`).
- Public functions: `sc_<module>_<action>` (e.g., `sc_provider_create`, `sc_channel_send`).
- Factory registration keys: stable, lowercase, user-facing (e.g., `"openai"`, `"telegram"`, `"shell"`).
- Tests: named by behavior (`subject_expected_behavior`), fixtures use neutral names.

### 6.2 Architecture Boundary Contract (Required)

- Extend capabilities by adding vtable implementations + factory wiring first.
- Keep dependency direction inward to contracts: concrete implementations depend on vtable/config/util, not on each other.
- Avoid cross-subsystem coupling (provider code importing channel internals, tool code mutating gateway policy).
- Keep module responsibilities single-purpose: orchestration in `agent/`, transport in `channels/`, model I/O in `providers/`, policy in `security/`, execution in `tools/`.

## 7) Change Playbooks

### 7.1 Adding a Provider

- Add `src/providers/<name>.c` implementing `sc_provider_t` vtable (`chat`, `supports_native_tools`, `get_name`, `deinit`).
- Register in `src/providers/factory.c`.
- Add tests for vtable wiring, error paths, and config parsing.

### 7.2 Adding a Channel

- Add `src/channels/<name>.c` implementing `sc_channel_t` vtable.
- Keep `send`, `listen`, `name`, `is_configured` semantics consistent with existing channels.
- Cover auth/config/health behavior with tests.

### 7.3 Adding a Tool

- Add `src/tools/<name>.c` implementing `sc_tool_t` vtable (`execute`, `name`, `description`, `parameters_json`).
- Validate and sanitize all inputs. Return `sc_tool_result_t`; never crash in the runtime path.
- Add `SC_IS_TEST` guard if the tool spawns processes or opens network connections.
- Register in `src/tools/factory.c`.

### 7.4 Adding a Peripheral

- Implement the `sc_peripheral_t` interface.
- Peripherals expose `read`/`write` methods that delegate to real hardware I/O.
- Use `probe-rs` CLI for STM32/Nucleo flash access; serial JSON protocol for Arduino.
- Non-Linux platforms must return `SC_ERR_NOT_SUPPORTED` (not silent 0).

### 7.5 Security / Runtime / Gateway Changes

- Include threat/risk notes in the commit or PR.
- Add/update tests for failure modes and boundaries.
- Keep observability useful but non-sensitive (no secrets in logs or errors).

## 8) Validation Matrix

Required before any code commit:

```bash
cd build && cmake --build . -j$(nproc) && ./seaclaw_tests  # all tests must pass, 0 ASan errors
```

For release changes:

```bash
cd build && cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel -DSC_ENABLE_LTO=ON && cmake --build . -j$(nproc)  # must compile clean
```

Additional expectations by change type:

- **Docs/comments only**: no build required, but verify no broken code references.
- **Security/runtime/gateway/tools**: include at least one boundary/failure-mode test.
- **Provider additions**: test vtable wiring + graceful failure without credentials.

If full validation is impractical, document what was run and what was skipped.

### 8.1 Git Hooks

The repository ships with pre-configured hooks in `.githooks/`. Activate once per clone:

```bash
git config core.hooksPath .githooks
```

Hooks:

| Hook         | What it does                                                                        |
| ------------ | ----------------------------------------------------------------------------------- |
| `pre-commit` | Runs format checks — blocks commit if code is not formatted                         |
| `pre-push`   | Runs `cmake --build build && ./build/seaclaw_tests` — blocks push if any test fails |

To bypass a hook in an emergency: `git commit --no-verify` / `git push --no-verify`.

## 9) Privacy and Sensitive Data (Required)

- Never commit real API keys, tokens, credentials, personal data, or private URLs.
- Use neutral placeholders in tests: `"test-key"`, `"example.com"`, `"user_a"`.
- Test fixtures must be impersonal and system-focused.
- Review `git diff --cached` before push for accidental sensitive strings.

## 10) Anti-Patterns (Do Not)

- Do not add C dependencies or large Zig packages without strong justification (binary size impact).
- Do not return vtable interfaces pointing to temporaries — dangling pointer.
- Do not silently weaken security policy or access constraints.
- Do not add speculative config/feature flags "just in case".
- Do not skip `free()` — every allocation must be freed.
- Do not modify unrelated modules "while here".
- Do not include personal identity or sensitive information in tests, examples, docs, or commits.
- Do not use `SQLITE_TRANSIENT` — use `SQLITE_STATIC` (null) instead.
- Do not use `-Werror` exceptions — fix warnings at the source.

## 11) Handoff Template (Agent → Agent / Maintainer)

When handing off work, include:

1. What changed
2. What did not change
3. Validation run and results (`./build/seaclaw_tests`)
4. Remaining risks / unknowns
5. Next recommended action

## 12) Vibe Coding Guardrails

When working in fast iterative mode:

- Keep each iteration reversible (small commits, clear rollback).
- Validate assumptions with code search before implementing.
- Prefer deterministic behavior over clever shortcuts.
- Do not "ship and hope" on security-sensitive paths.
- If uncertain about C API usage, check `src/` for existing patterns before guessing.
- If uncertain about architecture, read the vtable interface definition in `include/seaclaw/` before implementing.
