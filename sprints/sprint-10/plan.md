# Sprint 10 Plan — HuLa Platform

## Header

| Field | Value |
|---|---|
| Sprint | 10 |
| Goal | Ship minimum viable HuLa bindings (Python + TypeScript), an examples gallery, an MCP bridge, and SDK v0.2.0 so an external developer can write and run a HuLa program end-to-end |
| Dates | 2026-05-17 → 2026-05-28 |
| Branch | `sprint-10-hula-platform` |
| Working directory | `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula` |
| Base SHA | `ea02b08e` |
| Risk tier | LOW-MEDIUM (additive surface area; no vtable or IR changes) |
| Stories | 6 |
| Waves | 3 (Wave 0, Wave 1, Wave 2) |
| Budget estimate | ~$20 |

---

## §1 Sequencing

```
Wave 0 (parallel — no deps)
  ├── US-10.1  SDK v0.2.0 surface         [P0 / XS / LOW]    gates all bindings
  └── US-10.4  HuLa examples gallery      [P1 / S  / LOW]    pure data, no binding dep

Wave 1 (parallel — depend on US-10.1)
  ├── US-10.2  Python ctypes bindings     [P0 / M  / LOW]
  ├── US-10.3  TypeScript/Node bindings   [P1 / M  / LOW]
  └── US-10.5  MCP bridge                [P1 / M  / MED]    also depends on US-10.2 for Python example (AC-10.5.4)

Wave 2 (sequential gate — depends on US-10.2 + US-10.3)
  └── US-10.6  CI integration             [P2 / S  / LOW]
```

Dependency rationale:
- US-10.1 introduces `hu_hula_ctx_t`, `hu_hula_ctx_create/destroy`, and `hu_hula_error_string` as
  non-inline symbols in `src/agent/hula_sdk.c`. All three bindings (US-10.2, US-10.3, US-10.5) dlsym
  these entry points at runtime; without them the bindings have no stable SDK surface to bind.
- US-10.4 has zero code dependencies — it is pure JSON + Markdown + one C test file that calls
  production symbols already present (`hu_hula_parse_json`, `hu_hula_validate`). Wave 0 parallel is
  correct.
- US-10.5 depends on US-10.1 for the context handle and on US-10.2 for the Python example
  (`bindings/python/examples/mcp_bridge.py` in AC-10.5.4). US-10.5 can be dispatched in Wave 1
  alongside US-10.2; the Python example step (Step 8 in the design) is gated internally on US-10.2
  having completed, so the implementer must confirm `human_hula` is importable before writing the
  example. If US-10.2 closes before US-10.5's Python example step is reached, no external re-wave
  is needed — they are in the same wave and the implementer self-sequences that final step.
- US-10.6 requires both `bindings/python/tests/` and `bindings/typescript/tests/` to exist and be
  green locally before CI YAML can be written and validated (AC-10.6.5). This is a hard Wave 2 gate.

---

## §2 Wave Assignments Table

### Wave 0

| Story | Risk | Key files (new/modified) | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree |
|---|---|---|---|---|---|---|---|
| US-10.1 | LOW | `include/human/hula_sdk.h` (+50), `src/agent/hula_sdk.c` (NEW +120), `CMakeLists.txt` (+1), `tests/test_hula_sdk_v2.c` (NEW +130), `bindings/sdk-changelog.md` (NEW +60) | general-purpose | AC-10.1.1–10.1.6; `cmake --preset dev` build clean; `./build/human_tests` 0 failures 0 ASan errors; v0.1.0 callers grep-confirmed unchanged | ABI backward-compat (no v0.1.0 signature changed); switch coverage for `hu_hula_error_string`; no `static inline` bloat on new non-inline symbols; `tests-that-pin-bugs` rule on `test_hula_sdk_error_string_unknown_returns_sentinel` | MANDATORY before Wave 1 dispatch — ABI surface gating two downstream stories | `sprint-10-hula/` (sprint worktree) |
| US-10.4 | LOW | `examples/hula/01-simple-call/` through `05-multi-step-pipeline/` (NEW, 10 JSON+README files), `examples/hula/README.md` (NEW), `tests/test_hula_examples.c` (NEW +130), `CMakeLists.txt` (+1) | general-purpose | AC-10.4.1–10.4.9; 6 tests pass; `./build/human_tests` green; adversarial corruption test documented | JSON structural correctness for all 5 programs; README word counts; `test-references-production-symbol` rule satisfied; no legacy files deleted; `CMakeLists.txt` edit does not conflict with US-10.1's edit | Not mandatory; run if critic raises HIGH findings | `sprint-10-hula/` (sprint worktree) |

