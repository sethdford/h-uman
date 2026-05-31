# Phase E1 — Root Sprawl → Bounded Modules

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax. Dispatch ONE file-editing chip at a time (`~/.claude/rules/verify-worktree-isolation-before-fanout.md`).

**Goal:** Relocate the **101 loose `src/*.c`** files into bounded-context
directories so the module structure becomes load-bearing, driving the E0
`ROOT_BASELINE` ratchet **101 → 0**. Every chip is a behavior-preserving `git mv`
+ CMake path edit + characterization/coverage check — **no logic changes**.

**Architecture:** The loose files are an existing-but-unenforced taxonomy. Three
**cluster moves** account for 33 files (config 14, mcp 10, daemon_* 9); the
remaining ~67 **singletons** map mostly to *already-existing* context dirs
(`security/`, `eval/`, `channels/`, `voice/`, `doctor/`, `skills/`, …). A small
**composition-root** context (`src/app/`) absorbs the genuine entrypoint files
(`main.c`, `bootstrap.c`, `cli_commands.c`, …) that don't belong to a domain.

**Tech Stack:** `git mv`, `sed` for CMake path churn, `HU_TEST_*`, ASan dev build.
Public headers in `include/human/` do **not** move (contract paths stay stable);
only `src/` files relocate, so `#include "human/..."` lines are unaffected — only
CMake source paths change. This is the same low-churn property Phase-4 Part B relied on.

**Hard rule:** `daemon.c` (the 14,723-LOC god-file) is **E2's** target, not E1's.
E1 moves the *other* nine `daemon_*.c`; the `daemon.c` path relocation is the
first chip of E2 (it owns the carve). Do not touch `daemon.c` here.

---

## Relocation map (measured 2026-05-31)

### Cluster A — `config_*.c` (14) → `src/config/` (new dir)
`config.c config_getters.c config_merge.c config_migrate.c config_mutator.c config_parse.c config_parse_agent.c config_parse_behavior.c config_parse_channels.c config_parse_providers.c config_schema.c config_serialize.c config_types.c config_validate.c`
*(This is the Configuration supporting context. The narrow `hu_agent_app_config_t`
facade over `hu_config_t` is E4 Part A — E1 only co-locates the files.)*

### Cluster B — `mcp*.c` (10) → `src/mcp/` (new dir)
`mcp.c mcp_jsonrpc.c mcp_manager.c mcp_registry.c mcp_resources.c mcp_server.c mcp_tool_wrapper.c mcp_transport_http.c mcp_transport_sse.c mcp_transport_stdio.c`

