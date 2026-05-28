# iMessage Dermot Humanness Recovery — Tasks

**Status:** DRAFT — pending approval.
**Builds on:** requirements.md and design.md in this directory.

## Decomposition principles

- Each task is independently revertable in one commit.
- Each task maps to ≥ 1 AC; each AC is covered by ≥ 1 task.
- Sized per `~/.claude/rules/agent-task-sizing.md`: ~50–250 LOC, 30–90 min
  per task. None exceeds the "build + test at end" budget cap.
- Components ship in order C1 → C2 → C3. Within a component, tasks marked
  `[parallel]` can run concurrently in worktrees per
  `~/.claude/rules/agent-team-os.md`.

## Tasks

| # | Task | Component | ACs | Depends on | Owner | Status |
|---|---|---|---|---|---|---|
| **T1** | In `src/agent/self_rag_inline.c` STRICT score-based abstention block (lines 508-529), remove the `snprintf(resp->modified_draft, …)` and the `resp->draft_modified = true;` lines. Keep `outcome = HU_SELF_RAG_ABSTAINED` and the `hu_self_rag_render_refusal` call (telemetry). Policy refusal at lines 486-494 stays untouched. | C1 | AC-1, AC-3 | — | implementer | pending |
| **T2** | `[parallel with T1]` In `src/agent/self_rag_atomic.c` around line 715, apply the same change as T1: stop writing `resp->modified_draft`, keep `outcome` set and `refusal_text` rendered. | C1 | AC-1 | — | implementer | pending |
| **T3** | `[parallel with T1]` In `src/agent/world_model_bridge.c:746-764` (heuristic-backend dispatcher in `hu_w11_self_rag_verify`), split the ABSTAINED branch: if `resp.refusal_text[0] != '\0'` (came from a backend, so was policy), keep propagating; if empty (would have been score-based UNKNOWN_FACT), set `src = NULL; src_len = 0;` so `*out_modified` stays NULL. | C1 | AC-1, AC-3 | — | implementer | pending |
| **T4** | In `src/agent/agent.c::hu_agent_self_rag_apply` (lines 1609-1617), add an explicit defense-in-depth guard: if `outcome == HU_W11_OUTCOME_ABSTAINED && modified`, free `modified` and ensure `*swapped_out = NULL`. Increment `self_rag_refusals_rendered` ONLY when we actually propagate the swap (i.e. policy refusal). | C1 | AC-1, AC-4 | T1, T2, T3 | implementer | pending |
| **T5** | Add `tests/test_self_rag_abstain_passthrough.c`. Pin: (a) score-based ABSTAINED → original draft unchanged, `self_rag_abstentions` incremented, `self_rag_refusals_rendered` NOT incremented; (b) policy ABSTAINED → refusal text substituted, both counters incremented; (c) HEDGED outcome → modified_draft propagated as before (regression guard). Register suite in `tests/test_main.c` per `~/.claude/rules/test-source-gate-symmetry.md`. | C1 | AC-1, AC-2, AC-3, AC-4 | T1-T4 | implementer | pending |
| **T6** | Add `tests/test_outbound_no_canned_refusal.c` — a smoke test that runs a daemon turn against an in-memory stub provider returning a banter draft, asserts the outbound reply does NOT contain the substring `"I don't have memory backing this"`. Uses the existing test-channel mechanism. | C1 | AC-11 | T1-T4 | implementer | pending |
| **T7** | Add `hu_mlx_local_probe()` to `src/providers/mlx.c`: non-blocking HTTP GET to `http://127.0.0.1:<port>/health` with 300 ms timeout. Cache result for 60 s in a `static struct { int64_t last_check_ms; bool last_result; }`. Add `bool hu_mlx_local_probe(void);` to `include/human/providers/mlx.h`. | C2 | AC-5, AC-8 | — | implementer | pending |
| **T8** | Extend `hu_model_router_config_t` in `include/human/agent/model_router.h` with `bool mlx_local_enabled; const char *mlx_local_model; size_t mlx_local_model_len;`. Update `hu_model_route` in `src/agent/model_router.c`: when `cfg->mlx_local_enabled && hu_mlx_local_probe()`, emit `cfg->mlx_local_model` for `tier == conversational || tier == reflexive`. Analytical/Deep tiers continue to consult the Gemini fields. Emit one-shot WARN on first probe-fail per process. | C2 | AC-5, AC-6, AC-7, AC-8 | T7 | implementer | pending |
| **T9** | Add config parsing in `src/config_parse.c` to populate `agent.model_router.mlx_local_enabled` and `agent.model_router.mlx_local_model` from `~/.human/config.json`. Default `mlx_local_enabled = false` so the config edit is opt-in. | C2 | AC-5 | T8 | implementer | pending |
| **T10** | Add `tests/test_model_router_mlx_local.c`. Pin: (a) `mlx_local_enabled=true && probe_returns_true` → router emits `mlx_local_model` for conversational/reflexive; (b) `probe_returns_false` → falls back to Gemini conversational/reflexive without error; (c) analytical/deep ALWAYS route to Gemini regardless; (d) one-shot WARN fires exactly once across 10 probe-fail calls. Use a test-only probe override (`hu_mlx_local_set_test_probe(bool)`). Register in `test_main.c`. | C2 | AC-5, AC-6, AC-7, AC-8 | T7, T8, T9 | implementer | pending |
| **T11** | Edit `~/.human/config.json` (NOT committed — this is operator config) to set `"mlx_local_enabled": true` and `"mlx_local_model": "seth-lora-v4-repair-20260525-071921"` under `agent.model_router`. Add the diff to a release note in this spec directory (`release-notes.md`) so the operator step is documented. | C2 | AC-5 | T9 | operator | pending |
| **T12** | Add file-local `static hu_error_t parent_guid_to_text_prefix(const char *guid, size_t guid_len, char *out, size_t cap)` in `src/channels/imessage_reply.c`. Opens `~/Library/Messages/chat.db` read-only, executes `SELECT text FROM message WHERE guid = ? LIMIT 1`, copies first `min(32, len)` chars to `out`. Returns HU_ERR_NOT_FOUND if no match. Bounded prefix per `D8`. | C3 | AC-9 | — | implementer | pending |
| **T13** | Replace the `return false;` stub of `ax_reply_tier1_cmd_r` in `src/channels/imessage_reply.c` with a real implementation: `ax_open_conversation(target, target_len)` → `parent_guid_to_text_prefix(parent_guid, ..., prefix, 32)` → `ax_find_message_group(window, prefix, 0)` → `AXUIElementPerformAction(msg_group, kAXRaiseAction)` → synthesize Cmd-R via `CGEventCreateKeyboardEvent` (use pattern from `src/channels/imessage.c:3287+`) → poll for composer field → `CGEventKeyboardSetUnicodeString` body → Return key. Return true on success, false on any failure. Stay under `#if defined(__APPLE__) && defined(HU_IMESSAGE_TAPBACK_ENABLED)` AND skip under `HU_IS_TEST` so test-stubs continue to drive the path. | C3 | AC-9 | T12 | implementer | pending |
| **T14** | `[parallel with T13]` Replace the `return false;` stub of `ax_reply_tier2_show_menu` in `src/channels/imessage_reply.c`: same setup as T13, then `AXUIElementPerformAction(msg_group, kAXShowMenuAction)` → iterate context menu items via `AXUIElementCopyAttributeValue(menu, kAXChildrenAttribute, ...)` → match `title.startswith("Reply")` (handles "Reply…" U+2026 + "Reply..." 3-dot ASCII + simple "Reply") → `AXUIElementPerformAction(menu_item, kAXPressAction)` → poll for inline composer → type body → Return. Same compile guards as T13. | C3 | AC-9 | T12 | implementer | pending |
| **T15** | In `src/channels/imessage_reply.c::hu_imessage_reply`, add a file-static `static bool warned_ax_unavailable = false;`. When BOTH Tier 1 and Tier 2 fail and we drop to Tier 3 (`flat_fallback`), emit the WARN log exactly once per process. Per `~/.claude/rules/silent-config-gated-subsystems.md`: name the cause (`reason=ax_unavailable`) and the operator remedy (grant Accessibility permission). | C3 | AC-10 | T13, T14 | implementer | pending |
| **T16** | Extend `tests/test_imessage_reply.c` (or add `tests/test_imessage_reply_threaded.c` if cleaner) with stub-driven scenarios using `hu_imessage_set_test_reply_stubs`: (a) Tier 1 success → `g_last_tier == "cmdR"`; (b) Tier 1 fail + Tier 2 success → `"ax_menu"`; (c) Both fail → `"flat_fallback"` with one WARN; (d) Repeated calls all-fail → still exactly ONE WARN (one-shot guard). Register per gate-symmetry rule. | C3 | AC-10 | T13, T14, T15 | implementer | pending |
| **T17** | Add `scripts/verify_dermot_recovery.sh`. Documents the manual recipe per `D10`: (1) ensure mlx-server is up; (2) tail `~/.human/logs/service-loop-error.log` and `~/.human/logs/imessage_action.jsonl`; (3) prompt operator to send a banter message from a test contact; (4) assert `model route:` shows `seth-lora-v4-repair`; (5) assert `tier_used` is `cmdR` or `ax_menu`; (6) assert outbound reply (extracted from chat.db) does NOT contain `"I don't have memory backing this"`; (7) assert `thread_originator_guid` is non-null on the outbound message. | cross-cutting | AC-9, AC-12, AC-13 | T11, T15 | operator | pending |
| **T18** | Add a `hu_doctor_check_imessage_threading_ratio` check in `src/doctor.c` (or whatever the doctor TU is). Reads the last 100 entries from `~/.human/logs/imessage_action.jsonl`; if `flat_fallback` ratio > 50% AND total entries ≥ 20, report a WARN. Wires this into the existing doctor health-check rollup. R4 mitigation. | cross-cutting | AC-10, R4 | T15 | implementer | pending |

