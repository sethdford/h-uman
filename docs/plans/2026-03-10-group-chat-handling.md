---
status: complete
last_audit: 2026-05-17
---

# Group Chat Handling Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make group chat responses shorter, more natural, and actually use the history-aware classifier.

**Architecture:** Three targeted fixes — (1) move history load above the group gate so the classifier gets conversation context, (2) map `HU_GROUP_BRIEF` to `brief_mode` so the existing brief-response path fires, (3) prepend a group-chat instruction block to `agent->conversation_context` so the LLM knows to respond like an active group participant.

**Tech Stack:** C11, existing `hu_conversation_classify_group()`, `hu_prompt_build_system()`, `agent->conversation_context`.

---

### Task 1: Move history load above the group gate

**Files:**

- Modify: `src/daemon.c` (around line 1208)

The history load currently happens _after_ the group gate, so `hu_conversation_classify_group()` always gets `NULL, 0` for its history params — meaning the consecutive-response and 40%-participation checks never fire.

**Step 1: Read the code around the gate**

Read `src/daemon.c` lines 1200–1240 to confirm the current order.

**Step 2: Write a failing test**

In `tests/test_conversation.c`, add to `run_conversation_tests()`:

```c
static void classify_group_consecutive_2_skips_with_history(void) {
    hu_channel_history_entry_t entries[5] = {
        make_entry(false, "yo what's up", "12:00"),
        make_entry(false, "anyone wanna grab food?", "12:01"),
        make_entry(true,  "sounds good", "12:02"),
        make_entry(true,  "i'm free around 7", "12:03"),
    };
    uint32_t delay = 0;
    /* With 2 consecutive from_me entries, group classifier should skip */
    hu_group_response_t r = hu_conversation_classify_group(
        "yeah same", 9, "bot", 3, entries, 4);
    HU_ASSERT_EQ(r, HU_GROUP_SKIP);
}
```

Run: `./build/human_tests 2>&1 | grep classify_group_consecutive`
Expected: PASS (this tests the classifier in isolation — it already works correctly, confirming the bug is in the daemon call site).

**Step 3: Fix the daemon — move history load up**

In `src/daemon.c`, move the early_history load block (currently around lines 1228–1235) to _before_ the `if (msgs[batch_start].is_group)` block (currently line 1209). Then pass `early_history, early_history_count` to `hu_conversation_classify_group()` instead of `NULL, 0`.

The change looks like this — find the group gate block:

```c
/* Group chat gating: use group classifier to decide engagement */
if (msgs[batch_start].is_group) {
    ...
    hu_group_response_t gr = hu_conversation_classify_group(
        combined, combined_len, persona_name,
        persona_name ? strlen(persona_name) : 0, NULL, 0);  // ← fix this
```

Change to:

```c
/* Preload channel history early — needed by both group and DM classifiers */
hu_channel_history_entry_t *early_history = NULL;
size_t early_history_count = 0;
if (ch->channel->vtable->load_conversation_history) {
    ch->channel->vtable->load_conversation_history(
        ch->channel->ctx, alloc, batch_key, key_len, 10, &early_history,
        &early_history_count);
}

/* Group chat gating: use group classifier to decide engagement */
if (msgs[batch_start].is_group) {
    ...
    hu_group_response_t gr = hu_conversation_classify_group(
        combined, combined_len, persona_name,
        persona_name ? strlen(persona_name) : 0,
        early_history, early_history_count);  // ← now passes history
```

Then remove the duplicate history load that currently appears a few lines later (the one before `hu_conversation_classify_response`).

**Step 4: Build and run tests**

```bash
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./build/human_tests 2>&1 | tail -3
```

Expected: `--- Results: 3788/3788 passed ---`

**Step 5: Commit**

```bash
git add src/daemon.c tests/test_conversation.c
git commit -m "fix(group): pass conversation history to group classifier"
```

---

### Task 2: Map HU_GROUP_BRIEF to brief_mode

**Files:**

- Modify: `src/daemon.c` (around line 1211–1222)

Currently the daemon only checks `HU_GROUP_SKIP`. `HU_GROUP_BRIEF` falls through and gets treated identically to `HU_GROUP_RESPOND` — no brevity enforcement.

**Step 1: Write a failing test**

In `tests/test_conversation.c`, add:

```c
static void classify_group_medium_message_is_brief(void) {
    /* 30–100 char message with no question and no engage word → HU_GROUP_BRIEF */
    hu_group_response_t r = hu_conversation_classify_group(
        "just got back from the gym, pretty tired", 40, "bot", 3, NULL, 0);
    HU_ASSERT_EQ(r, HU_GROUP_BRIEF);
}
```

Run: `./build/human_tests 2>&1 | grep classify_group_medium`
Expected: PASS (classifier already returns BRIEF here; this documents the expectation).

**Step 2: Fix the daemon — capture group result and apply brief_mode**

