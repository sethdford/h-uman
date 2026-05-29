---
title: Channel Testing Checklist
---

# Channel Testing Checklist

Standards for testing the messaging channel implementations in the human runtime.

**Cross-references:** [../engineering/testing.md](../engineering/testing.md), [../engineering/naming.md](../engineering/naming.md)

---

## Vtable Contract

Every channel must implement the `hu_channel_vtable_t`:

- Required: `start`, `stop`, `send`, `name`, `health_check`
- Optional: `send_event`, `start_typing`, `stop_typing`, `load_conversation_history`, `get_response_constraints`, `react`

## Required Tests (every channel)

| Test Category               | What to Assert                                                | Example Test Name                         |
| --------------------------- | ------------------------------------------------------------- | ----------------------------------------- |
| Creation                    | Factory creates valid channel with correct name               | telegram_create_returns_valid_channel     |
| Name                        | name() returns the expected string                            | telegram_name_returns_telegram            |
| Health check (unconfigured) | health_check returns false without credentials                | telegram_health_check_false_without_token |
| Send (test mode)            | send() succeeds in HU_IS_TEST mode with mock buffer           | telegram_send_stores_in_mock_buffer       |
| Send (missing params)       | send() returns error on NULL target/message                   | telegram_send_rejects_null_target         |
| Start/Stop                  | start/stop cycle completes without crash or leak              | telegram_start_stop_cycle                 |
| Cleanup                     | destroy frees all resources; tracking allocator shows 0 leaks | telegram_destroy_frees_all                |

## Optional Method Tests (if implemented)

- send_event: chunk and final stages both work
- start_typing/stop_typing: no crash, returns HU_OK in test mode
- load_conversation_history: returns entries in chronological order; handles empty history
- get_response_constraints: returns valid max_chars for the platform
- react: valid reaction types accepted; invalid types return error

## HU_IS_TEST Mocking Pattern

All channels should follow this pattern for test isolation:

```c
// In channel struct:
#if HU_IS_TEST
    char last_message[4096];
    size_t last_message_len;
    hu_channel_message_t *mock_msgs;
    size_t mock_count;
#endif

// In send():
#if HU_IS_TEST
    memcpy(ctx->last_message, message, message_len);
    ctx->last_message_len = message_len;
    return HU_OK;
#endif
```

## Destroy Signature Convention

All channels must use: `void hu_<name>_destroy(hu_channel_t *ch)`

- The channel struct is accessed via `ch->ctx`
- Free all internal allocations, then free the ctx itself
- Safe to call with NULL (no-op)
- If the channel holds an allocator reference, it should be passed at create time and stored in ctx -- not passed to destroy

## Memory Testing

- Every channel test should use `hu_tracking_allocator_t`
- Assert `hu_tracking_allocator_leaks(ta) == 0` at end of test
- Test the error-path cleanup (create with invalid config, verify no leaks)

---

## Anti-Patterns

```c
// WRONG -- test depends on real network
void test_telegram_send(void) {
    hu_channel_t ch;
    hu_telegram_create(&alloc, real_token, token_len, &ch);
    hu_channel_send(&ch, "chat123", 6, "Hello", 5);  /* hits real API */
}

// RIGHT -- HU_IS_TEST guard stores in mock buffer; no network
void test_telegram_send(void) {
    hu_channel_t ch;
    hu_telegram_create(&alloc, "test:token", 10, &ch);
    hu_error_t err = ch.vtable->send(ch.ctx, "chat123", 6, "Hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_telegram_destroy(&ch);
}
```

```c
// WRONG -- skip tracking allocator; no leak assertion
void test_telegram_create(void) {
    hu_channel_t ch;
    hu_telegram_create(hu_system_allocator(), "t", 1, &ch);
    hu_telegram_destroy(&ch);
}

// RIGHT -- use tracking allocator; assert zero leaks
void test_telegram_create(void) {
    hu_tracking_allocator_t ta;
    hu_tracking_allocator_init(&ta, hu_system_allocator());
    hu_channel_t ch;
    hu_telegram_create(hu_tracking_allocator_get(&ta), "t", 1, &ch);
    hu_telegram_destroy(&ch);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(&ta), 0);
    hu_tracking_allocator_deinit(&ta);
}
```

```c
// WRONG -- destroy requires allocator parameter
void hu_telegram_destroy(hu_channel_t *ch, hu_allocator_t *alloc);

// RIGHT -- allocator stored in ctx at create; destroy takes only channel
void hu_telegram_destroy(hu_channel_t *ch) {
    if (!ch || !ch->ctx) return;
    struct telegram_ctx *ctx = ch->ctx;
    hu_allocator_free(ctx->alloc, ctx->token);
    hu_allocator_free(ctx->alloc, ctx);
    ch->ctx = NULL;
}
```

```
WRONG -- test only the happy path (create + send)
RIGHT -- include health_check unconfigured, send with NULL params, error-path cleanup

WRONG -- assert "does not crash" without checking return value or output
RIGHT -- assert on specific return codes, last_message content, and leak count

WRONG -- pass allocator to destroy when it was stored at create
RIGHT -- store allocator in ctx; destroy uses ctx->alloc for all frees
```