Wave 0 pre-flight checks:
- Both implementers commit to `sprint-10-hula-platform` via `git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula add <paths> && git -C ... commit`.
- US-10.1 implementer runs `grep -r "hu_hula_sdk_" --include="*.c" --include="*.h"` across the tree to confirm zero v0.1.0 callers need changes.
- US-10.4 implementer confirms `CMakeLists.txt` `add_test` pattern matches surrounding `test_hula_golden.c` registration before committing.
- Scrum Master verifies Wave 0 commits before dispatching Wave 1: `git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula log sprint-10-hula-platform ^ea02b08e --oneline | grep -E "^[0-9a-f]+ (feat|test)\(hula"`.

### Wave 1

| Story | Risk | Key files (new/modified) | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree |
|---|---|---|---|---|---|---|---|
| US-10.2 | LOW | `bindings/python/pyproject.toml`, `human_hula/__init__.py`, `_lib.py`, `context.py`, `errors.py`, `examples/hello_hula.py`, `tests/test_bindings.py` (all NEW, ~570 LOC) | general-purpose | AC-10.2.1–10.2.6; `pip install -e bindings/python/` exits 0; `pytest bindings/python/tests/ -v` all pass; `python examples/hello_hula.py` non-empty stdout exit 0; `./build/human_tests` green | Shared library precondition check performed (Step 1); no C source modified; `test_run_json_malformed_raises_hula_error` uses `pytest.raises(HulaError)` not `result is None`; `tests-that-pin-bugs` rule; memory leak path for `result_out` buffer documented | Not mandatory for LOW-risk story; run if critic raises HIGH findings | `sprint-10-hula/` (sprint worktree) |
| US-10.3 | LOW | `bindings/typescript/package.json`, `tsconfig.json`, `src/index.ts`, `src/ffi.ts`, `src/errors.ts`, `tests/bindings.test.ts`, `examples/hello_hula.ts`, `README.md` (all NEW, ~583 LOC) | general-purpose | AC-10.3.1–10.3.6; `npm install` exits 0; `npm test` all vitest pass; `tsc --noEmit --strict` exits 0; `npx tsx examples/hello_hula.ts` non-empty stdout exit 0 | `ffi-napi` Node version range declared in `package.json`; memory loop test present (1000 iterations, RSS bounded); `HulaError.code` is string matching `hu_error_t` name; `tests-that-pin-bugs` rule on rejection test | Not mandatory; run if critic raises HIGH findings | `sprint-10-hula/` (sprint worktree) |
| US-10.5 | MEDIUM | `include/human/hula_mcp_bridge.h` (NEW +60), `src/agent/hula_mcp_bridge.c` (NEW +220), `include/human/hula_sdk.h` (+1 include line), `tests/test_hula_mcp_bridge.c` (NEW +260), `tests/fixtures/stub_mcp_server.{c,h}` (NEW ~120), `bindings/python/examples/mcp_bridge.py` (NEW +80), `CMakeLists.txt` (+2) | general-purpose | AC-10.5.1–10.5.6; 4 unit tests pass (list, exec, error-propagation, malformed-prefix); `./build/human_tests` green 0 ASan; Python example runs | AC-10.5.5 enforced: no edits to `src/mcp/*.c` or `src/agent/hula.c`; `bridge_propagates_mcp_error_does_not_succeed` assertions are `HU_ASSERT_EQ(result.ok, false)` NOT `HU_ASSERT_EQ(rc, HU_OK)` — `tests-that-pin-bugs` rule is MANDATORY here; memory ownership: `..._free` releases all bridge-allocated contexts; Step-1 spike recorded in PR | MANDATORY — MEDIUM-risk story with new public header and adversarial tests touching MCP client infrastructure | `sprint-10-hula/` (sprint worktree) |

