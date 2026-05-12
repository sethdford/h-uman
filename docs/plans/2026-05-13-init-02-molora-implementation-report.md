---
title: "Init #02 (MoLoRA per-channel persona routing) — S2 implementation report"
created: 2026-05-13
status: shipped (S2)
parent: 2026-05-11-init-02-molora-channels.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-11-init-02-molora-channels.md
  - 2026-05-12-s1.5-critic-findings.md
  - 2026-05-11-init-04-mlx-qwen3-provider.md
  - 2026-05-11-init-11-stephanie2-prism.md
risk: medium
---

# Init #02 — MoLoRA per-channel persona routing (S2 implementation)

This is the S2 implementer's report for SOTA-2026 initiative #02. It pairs
with the design doc at `docs/plans/2026-05-11-init-02-molora-channels.md`
and the S1.5 critic-findings list at `docs/plans/2026-05-12-s1.5-critic-findings.md`.
Branch base: `sprint-2c-followups` @ `0e2f2e39`.

## TL;DR

- The MoLoRA **dispatcher** lands in `src/agent/molora_dispatcher.c`. It is
  the only in-tree caller of `HU_LORA_APPLY_MODE_STACK`.
- The **mlx_qwen3** provider now honors `STACK` — it keeps a small
  `HU_MOLORA_MAX_ACTIVE`-sized resident pool on top of the REPLACE base.
- `hu_persona_overlay_t` gains two optional fields (`lora_adapter_path`,
  `lora_adapter_id`); both are zero-init-compatible. Persona JSON parsing
  + free path were extended; rejection-on-`..` matches the provider guard.
- `hu_typing_profile_resolve` now actually dereferences its `persona`
  argument — the **HF5** critic finding is closed.
- Every in-tree call site of `hu_typing_profile_resolve` already complies
  with the HF5 pre-req (CLI typing tests pass a real `hu_persona_t`).
- 10,236 tests pass under ASan with 0 leaks. The cloud-safety contract
  (`m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`) and the
  S1.5 `path_traversal` regressions stay green.

## Scope vs. design doc

The design doc enumerates ~30 implementation files (F1–F30) including a
1,000-param MLP router, manifest parser, idle W14 training job, gateway
metrics surface, and a UI tile. **S2 deliberately ships a focused subset**
that the brief calls out, and explicitly defers the rest:

| Brief deliverable | Status | File(s) |
|---|---|---|
| MoLoRA dispatcher in `src/agent/` | shipped | `src/agent/molora_dispatcher.c`, `include/human/agent/molora_dispatcher.h` |
| Per-channel adapter routing table parsing | shipped | `include/human/persona.h`, `src/persona/persona.c` |
| At least one provider honors STACK | shipped (mlx_qwen3) | `src/providers/mlx_qwen3.c` |
| `hu_typing_profile_resolve` wired to persona (HF5) | shipped | `src/agent/typing_simulator.c` |
| `m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` GREEN | confirmed | unchanged + STACK regression added in `tests/test_provider_all.c` |
| `path_traversal` GREEN | confirmed | 46/46 pass |
| Binary size delta | reported below | — |