## Coverage matrix (each AC has ≥ 1 task)

| AC | Tasks covering |
|---|---|
| AC-1 (verifier ABSTAINED → pass-through) | T1, T2, T3, T4, T5 |
| AC-2 (HEDGED/REWRITTEN unchanged) | T5 |
| AC-3 (POLICY refusal untouched) | T1 (no-op on policy block), T3 (branch split), T5 |
| AC-4 (telemetry preserved) | T4, T5 |
| AC-5 (conversational → mlx_local) | T7, T8, T9, T10, T11 |
| AC-6 (reflexive → mlx_local) | T8, T10 |
| AC-7 (analytical/deep stay Gemini) | T8, T10 |
| AC-8 (fallback on probe fail) | T7, T8, T10 |
| AC-9 (≥95% threaded send on live macOS) | T12, T13, T14, T17 |
| AC-10 (one-shot WARN on AX unavailable, no retry-storm) | T15, T16, T18 |
| AC-11 (no canned template substring) | T6, T17 |
| AC-12 (mlx_local serves reply on live macOS) | T11, T17 |
| AC-13 (native thread reply on live macOS) | T13, T14, T17 |

## Dependency graph

```
[C1]  T1 ─┐
      T2 ─┼── T4 ── T5 ── T6
      T3 ─┘

[C2]  T7 ── T8 ── T9 ── T10 ── T11

[C3]  T12 ─┬── T13 ─┐
           └── T14 ─┴── T15 ── T16

[cross-cutting]  T17 ── (depends on T11 + T15)
                 T18 ── (depends on T15)

Component ship order: C1 → C2 → C3 (each independently revertable).
```