Wave 1 pre-flight checks:
- US-10.1 Wave 0 commit confirmed on `sprint-10-hula-platform` before any Wave 1 implementer starts.
- US-10.2 implementer runs precondition check: `ls build/libhuman_hula.{so,dylib}` — if absent, BLOCK and surface to Scrum Master immediately.
- US-10.3 implementer pins `"engines": { "node": ">=18.18 <23" }` in `package.json` and uses Node 20 LTS for local testing.
- US-10.5 implementer performs Step-1 spike (read `src/mcp/` for existing stub transport) and records decision before writing any new code.
- All three Wave 1 implementers commit to `sprint-10-hula-platform` before reporting DONE. Working-tree-only DONE reports will be rejected.

### Wave 2

| Story | Risk | Key files (new/modified) | Implementer | Verifier scope | Critic scope | Aspect-panel | Worktree |
|---|---|---|---|---|---|---|---|
| US-10.6 | LOW | `.github/workflows/ci.yml` (+70 LOC YAML: `changes` preflight job + `bindings` matrix job) | general-purpose | AC-10.6.1–10.6.5; `actionlint .github/workflows/ci.yml` exits 0; `gh workflow view ci.yml` shows `bindings` job; green run on `sprint-10-hula-platform` for both `ubuntu-latest` and `macos-latest`; falsifiability test (symbol removed → job red) documented | No `continue-on-error: true`; no `if: always()`; `changes` preflight defaults to `'true'` on `dorny/paths-filter` failure; `npm ci` not `npm install`; `pytest -v` not bare `pytest`; path filter covers both `bindings/**` AND `include/human/hula_sdk.h` | Not mandatory for LOW-risk CI-only change; run if critic raises HIGH findings | `sprint-10-hula/` (sprint worktree) |

Wave 2 gate: US-10.2 DONE (verifier PASS, critic CLEAN, commit on branch) AND US-10.3 DONE (same) before US-10.6 is dispatched.

---

## §3 Implementer Commit Discipline

Every implementer MUST follow this protocol before reporting DONE:

1. Stage only sprint-scoped files:
   ```
   git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula \
     add <explicit file paths — no "git add .">
   ```

2. Commit to `sprint-10-hula-platform`:
   ```
   git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula \
     commit -m "feat(hula): <description per conventional commits>"
   ```

3. Verify the commit landed before reporting DONE. The Scrum Master checks:
   ```
   git -C /Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula \
     log sprint-10-hula-platform ^ea02b08e --oneline
   ```

4. **Working-tree-only DONE reports are rejected.** If an implementer reports DONE without a commit visible in the above log, the story is re-opened and re-dispatched. This rule exists because concurrent agents can wipe working-tree changes with `git reset --hard` between waves. Sprint 1 was lost to this exact failure mode twice.

5. Do NOT switch branches mid-sprint, run `git reset --hard`, or work outside `/Users/sethford/Projects/h-uman/.claude/worktrees/sprint-10-hula` without Scrum Master sign-off.

---

## §4 Quality Gates

A story is DONE only when ALL of the following hold — no exceptions:

### Per-story gates (enforced before story closes)