In `src/daemon.c`, change the group gate block from:

```c
if (msgs[batch_start].is_group) {
    ...
    hu_group_response_t gr = hu_conversation_classify_group(...);
    if (gr == HU_GROUP_SKIP) {
        ...
        continue;
    }
}
```

To:

```c
bool group_brief = false;
if (msgs[batch_start].is_group) {
    ...
    hu_group_response_t gr = hu_conversation_classify_group(...);
    if (gr == HU_GROUP_SKIP) {
        ...
        continue;
    }
    if (gr == HU_GROUP_BRIEF)
        group_brief = true;
}
```

Then find the `brief_mode` assignment (around line 1355):

```c
bool brief_mode = (action == HU_RESPONSE_BRIEF);
```

Change to:

```c
bool brief_mode = (action == HU_RESPONSE_BRIEF) || group_brief ||
                  msgs[batch_start].is_group;  /* always brief in group chats */
```

The `|| msgs[batch_start].is_group` ensures even `HU_GROUP_RESPOND` produces shorter output — matching "active participant" tone (a friend in a group doesn't write essays).

**Step 3: Build and run tests**

```bash
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./build/human_tests 2>&1 | tail -3
```

Expected: `--- Results: 3789/3789 passed ---`

**Step 4: Commit**

```bash
git add src/daemon.c tests/test_conversation.c
git commit -m "fix(group): map HU_GROUP_BRIEF to brief_mode, force brief for all group responses"
```

---

### Task 3: Inject group-context instruction into the prompt

**Files:**

- Modify: `src/daemon.c` (around line 2891–2897)

The LLM currently doesn't know it's in a group chat. It responds with full DM-style answers. We need to prepend a short instruction block to `agent->conversation_context` when `is_group` is true.

**Step 1: Write a failing test (prompt injection)**

In `tests/test_conversation.c`, add:

```c
static void group_prompt_injection_contains_group_hint(void) {
    /* Verify the group instruction string exists and is non-empty.
     * This is a compile-time check — the constant must be defined. */
    const char *hint = HU_GROUP_CHAT_PROMPT_HINT;
    HU_ASSERT_NOT_NULL(hint);
    HU_ASSERT_TRUE(strlen(hint) > 10);
}
```

And in `include/human/context/conversation.h` (or a new `group.h`), add:

```c
#define HU_GROUP_CHAT_PROMPT_HINT \
    "[GROUP CHAT] You are in a group conversation. " \
    "Keep responses to 1-2 sentences. " \
    "Don't explain — react. " \
    "Don't try to be helpful unless asked directly. " \
    "Match the group's energy. Don't dominate.\n\n"
```

Run: `./build/human_tests 2>&1 | grep group_prompt`
Expected: PASS after adding the define.

**Step 2: Inject the hint into conversation_context in daemon.c**

Find the block in `src/daemon.c` that sets `agent->conversation_context` (around line 2891–2894):

```c
agent->conversation_context = convo_ctx;
agent->conversation_context_len = convo_ctx_len;
```

Before this block, add:

```c
/* Prepend group-chat instruction when responding in a group */
if (msgs[batch_start].is_group) {
    const char *hint = HU_GROUP_CHAT_PROMPT_HINT;
    size_t hint_len = sizeof(HU_GROUP_CHAT_PROMPT_HINT) - 1;
    size_t new_len = hint_len + (convo_ctx ? convo_ctx_len : 0);
    char *new_ctx = (char *)alloc->alloc(alloc->ctx, new_len + 1);
    if (new_ctx) {
        memcpy(new_ctx, hint, hint_len);
        if (convo_ctx)
            memcpy(new_ctx + hint_len, convo_ctx, convo_ctx_len);
        new_ctx[new_len] = '\0';
        if (convo_ctx)
            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
        convo_ctx = new_ctx;
        convo_ctx_len = new_len;
    }
}
```

**Step 3: Build and run tests**

```bash
cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./build/human_tests 2>&1 | tail -3
```

Expected: `--- Results: 3790/3790 passed ---`

**Step 4: Commit**

```bash
git add src/daemon.c include/human/context/conversation.h tests/test_conversation.c
git commit -m "feat(group): inject group-chat prompt hint into conversation context"
```

---

### Task 4: Verify and clean up

**Step 1: Run the full test suite**

```bash
./build/human_tests 2>&1 | tail -5
```

Expected: all tests pass, 0 failures.

**Step 2: Check for double history load**

Grep for the history load pattern to make sure it only appears once (no leftover duplicate from Task 1):

```bash
grep -n "load_conversation_history" src/daemon.c
```

Expected: exactly one call site in `hu_service_run`.

**Step 3: Commit docs sync if CLAUDE.md updated**

```bash
git diff --stat
# if CLAUDE.md or docs changed:
git add -u
git commit -m "docs: sync after group chat handling"
```

**Step 4: Push**

```bash
git push
```