### Cluster C — root `daemon_*.c` (9) → `src/daemon/` (exists, 6 files)
`daemon_cron.c daemon_follow_up_watcher.c daemon_imessage_observer.c daemon_lifecycle.c daemon_proactive.c daemon_reaction_poll.c daemon_reflection_tick.c daemon_routing.c daemon_social_tick.c`
*(NOT `daemon.c` — that's E2.)*

### Singletons → existing contexts (one chip per small group)

| Destination (exists) | Files |
|---|---|
| `src/security/` | `auth.c oauth.c net_security.c permission.c vertex_adc.c identity.c` |
| `src/eval/` | `eval.c eval_benchmarks.c eval_dashboard.c eval_judge.c` |
| `src/channels/` | `channel_adapters.c channel_catalog.c channel_loop.c channel_manager.c` |
| `src/voice/` | `voice.c voice_config.c` |
| `src/doctor/` | `doctor.c doctor_fix.c` |
| `src/skills/` | `skills.c skill_registry.c skillforge.c` |
| `src/plugins/` | `plugin_discovery.c plugin_loader.c` |
| `src/observability/` | `observability.c health.c heartbeat.c usage.c cost.c status.c` |
| `src/multimodal/` | `multimodal.c youtube.c` |
| `src/util/` (or `core/`) | `util.c json_util.c http_util.c portable_atomic.c` |
| `src/memory/` | `rag.c` |
| `src/onboard/` | `onboard.c` |

### Singletons → composition root `src/app/` (new dir)

The genuine application-entry / wiring files that aren't a domain:
`main.c main_wasi.c bootstrap.c cli_commands.c cli_ctl.c cli_evaluation.c version.c update.c platform.c capabilities.c`

### Residual — needs a per-file destination decision (one chip each)

These have a plausible home but the call deserves a moment's thought at chip time;
default to the nearest existing context, escalate only if genuinely ambiguous:
`a2a.c agent_routing.c bus.c channel_loop.c cron.c crontab.c follow_up.c hardware.c hook.c hook_pipeline.c humanness.c inspiration.c migration.c service.c session.c state.c subagent.c task_manager.c terminal.c webhook.c`
Suggested: orchestration glue (`bus.c service.c session.c state.c agent_routing.c
task_manager.c subagent.c`) → `src/agent/` or `src/daemon/`; `hook*.c` →
`src/security/`; `cron.c crontab.c` → `src/daemon/`; `terminal.c hardware.c` →
`src/peripherals/`.

---

## Task 0: Cluster move EXEMPLAR — `config_*.c` → `src/config/`

Prove the chip pattern once on the highest-value cluster, then repeat it for B/C
and each singleton group.

- [ ] **Step 1: Create the dir + move the 14 files (path-only, behavior-preserving)**

```bash
cd "$(git rev-parse --show-toplevel)"
mkdir -p src/config
git mv src/config.c src/config_getters.c src/config_merge.c src/config_migrate.c \
       src/config_mutator.c src/config_parse.c src/config_parse_agent.c \
       src/config_parse_behavior.c src/config_parse_channels.c \
       src/config_parse_providers.c src/config_schema.c src/config_serialize.c \
       src/config_types.c src/config_validate.c  src/config/
```

- [ ] **Step 2: Rewrite the CMake source paths (scripted — N=14 ≥ 9 ⇒ script per `agent-task-sizing.md`)**

```bash
sed -i '' -E 's#src/(config[A-Za-z0-9_]*)\.c#src/config/\1.c#g' src/CMakeLists.txt
git diff --stat src/CMakeLists.txt   # confirm 14 path rewrites, no content change
```

- [ ] **Step 3: Build production binary (touch first) + FULL suite**

```bash
touch src/config/*.c && cmake --build build --target human -j8 && \
  cmake --build build --target human_tests -j8 && ./build/human_tests 2>/dev/null | grep -E 'Results:'
```
Expected: `Linking C executable human`; `Results: N/N passed`, 0 failures, 0 ASan errors. The existing `test_config_parse` suite is the characterization net — it must stay green unchanged (the move is path-only). Per `ground-truth-over-proxy-signals.md`, ignore any clangd include-path alarms; trust the build.

- [ ] **Step 4: Confirm the ratchet dropped + lower the baseline**

```bash
find src -maxdepth 1 -name '*.c' | wc -l            # 101 → 87
```
Edit `scripts/check-no-new-root-files.sh`: `ROOT_BASELINE=87`. (Each E1 chip lowers it; the floor is 0.)

- [ ] **Step 5: Commit**

```bash
git add -A && git commit -m "refactor(ddd): relocate config_*.c → src/config/ (E1 cluster A; root 101→87)"
```

---

## Task 1–N: Repeat the chip for each cluster/group

For **Cluster B (`mcp/`)**, **Cluster C (root `daemon_*` → existing `src/daemon/`)**,
and each **singleton group** in the map above, apply the identical 5-step chip:

1. `mkdir -p` the dir if new; `git mv` the group.
2. `sed` the CMake paths (scripted if N≥9, manual if small).
3. `touch` moved files + build prod + full suite → `Results: N/N`.
4. Lower `ROOT_BASELINE` in `check-no-new-root-files.sh` by the group size.
5. Commit `refactor(ddd): relocate <group> → src/<ctx>/ (E1; root X→Y)`.

**Sizing (per `agent-task-sizing.md`):** one cluster or one singleton group per
chip — never "move all 67 singletons" in one dispatch. Order: A (config) → B
(mcp) → C (daemon_*) → existing-context singletons (security, eval, channels, …)
→ `src/app/` composition root → residual one-file-at-a-time decisions.

**For `src/app/`:** create the dir, move the entrypoint files, and verify the
binary still links its `main` (`./build/human --version` smoke). `main.c` is the
composition root — moving it is pure path churn, but it's the file most likely to
have a hardcoded relative path in CMake/packaging, so build the **release** preset
too (`cmake --preset release && cmake --build --preset release`) before committing.

---

## Task FINAL: Lock the root at zero

- [ ] **Step 1:** `find src -maxdepth 1 -name '*.c' | wc -l` → `0`.
- [ ] **Step 2:** In `scripts/check-no-new-root-files.sh`, set `ROOT_BASELINE=0` and flip the FAIL message to "no `.c` may live at `src/` root — every source belongs to a bounded context." The ratchet is now a hard gate.
- [ ] **Step 3:** Full suite green; commit `ci(ddd): lock src/ root at zero loose files (E1 complete)`.
- [ ] **Step 4:** Update `ARCHITECTURE.md` "Key Directories" to list `config/`, `mcp/`, `app/` as bounded contexts; update `docs/standards/engineering/bounded-contexts.md` if a new context name was coined.

---

## Self-Review

- **Spec coverage:** 101 loose files mapped — 33 in three clusters (exact lists),
  ~67 singletons to existing/new contexts, residual long-tail to per-file chips. ✓
- **No placeholders:** cluster file lists are exact (measured 2026-05-31); the
  residual list is explicitly flagged as "one chip + one decision each," not a TODO. ✓
- **Behavior preservation:** every chip is `git mv` + CMake path `sed` only;
  public headers don't move so `#include`s are untouched; each chip ends in the
  full suite + prod build. No `src/` logic edits. ✓
- **Boundary respected:** `daemon.c` (the god-file) is explicitly excluded and
  deferred to E2; only the 9 sibling `daemon_*.c` move here. ✓
- **Ratchet-driven:** each chip lowers `ROOT_BASELINE`; the final chip locks it at
  0, converting the ratchet into a permanent no-root-files gate. ✓