- [ ] Commit exists on `sprint-10-hula-platform` (checked via `git log` as above).
- [ ] `/verify` ran and returned `RESULT_verifier=PASS`. Reading code is not verification.
- [ ] All AC have evidence (test output, example run output, build log snippet).
- [ ] Per-story critic ran immediately after implementer reported DONE (not batched at sprint end). Returns CLEAN or LOW/INFO only — HIGH or CRITICAL findings re-open the story.
- [ ] US-10.5 and the Wave 0 US-10.1 MUST pass `/aspect-panel` before Wave 1 dispatch (US-10.1: ABI surface; US-10.5: MEDIUM-risk new public header touching MCP infrastructure).
- [ ] Full C test suite (`./build/human_tests`) shows 0 failures, 0 ASan errors after every C-touching story (US-10.1, US-10.4, US-10.5).
- [ ] `tests-that-pin-bugs.md` rule reviewed on every adversarial test. Any test whose name asserts a security/correctness contract must have assertions that FAIL when the contract is violated — not assertions that pass because the bug is present. Specifically:
  - US-10.1: `test_hula_sdk_error_string_unknown_returns_sentinel` must assert the exact sentinel literal.
  - US-10.2: `test_run_json_malformed_raises_hula_error` must use `pytest.raises(HulaError)` with `exc_info.value.code != 0`.
  - US-10.3: `runJson_rejects_with_HulaError_on_malformed_json` must use `expect(...).rejects.toThrow(HulaError)` plus `.code` assertion.
  - US-10.5: `bridge_propagates_mcp_error_does_not_succeed` must assert `result.ok == false` and `trace.error_code != HU_OK` — NOT `rc == HU_OK`.
- [ ] `test-references-production-symbol.md` rule satisfied: every `tests/test_*.c` file references at least one `hu_*` symbol from the implied production module (US-10.1: `hu_hula_ctx_create`; US-10.4: `hu_hula_parse_json`; US-10.5: `hu_hula_mcp_bridge_tool_list`).
- [ ] No new `TODO`/`FIXME` without owner and tracking issue.

### Per-sprint gate (before Sprint Review)

- [ ] All 6 stories DONE or explicitly deferred with documented reason.
- [ ] No outstanding CRITICAL critic findings on any story.
- [ ] No `REGRESSION-` tasks open.
- [ ] Sprint Review summary written at `sprints/sprint-10/review.md`.
- [ ] Sprint close tag pinned: `scripts/tag-sprint-close.sh sprint-10`.

---

## §5 Cross-Sprint Coordination

### File ownership boundaries — no overlap with active workstreams

| Story | Files touched | Active workstreams to avoid |
|---|---|---|
| US-10.1 | `include/human/hula_sdk.h`, `src/agent/hula_sdk.c` | Sprint 7/8/9 work landed on `main`; no active branch touches `hula_sdk.h` as of base SHA `ea02b08e` — confirmed before dispatch |
| US-10.5 | `src/agent/hula_mcp_bridge.c`, `include/human/hula_mcp_bridge.h` | Tech Lead correction (see stories.md §US-10.5 AC-10.5.5): implementation lives in `src/agent/`, NOT `src/hula/` (which does not exist) and NOT inside `src/mcp/` or `src/agent/hula.c`. The MCP bridge is a new file only. |
| US-10.4 | `CMakeLists.txt` (test target list) | US-10.1 also touches `CMakeLists.txt` (adds `src/agent/hula_sdk.c` to library sources). These are different list entries; a merge conflict is LOW probability but the Wave 1 implementers must `git pull` before touching `CMakeLists.txt`. |
| US-10.6 | `.github/workflows/ci.yml` | No other sprint-10 story touches `ci.yml`. The Wave 2 sequencing ensures US-10.6 is dispatched after all code stories are committed. |

### No overlap with sprints 7/8/9