## Parallelism

Tasks within each implementer group can be dispatched in parallel:

- **Sprint A (C1):** Three parallel implementers cover T1, T2, T3 → join → T4 → join → T5, T6 (parallel)
- **Sprint B (C2):** Sequential T7 → T8 → T9 → T10 → T11 (operator step)
- **Sprint C (C3):** T12 → parallel T13, T14 → T15 → T16
- **Sprint X (cross):** T17, T18 after Sprint C

Per `~/.claude/rules/agent-task-sizing.md`, none of the parallel batches
exceeds the 8-site cap — each is at most 3 sites.

## Verification gate

Before declaring spec complete:

1. All 18 tasks marked `completed` in TaskList.
2. Spawn the `verifier` agent on each component's test suite — must return
   `RESULT_verifier=PASS`.
3. Spawn the `spec-verifier` agent with this spec directory; must return
   `RESULT_spec-verifier=PASS` for every AC.
4. Manual recipe `scripts/verify_dermot_recovery.sh` executed by Seth on
   live macOS; AC-9 / AC-12 / AC-13 confirmed visually + via chat.db.
5. Full `human_tests` suite green (11,900+ tests, 0 failures, 0 ASan).
6. `~/.claude/rules/cmake-build-stale-binary.md` check — `touch` the
   edited sources before final `cmake --build`; verify `nm build/human`
   shows new symbols (`hu_mlx_local_probe`).

## Out-of-band notes

- The operator step T11 is intentionally NOT a code task. It's a config
  edit on the live machine, surfaced here so it's not forgotten and the
  release note documents what changed.
- T13 + T14 are the only LIVE-ONLY-VERIFIABLE tasks. They have unit-test
  coverage via the stub mechanism (T16), but the AX wiring's empirical
  ≥95% success rate (AC-9) can only be confirmed by running on a live
  macOS box with the conversation visible.
