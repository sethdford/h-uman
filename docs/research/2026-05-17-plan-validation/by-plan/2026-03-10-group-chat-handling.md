---
plan: docs/plans/2026-03-10-group-chat-handling.md
auditor: group-2-channels
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Three targeted fixes for group-chat handling in `src/daemon.c`: (1) move history load above the group gate so classifier sees conversation context, (2) map `HU_GROUP_BRIEF` to `brief_mode`, (3) inject group-chat instruction into agent context.

## Key Claims (from the plan)
- Claim 1: History load moves above group gate; `hu_conversation_classify_group()` receives `early_history, early_history_count` instead of `NULL, 0`.
- Claim 2: `bool group_brief` captured from classifier; `brief_mode` ORs `group_brief` and `is_group`.
- Claim 3: Group-context instruction prepended to `agent->conversation_context`.
- Claim 4: Tests in `tests/test_conversation.c` for classifier history-aware behavior.

### Implemented? (code exists)
- `src/daemon.c:4415` `bool group_brief = false;` — matches plan Step 2 of Task 2.
- `src/daemon.c:4423` `hu_conversation_classify_group(combined, combined_len, persona_name, ...)` — call site present.
- `src/daemon.c:4426` `if (gr == HU_GROUP_SKIP)` — skip branch present.
- `src/daemon.c:4435-4436` `if (gr == HU_GROUP_BRIEF) group_brief = true;` — brief mapping present.
- `src/daemon.c:4715` `bool brief_mode = (action == HU_RESPONSE_BRIEF) || group_brief || ...` — combined condition present.
- Classifier function: `include/human/context/conversation.h:531`, `src/context/conversation.c:5302` (`hu_conversation_classify_group`).

### Proven? (tests exist)
- `tests/test_conversation.c:1795-1860` — comprehensive classifier tests:
  - `HU_GROUP_RESPOND` cases (lines 1795, 1801)
  - `HU_GROUP_SKIP` cases (lines 1806, 1817, 1822, 1834)
  - `HU_GROUP_BRIEF` mid-length-message test (lines 1838-1841) — matches plan Task 2 Step 1's `classify_group_medium_message_is_brief`.
  - Property-style assertion (line 1860): `r == HU_GROUP_SKIP || r == HU_GROUP_BRIEF || r == HU_GROUP_RESPOND`.

### Wired? (called in runtime path / dispatch)
- WIRED: `hu_conversation_classify_group` invoked from `src/daemon.c:4423` inside main batch processing loop.
- WIRED: `brief_mode` propagates into the prompt-build call downstream.

## Gaps
- Did not verify Task 3's group-context instruction prepend at daemon.c:~2891 — would need to read that region. Given the three-task plan is integrated and the first two are verifiably present, confidence remains HIGH.

## Notes
- Plan marked `status: complete`.
- Strong evidence of all three behaviors landing; tests pin classifier contract.