- Sprints 7/8/9 work (agent regression infra, daemon DRY helpers, fix/test PRs #96–#106) is merged to `main` and tagged. Sprint 10 does not reopen any of those files.
- The MCP client (`src/mcp/`) is read by US-10.5 in the Step-1 spike but NOT modified. AC-10.5.5 explicitly forbids modifications to `src/mcp/` files.
- `src/agent/hula.c` and `include/human/agent/hula.h` are read by all parties but NOT modified by any Sprint 10 story.

---

## §6 Risks

### Risk 1 — ABI stability of SDK v0.2.0 while bindings cannot compile-check (HIGH probability of silent regression, MEDIUM impact)

**Description:** US-10.1 publishes three non-inline C symbols (`hu_hula_ctx_create`, `hu_hula_ctx_destroy`, `hu_hula_error_string`) with a documented ABI stability promise ("Stable in v0.2.0. Changes to this signature bump MAJOR"). Once US-10.2 and US-10.3 bind these symbols via ctypes / ffi-napi, any signature change to the `.c` implementation that does NOT change the header will compile clean but crash at runtime — there is no cross-language compile-time check. A reviewer in Wave 1 or Wave 2 could inadvertently modify `src/agent/hula_sdk.c` in a way that changes argument layout or return type without updating `hula_sdk.h`.

**Mitigation:**
1. The `sdk-changelog.md` (AC-10.1.5) documents each symbol's signature and ABI promise. Critic reviews must grep for signature drift: `git diff sprint-10-hula-platform ^ea02b08e -- src/agent/hula_sdk.c include/human/hula_sdk.h | grep -E "^\+.*hu_hula_ctx_create|hu_hula_ctx_destroy|hu_hula_error_string"` and confirm the `.c` definition matches the header prototype.
2. The aspect-panel for US-10.1 (mandatory per §4) includes a correctness reviewer specifically checking the symbol signatures against the ctypes argtypes declared in the US-10.2 design (`_lib.py` table: `c_void_p, c_void_p, c_int`).
3. Any Sprint 10 story that discovers a mismatch between the `.c` definition and the Python/TS argtypes must BLOCK on Scrum Master sign-off before proceeding.

### Risk 2 — ffi-napi / Node version gap (MEDIUM probability, MEDIUM impact)

**Description:** `ffi-napi` historically lags Node major releases by 3–6 months. The US-10.3 design targets Node 18–20 LTS; Node 22 is current. If the CI runner defaults to Node 22 and `ffi-napi` does not support it, the US-10.6 `bindings` matrix job will fail on a platform-version mismatch unrelated to the binding's correctness.

**Mitigation:**
1. US-10.3 must pin `"engines": { "node": ">=18.18 <23" }` in `package.json` (documented in design Step 2). The CI job (US-10.6) uses `actions/setup-node@v4` with `node-version: '20'` — hardcoded to an LTS, not `current`.
2. If `ffi-napi` fails to install or load under Node 20 during local development, the implementer falls back to Node 18.18 and documents the constraint in `bindings/typescript/README.md`.
3. This risk is entirely contained within `bindings/typescript/` — a failure blocks US-10.3 and US-10.6 but does not affect US-10.1, US-10.2, US-10.4, or US-10.5.

### Risk 3 — Stub MCP server availability for US-10.5 tests (MEDIUM probability, MEDIUM impact)

**Description:** AC-10.5.1/10.5.2 require an in-process stub MCP server for unit tests. The US-10.5 design mandates a 10-minute spike (Step 1) to determine whether `src/mcp/` already exposes a loopback transport. If it does not, the implementer must write `tests/fixtures/stub_mcp_server.{c,h}` (~80 LOC). If that stub is harder than estimated (e.g., the MCP client interface requires a full transport framing layer), the story estimate climbs from M toward L and may not close before Wave 1 ends.

**Mitigation:**
1. Step-1 spike decision must be recorded in the PR description and surfaced to Scrum Master within the first 30 minutes of US-10.5 dispatch. If the spike reveals a blocking complexity, re-estimate to L and adjust the wave schedule before the implementer writes test code.
2. The stub is isolated to `tests/fixtures/` — it is not a production dependency. If the stub cannot be made deterministic under `HU_IS_TEST` (no real process spawning, no real network), AC-10.5.1/10.5.2 must be renegotiated with the product owner rather than bypassed with a real-process test.
3. US-10.5's Python example (AC-10.5.4) depends on US-10.2. If US-10.2 does not close before the Python example step in US-10.5, the implementer skips AC-10.5.4 until US-10.2 closes, then completes it without re-wave dispatch. The Scrum Master tracks this internal dependency.

---

*Plan authored by Scrum Master. All six designs are read and incorporated. No implementation dispatched.*

`RESULT_scrum-master=PLAN_READY`
