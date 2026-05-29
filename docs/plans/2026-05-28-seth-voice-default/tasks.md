# Seth-Voice Default — Tasks

Each task maps to ≥1 AC; each AC is covered. `/team`-taggable column marks
worktree-isolatable units. Sizing per `agent-task-sizing.md` (N≤8 mechanical
sites/task). Build/verify against the **release** preset where the local-voice
path matters (curl ON); dev preset for everything else.

| # | Task | ACs | Parallel? | Status |
|---|------|-----|-----------|--------|
| 1 | Add `mlx_local_routing` tri-state (`OFF`/`AUTO`/`FORCE`) to config: enum in `include/human/config_types.h`, parse in `src/config_parse_agent.c` (near :390), seed default `AUTO` in `src/config_merge.c`. Keep legacy `mlx_local_enabled` working (`FORCE` ≡ old true; absent ≡ `AUTO`). | AC-1 | worktree A | pending |
| 2 | Local health probe: helper that sets `cfg->mlx_local_healthy` from (a) adapter file present + nonzero size, (b) cached MLX server reachability ping (≈60s TTL, reuse `hu_http_post_json`). Lives in `src/agent/model_router_health.c` (new) or `src/providers/from_config.c`. Never blocks the turn. `HU_IS_TEST` guards the network ping. | AC-1, AC-3 | worktree B | pending |
| 3 | Relax `maybe_override_to_mlx_local` (`src/agent/model_router.c:226`): in `AUTO`, fire when `mlx_local_healthy && mlx_local_model` regardless of explicit opt-in; `OFF` forces cloud; `FORCE` = today's behavior. Router stays pure. | AC-1 | — (after 1,2) | pending |
| 4 | Local→cloud turn fallback: extend the existing fallback path at `src/agent/agent_stream.c:230` (+ `agent_turn.c`) so a local provider error/timeout/response_guard-degenerate triggers exactly one cloud retry in the same turn, with one operator log line naming the reason. A reply is always produced. | AC-2 | — (after 3) | pending |
| 5 | Local-voice doctor check: `src/doctor/check_local_voice.c` reporting 3 distinct sub-states (adapter present + path/size, MLX server reachable, curl-enabled build); register beside existing `src/doctor/check_*.c`. | AC-3 | worktree C (after 2) | pending |
| 6 | Seed `cfg->reaction_collection.enabled = true` in `src/config_merge.c` (fresh-config default). No validator change (already whitelisted at config_validate.c:68). | AC-4 | worktree D | pending |
| 7 | Real DPO pair sourcing: verify/wire iMessage tapback + user-edit collectors to write `pair.source = "imessage_tapback"` / `"user_edit"` through `src/ml/dpo_miner.c` (source tagging exists at :340). Add source constants if missing. End-to-end fixture test asserting ≥1 real-sourced pair. | AC-5 | worktree D | pending |
| 8 | Nightly train→swap closure: confirm `hu_lora_nightly_should_run` threshold reads real-pair count (`src/ml/lora_nightly.c:46`); make the curl-OFF swap path (`src/ml/mlx_admin.c:194`) log one clear "swap skipped: build lacks curl" instead of silent `HU_ERR_NOT_SUPPORTED`. Test both curl-on attempt + curl-off log paths. | AC-6 | worktree E | pending |
| 9 | Style-affinity example selector: `src/persona/examples.c` `hu_persona_select_examples` (+ `include/human/persona.h`) gains optional `const hu_communication_style_t *`; scores by topic overlap + fingerprint distance (lowercase_ratio, abbreviation_ratio, length). NULL ⇒ byte-identical to today (pin it). Wire the caller (persona.c:4491 / agent_turn) to pass the personal-model style. | AC-7, AC-8 | worktree F | pending |
| 10 | Learned G5 length baseline: `src/agent/response_guard.c` G5 uses contact/channel baseline (personal_model.style.avg_message_length + contact stage); keep the absolute floor (commit ab6d000c) as backstop when no baseline exists. Test: Seth-normal length to a contact passes; genuine anomaly still rejected. | AC-9 | worktree G | pending |
| 11 | Per-contact shape caps: `include/human/eval/shape.h` + shape impl accept an optional learned length bound; `close` contacts may exceed the universal cap up to that bound; structural fails (markdown, AI openers) unchanged. Test: 300-char iMessage to `close` passes; same + bullet list still fails. | AC-10 | worktree G | pending |
| 12 | Live-path fidelity eval: extend `scripts/eval_fidelity_nightly.py` (or sibling) to invoke the actual router-chosen path and record `path_used` + numeric score in the verdict JSON. Headless test produces verdict with both fields. | AC-11 | worktree H (after 3) | pending |
| 13 | Blind human-eval harness: `scripts/blind_eval_export.py` (anonymized real-vs-model pairs → rateable file) + `scripts/blind_eval_ingest.py` (1–10 ratings on tone/vocab/humor/decision-style → per-dimension aggregate). Round-trip fixture test. | AC-12 | worktree I | pending |

## Dependencies
- 3 depends on 1, 2
- 4 depends on 3
- 5 depends on 2
- 7 depends on 6 (collection must be on to produce real pairs)
- 12 depends on 3 (needs the router path to choose local-or-cloud)
- 9, 10, 11, 13 independent (own worktrees)
- 8 independent of the routing chain (training side)

## Suggested dispatch waves (for /team)
- **Wave 1 (parallel):** 1, 2, 6, 8, 9, 10, 11, 13 — no cross-deps, distinct files.
- **Wave 2:** 3 (after 1,2), 7 (after 6), 5 (after 2).
- **Wave 3:** 4 (after 3), 12 (after 3).

## Coverage check
- AC-1→{1,2,3}, AC-2→{4}, AC-3→{2,5}, AC-4→{6}, AC-5→{7}, AC-6→{8}, AC-7→{9}, AC-8→{9}, AC-9→{10}, AC-10→{11}, AC-11→{12}, AC-12→{13}. Every AC covered; no orphan tasks.

## Definition of Done (per task)
- Full `human_tests` passes (0 failures, 0 ASan) — not just changed-suite.
- `/verify` returns PASS for the task's ACs.
- Positive-contract tests (not bug-pinning) per `tests-that-pin-bugs.md`.
- Test/source gate symmetry honored for any flag-gated source.
- Default-behavior changes (1, 6) prove the safe path: cloud-only users with no adapter/server see no regression; no turn left unanswered.