Deferred (with concrete file/line pointers so S3 doesn't have to re-discover):

- **MLP router (`hu_lora_router_t`).** Design doc §2.1, §5.1. Lives in
  follow-up sprint; the dispatcher currently selects by direct overlay
  lookup. Adding the router is additive — the dispatcher signature is
  the seam.
- **`HU_JOB_MOLORA_ROUTER_TRAIN` W14 idle trainer.** Design doc §3.3.
  Requires the router from the previous bullet; defers with it.
- **Manifest JSON loader + writer (`src/persona/molora.c`).** Design doc
  §2.5, §2.6 F11/F12. The overlay-based routing table replaces this for
  the dispatcher's MVP scope; a manifest is only needed once experts
  exist outside persona JSON.
- **Config block `hu_molora_config_t`.** Design doc §2.5. Until the
  daemon actually wires `hu_molora_dispatcher_apply` into
  `agent_turn.c`, there is no operator-facing knob to expose.
- **Hot-path integration in `agent_turn.c` / `agent_stream.c`.** Design
  doc F20/F21. The dispatcher exists and is unit-tested; an explicit
  follow-up sprint will call it from `agent_turn.c` once init-04's MLX
  helper protocol P2/P4 lands so the production STACK actually rebinds
  Metal kernels.
- **Gateway `metrics.molora` + UI tile.** Design doc F29/F30. Premature
  until router accuracy data exists.

The S2 brief explicitly said "STOP and report" if scope expansion is
required; the above defers are the conservative reading — every deferred
file has a clear "wait for router or wait for provider P2/P4" reason.

## Files touched

```text
$ git diff --stat (vs origin/sprint-2c-followups @ 0e2f2e39)
 CMakeLists.txt                  |   2 +
 include/human/lora.h            |  12 +++
 include/human/persona.h         |  13 +++
 src/agent/typing_simulator.c    |  64 +++++++++++++-
 src/persona/persona.c           |  34 ++++++++
 src/providers/mlx_qwen3.c       | 118 +++++++++++++++++++++++---
 tests/test_channel_cli.c        |  25 ++++--
 tests/test_main.c               |   2 +
 tests/test_mlx_qwen3_provider.c | 182 +++++++++++++++++++++++++++++++++++++++-
 tests/test_provider_all.c       |   7 ++
 tests/test_typing_simulator.c   |  86 ++++++++++++++++++-
 11 files changed, 517 insertions(+), 28 deletions(-)

Untracked:
 include/human/agent/molora_dispatcher.h   (116 LOC)
 src/agent/molora_dispatcher.c             (120 LOC)
 tests/test_molora_dispatcher.c            (388 LOC)
```

Public-surface changes:

1. **`include/human/lora.h`** — adds `HU_MOLORA_MAX_ACTIVE` (3) and
   `HU_MOLORA_MAX_SLOTS` (8) next to the existing `hu_lora_apply_mode_t`
   so providers that honor `STACK` share one cap. The S1.5 hoist comment
   (single source of MoLoRA constants) is now accurate.

2. **`include/human/persona.h`** — `hu_persona_overlay_t` gets
   `char *lora_adapter_path` and `char *lora_adapter_id`. Zero-init
   means "no channel expert"; tests and existing personas keep working.

3. **`include/human/agent/molora_dispatcher.h`** — new public surface.
   One entry point: `hu_molora_dispatcher_apply(...)`. Returns
   `HU_ERR_NOT_SUPPORTED` honestly for cloud providers so the daemon
   fallthrough contract pins.

Provider behaviour changes:

1. **`src/providers/mlx_qwen3.c`** — adds an `HU_MOLORA_MAX_ACTIVE`-sized
   stacked-adapter pool. REPLACE wipes the pool atomically; STACK
   appends; chat-mock formats `[mlx_qwen3:<base>+<expert1>+<expert2>]`
   so init-05 fidelity scoring can pin the contract without real Metal.
   STACK without an active base is rejected (no implicit-base fallback).

2. **`src/persona/persona.c`** — `parse_overlay` reads the two new
   fields. Embedded `..` in `lora_adapter_path` is rejected at load
   time. `free_overlay` releases them.

3. **`src/agent/typing_simulator.c`** — `hu_typing_profile_resolve` now
   dereferences `persona` (when `HU_ENABLE_PERSONA`), calls
   `hu_persona_find_overlay`, and maps `formality` / `avg_length` to
   profile knobs. NULL persona / NULL channel / missing overlay all
   keep the legacy defaults — covered by tests in the Typing suite.

Test surface changes:

1. **`tests/test_channel_cli.c`** — every `hu_cli_set_persona` call now
   passes a real `hu_persona_t *` (was an int sentinel; HF5 pre-req).
2. **`tests/test_mlx_qwen3_provider.c`** — drops the STACK→NOT_SUPPORTED
   assertion, adds 4 STACK lifecycle tests + 1 chat-mock pin (total +5
   tests when `HU_ENABLE_MLX_QWEN3=ON`).
3. **`tests/test_provider_all.c`** — `load_adapter_check_not_supported`
   gains a STACK assertion so every cloud provider continues to
   short-circuit before the dispatcher can do harm.
4. **`tests/test_typing_simulator.c`** — 4 new persona-driven resolver
   tests pin HF5 closure.
5. **`tests/test_molora_dispatcher.c`** — 10 new dispatcher tests
   (NULL guards, cloud fallthrough, mlx_qwen3 happy path, overlay
   precedence, replace-only fake for huml/llamacpp semantics).

## Test count delta

| Suite | Before | After | Delta |
|---|---|---|---|
| mlx_qwen3 provider | 18 | 23 | +5 |
| Provider All | unchanged | unchanged (STACK case folded into existing helper) | 0 (assertion grew) |
| Typing | 18 | 22 | +4 |
| MoLoRA dispatcher | 0 | 10 | +10 |
| **Total tests** | 10,217 | 10,236 | **+19** |

Full suite: `10236/10236 passed` under ASan dev build. No skips beyond
the standard `wasm WASI` build-gated skip.

## Cloud-safety regression result

Both regressions stay GREEN:

```text
$ ./build/human_tests --filter=m3_daemon_pattern_cloud_provider_falls_through_to_base_chat
--- Results: 16/16 passed, 10220 skipped ---

$ ./build/human_tests --filter=path_traversal
--- Results: 46/46 passed, 10190 skipped ---
```

The new `load_adapter_check_not_supported` helper now asserts both
REPLACE and STACK return `HU_ERR_NOT_SUPPORTED` on cloud providers
(openai, anthropic, gemini, ollama, openrouter). This is a tighter
pin than the brief required — it guards against a future provider
that "softly supports" STACK silently breaking the daemon
fallthrough.

## HF5 closure confirmation

The S1.5 audit (`docs/plans/2026-05-12-s1.5-critic-findings.md`)
flagged that `hu_typing_profile_resolve` advertised an overlay
lookup but ignored its `persona` argument. CLI typing tests passed
`int` sentinels into `hu_cli_set_persona`, which would dereference
to garbage the moment init-02 wired the real lookup.

Evidence of closure:

1. **Test patch**: `tests/test_channel_cli.c` now declares a
   `static hu_persona_t s_test_persona;` and passes its address to
   every `hu_cli_set_persona` call (replaces three `int sentinel` sites).
2. **Resolver patch**: `src/agent/typing_simulator.c::hu_typing_profile_resolve`
   casts `persona` to `const hu_persona_t *` (guarded by
   `HU_ENABLE_PERSONA`), calls `hu_persona_find_overlay`, and maps
   `formality`/`avg_length` fields to typing-profile knobs.
3. **Test pin**:
   - `test_typing_profile_resolve_formal_overlay_slows_typing` —
     formal overlay ⇒ WPM strictly < 65, pause strictly > 420 ms.
   - `test_typing_profile_resolve_casual_short_overlay_speeds_typing` —
     casual+short overlay ⇒ WPM strictly > 65, pause strictly < 420 ms.
   - `test_typing_profile_resolve_returns_defaults` — NULL persona
     still yields canonical defaults (legacy CLI path).
   - `test_typing_profile_resolve_null_channel_returns_defaults` —
     real persona but NULL channel ⇒ defaults.
   - `test_typing_profile_resolve_missing_overlay_returns_defaults` —
     real persona with no overlays ⇒ defaults.

If the resolver regresses to `(void)persona;`, the two non-default
tests fail with concrete numeric mismatches.

## Binary size delta

Measured by `git stash --include-untracked` → rebuild → diff.

| Target | Build flags | Before | After | Delta |
|---|---|---|---|---|
| `build-release/human` | `MinSizeRel + HU_ENABLE_LTO=ON` | 2,246,800 B | 2,246,800 B | **+0 B** |
| `build/human` | ASan dev (`HU_ENABLE_ASAN=ON`) | 18,452,576 B | 18,452,992 B | **+416 B** |
| `build/human_tests` | ASan dev | 59,658,760 B | 59,748,136 B | **+89,376 B** |

Discussion. The shipping binary (`build-release/human`) sees a
`+0 B` delta because the new dispatcher symbol has no in-tree caller
in `human` main yet; LTO dead-code-eliminates it. The ASan dev
`human` binary (no LTO, retains the dispatcher symbol via direct
reference from no production caller) still only grows by 416 bytes.

The test binary grows by ~87 KB, which is over the 50 KB soft target,
but that delta is entirely test code (`tests/test_molora_dispatcher.c`
is 388 LOC, plus the new STACK tests in
`tests/test_mlx_qwen3_provider.c` and new typing tests). No
production code is in that 87 KB. Reporting the breakdown faithfully:

- 28,936 B — `tests/test_molora_dispatcher.c.o` (new)
- 8,176 B — added test code in `test_mlx_qwen3_provider.c.o` (STACK tests)
- 6,792 B — added test code in `test_typing_simulator.c.o` (HF5 tests)
- 416 B — production code (the human-binary delta above)
- 45,056 B — ASan instrumentation amplification + test-framework boilerplate fixed costs

The brief asked the size to be reported; the production target is the
0 KB / +0.4 KB number, well under the soft cap. The test-binary number
is reported for transparency.

## Anything deferred to S3

See "Scope vs. design doc" above. The defers in summary:

1. **MLP router (`hu_lora_router_t`).** Design doc §2.1–§2.2, §5.1.
   Dispatcher currently selects by direct overlay lookup; adding the
   router replaces the `hu_persona_find_overlay → spec` step in
   `src/agent/molora_dispatcher.c:80-105` without changing the public
   signature. **Trigger**: S3 sprint with the brief that includes
   "router blob + training", or once init-05 / init-06 produce real
   per-expert fidelity labels.
2. **W14 `HU_JOB_MOLORA_ROUTER_TRAIN` runner.** Design doc §2.4, §5.2.
   Same trigger as the router. The scheduler enum extension in
   `include/human/agent/scheduler.h` was NOT made (touch is reserved
   for the sprint that ships the runner — adding an unused enum value
   today would violate YAGNI and the persona-subsystem registry
   would have to NULL-handle it).
3. **Manifest JSON loader (`src/persona/molora.c`).** Design doc §2.5,
   F11/F12. Until experts exist outside the persona JSON, the overlay
   carries the route. **Trigger**: when a single persona starts
   carrying so many overlays that the manifest's `kind: macro_mode` /
   `kind: channel` typing pays for itself.
4. **`hu_molora_config_t`.** Design doc §2.5. No operator-facing knob
   to expose until the dispatcher is on the hot path.
5. **Hot-path integration in `agent_turn.c` / `agent_stream.c`.**
   Design doc F20/F21. Dispatcher is unit-tested in isolation; wiring
   into `agent_turn.c::agent_turn_run` requires init-04 P2/P4 (real
   MLX helper protocol) so the STACK actually triggers Metal kernel
   rebinding instead of merely tracking the slot. **Trigger**: init-04
   P2/P4 landing OR a follow-up sprint that's explicit about
   "wire-without-real-mixture" being acceptable.
6. **Gateway `metrics.molora` and UI tile.** Design doc F29/F30. Defer
   until router exists (no router_accuracy without router).
7. **`llamacpp` and `huml` STACK support.** Design doc §1.1 lists both
   as future. `llamacpp` already takes an array in its modern API and
   could be wired in one ~30 LOC delta in `src/providers/llamacpp.c`;
   `huml` would need a real mixture pass in its inference layer.
   Both continue to return `HU_ERR_NOT_SUPPORTED` and the dispatcher's
   `channel_expert_skipped` flag flips so the agent stays on the base
   adapter — this is the documented graceful degrade.

No "trust me" defers — every item above names the file or line that
needs to change.

## Quality bar checklist

- [x] `cmake --build build && ./build/human_tests` → 10,236/10,236 passed, 0 ASan errors
- [x] STACK on mlx_qwen3 test: `tests/test_mlx_qwen3_provider.c::test_mlx_qwen3_stack_appends_after_replace`
- [x] Channel-overlay selection test: `tests/test_molora_dispatcher.c::test_dispatcher_mlx_qwen3_loads_base_and_stacks_channel`
- [x] Cloud STACK NOT_SUPPORTED test: `tests/test_provider_all.c::load_adapter_check_not_supported` (STACK arm)
- [x] HF5 (`hu_typing_profile_resolve` uses persona): `tests/test_typing_simulator.c::test_typing_profile_resolve_formal_overlay_slows_typing`
- [x] `m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` GREEN
- [x] `path_traversal` filter GREEN
- [x] AGENTS.md / CLAUDE.md discipline followed (KISS, YAGNI, vtable-first, no `SQLITE_TRANSIENT`, `HU_IS_TEST` guards, free every alloc)
- [x] Binary size reported (production: +0 B, ASan dev: +416 B, ASan test: +87 KB)

## Hand-off

- All work staged via `git add` (no commit, no push, no branch switch).
- Worktree: `/Users/sethford/.cursor/worktrees/init-02-molora-c0498688/h-uman-a65c53c0c379`
- Worktree id: `init-02-molora-c0498688`
- Worktree start ref: `HEAD` (sprint-2c-followups @ 0e2f2e39)
- Parent agent reviews diff and decides commit / push / squash.
- Next reasonable sprint S3 targets: (a) hot-path integration into
  `agent_turn.c` once init-04 P2/P4 is in, or (b) MLP router + W14
  runner if init-05 fidelity labels are landing first.
