# Seth-Aliveness — Tasks

Each task maps to ≥1 AC; each AC is covered. Worktree column marks isolation units.
Sizing per `agent-task-sizing.md`. Build/verify against the **dev** preset (ASan).
Every default-behavior change ships its "no-change" safe-default test (per
`tests-that-pin-bugs.md` — positive contract, not bug-pinning).

| # | Task | ACs | Worktree | Status |
|---|------|-----|----------|--------|
| 1 | Add `hu_personal_model_build_prompt_for_contact(model, contact_handle, buf, cap)` to `src/memory/personal_model.c` + header. Factor the emotional/anticipatory/causal walk (`personal_model.c:800-870`) to filter to `contact_handle` when non-NULL; existing `hu_personal_model_build_prompt` delegates with NULL (broadcast preserved). Tests: X-in/Y-out for a contact; NULL ⇒ byte-identical broadcast. | AC-B1 | A (memory) | pending |
| 2 | Switch the live reply path `src/agent/autoresponder.c:493` to call the new `_for_contact` builder with the in-scope `contact_handle`. Test: autoresponder prompt for X excludes Y's emotional context end-to-end. | AC-B1 | A (memory) | pending |
| 3 | Source `temporal_decay_factor` from config with a conservative nonzero default at `src/agent/memory_loader.c:167` (decay already applied at `engine.c:62`). Add the config key + parse + merge default. Tests: recent>old at equal salience after decay; `0.0` ⇒ today's ordering (pinned rollback). | AC-B2 | A (memory) | pending |
| 4 | Populate `hu_init_proposer` context slots (`init_proposer.c:189-193`): render the target contact's `build_prompt_for_contact` into a proposer scratch buffer → `content/bytes[HU_INIT_FIELD_PERSONAL_MODEL]`; summarize `agent->memory` → `[MEMORY]`; format local time-of-day → `[AWARENESS]`. Tests: populated model ⇒ nonzero PERSONAL_MODEL+AWARENESS bytes; **target-contact X's tokens present AND a different contact Y's private context absent in the rendered PERSONAL_MODEL slot** (CHI-2025 grounding-reaches-generation contract); empty ⇒ byte-identical. | AC-A1 | B (initiative) | pending |
| 5 | Add optional `parent_guid` to the scheduled-message record (`sched_queue`, `src/context/conversation.c:8110`) + additive `hu_conversation_schedule_threaded_message_on(...)` variant (old `schedule_message_on` delegates NULL) + return parent_guid from `hu_conversation_flush_scheduled_for` (new out-param). Header updates. Tests: round-trip a scheduled record's parent_guid through flush; NULL stays NULL. | AC-C1 | C (imessage) | pending |
| 6 | Wire the daemon flush site (`src/daemon.c:1523`) to thread when the flushed record carries a parent_guid (`vtable->reply`) else flat `vtable->send` (today). Wire the follow-up watcher to schedule via the threaded variant carrying the unreplied message's guid. Test: scheduled record with parent ⇒ threaded send call; without ⇒ flat. | AC-C1 | C (imessage) | pending (after 5) |
| 7 | Add a banter signal to `hu_followup_input_t` (`include/human/follow_up.h`) + register-aware fast path in `hu_followup_compute_send_time` (`src/follow_up.c:39`): banter ⇒ scaled-down base delay (jitter + chronotype-snap + 24h-cap retained); unset ⇒ today's schedule. Set the signal at the daemon follow-up call site from incoming message register. Tests: casual one-word ⇒ shorter delay than substantive; quiet-hours snap on both; unset field ⇒ byte-identical. | AC-C2 | C (imessage) | pending |

## Dependencies
- 2 depends on 1 (needs the new builder).
- 4 depends on 1 (reuses `build_prompt_for_contact`).
- 6 depends on 5 (needs the threaded schedule API + flush out-param).
- 3, 7 independent.

## Suggested phase order (own worktree each, merged when peer quiet)
- **Phase A — memory (worktree A):** 1 → 2, and 3 (parallel within A).
- **Phase B — initiative (worktree B):** 4 (after 1 merges, or cherry-pick the builder).
- **Phase C — iMessage (worktree C):** 5 → 6, and 7 (parallel within C).

## Coverage check
- AC-A1→{4}; AC-B1→{1,2}; AC-B2→{3}; AC-C1→{5,6}; AC-C2→{7}. Every AC covered; no orphan tasks.

## Definition of Done (per task)
- Full `human_tests` (12,993+) passes — 0 failures, 0 ASan, 0 leaks.
- `/verify` returns PASS for the task's ACs (runs the code, not just reads it).
- Positive-contract test + the safe-default "no-change" test for every default-behavior change.
- Test/source gate symmetry honored for any flag-gated source.
- Touch source before rebuilding the production binary (`cmake-build-stale-binary.md`).
- Worktree branches merged before any cleanup (`worktree-merge-before-cleanup.md`).
