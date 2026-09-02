/* Slice A of the daemon.c batch-reply carve-out: hu_daemon_reactive_context_load.
 *
 * Under HU_IS_TEST the function's gated tail (contact profile, style learning,
 * channel history, comfort, cross-channel) is compiled out exactly as it was in
 * hu_service_run, so these tests pin the ungated prefix: history clear, active
 * channel, session-store restore, and the every-output-starts-empty contract. */
#include "human/daemon/reactive_turn.h"

#include "human/agent.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "test_framework.h"

#include <string.h>

/* ── Fixtures ───────────────────────────────────────────────────────── */

static const char *fake_channel_name(void *ctx) {
    (void)ctx;
    return "unit";
}

static const hu_channel_vtable_t k_fake_channel_vt = {
    .name = fake_channel_name,
};

typedef struct fake_store {
    size_t load_calls;
    size_t sid_len_seen;
    char sid_seen[64];
} fake_store_t;

static hu_error_t fake_load_messages(void *ctx, hu_allocator_t *alloc, const char *sid,
                                     size_t sid_len, hu_message_entry_t **out, size_t *out_count) {
    fake_store_t *fs = (fake_store_t *)ctx;
    fs->load_calls++;
    fs->sid_len_seen = sid_len;
    if (sid_len < sizeof(fs->sid_seen)) {
        memcpy(fs->sid_seen, sid, sid_len);
        fs->sid_seen[sid_len] = '\0';
    }
    /* Allocated exactly the way the production restore loop frees them:
     * role/content via len+1, the array via count*sizeof. */
    hu_message_entry_t *e =
        (hu_message_entry_t *)alloc->alloc(alloc->ctx, 2 * sizeof(hu_message_entry_t));
    if (!e)
        return HU_ERR_OUT_OF_MEMORY;
    memset(e, 0, 2 * sizeof(hu_message_entry_t));
    e[0].role = hu_strndup(alloc, "user", 4);
    e[0].role_len = 4;
    e[0].content = hu_strndup(alloc, "hey", 3);
    e[0].content_len = 3;
    e[1].role = hu_strndup(alloc, "assistant", 9);
    e[1].role_len = 9;
    e[1].content = hu_strndup(alloc, "yo", 2);
    e[1].content_len = 2;
    *out = e;
    *out_count = 2;
    return HU_OK;
}

static const hu_session_store_vtable_t k_fake_store_vt = {
    .load_messages = fake_load_messages,
};

static hu_daemon_comfort_pending_t g_comfort_slots[HU_COMFORT_PENDING_MAX];

typedef struct fixture {
    hu_allocator_t alloc;
    hu_agent_t agent;
    hu_channel_t channel;
    hu_service_channel_t svc;
    fake_store_t store_ctx;
    hu_session_store_t store;
    hu_reactive_turn_ctx_t rt;
} fixture_t;

static void fixture_init(fixture_t *f, bool with_store) {
    memset(f, 0, sizeof(*f));
    f->alloc = hu_system_allocator();
    f->agent.alloc = &f->alloc;
    f->channel.vtable = &k_fake_channel_vt;
    f->svc.channel = &f->channel;
    f->store.ctx = &f->store_ctx;
    f->store.vtable = &k_fake_store_vt;
    if (with_store)
        f->agent.session_store = &f->store;
    f->rt.ch = &f->svc;
    f->rt.batch_key = "+15551234567";
    f->rt.key_len = strlen(f->rt.batch_key);
    f->rt.combined = "hey";
    f->rt.combined_len = 3;
    f->rt.comfort_pending = g_comfort_slots;
}

static void fixture_free(fixture_t *f) {
    hu_agent_clear_history(&f->agent);
    if (f->agent.history)
        f->alloc.free(f->alloc.ctx, f->agent.history,
                      f->agent.history_cap * sizeof(hu_owned_message_t));
    f->agent.history = NULL;
    f->agent.history_cap = 0;
}

static void load(fixture_t *f) {
    hu_daemon_reactive_context_load(&f->alloc, &f->agent, NULL, &f->svc, 1, &f->rt);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static void test_restores_session_history_with_roles(void) {
    fixture_t f;
    fixture_init(&f, true);
    HU_ASSERT_EQ(f.agent.history_count, 0u);

    load(&f);

    HU_ASSERT_EQ(f.store_ctx.load_calls, 1u);
    HU_ASSERT_EQ(f.store_ctx.sid_len_seen, f.rt.key_len);
    HU_ASSERT_STR_EQ(f.store_ctx.sid_seen, "+15551234567");
    HU_ASSERT_EQ(f.agent.history_count, 2u);
    HU_ASSERT_EQ((int)f.agent.history[0].role, (int)HU_ROLE_USER);
    HU_ASSERT_STR_EQ(f.agent.history[0].content, "hey");
    HU_ASSERT_EQ(f.agent.history[0].content_len, 3u);
    HU_ASSERT_EQ((int)f.agent.history[1].role, (int)HU_ROLE_ASSISTANT);
    HU_ASSERT_STR_EQ(f.agent.history[1].content, "yo");
    fixture_free(&f);
}

static void test_sets_active_channel_from_vtable_name(void) {
    fixture_t f;
    fixture_init(&f, false);
    HU_ASSERT_NULL(f.agent.active_channel);

    load(&f);

    HU_ASSERT_NOT_NULL(f.agent.active_channel);
    HU_ASSERT_STR_EQ(f.agent.active_channel, "unit");
    HU_ASSERT_EQ(f.agent.active_channel_len, 4u);
    fixture_free(&f);
}

static void test_clears_prior_history_before_restore(void) {
    fixture_t f;
    fixture_init(&f, false);
    /* One stale row from a previous contact's turn. */
    f.agent.history = (hu_owned_message_t *)f.alloc.alloc(f.alloc.ctx, sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(f.agent.history);
    memset(f.agent.history, 0, sizeof(hu_owned_message_t));
    f.agent.history[0].role = HU_ROLE_ASSISTANT;
    f.agent.history[0].content = hu_strndup(&f.alloc, "old", 3);
    f.agent.history[0].content_len = 3;
    f.agent.history_count = 1;
    f.agent.history_cap = 1;

    load(&f);

    HU_ASSERT_EQ(f.agent.history_count, 0u);
    fixture_free(&f);
}

static void test_outputs_start_empty(void) {
    fixture_t f;
    fixture_init(&f, true);
    /* Poison the outputs the loader writes in every build: it must own them,
     * not inherit them. ctx_entries/ctx_count are produced only outside
     * HU_IS_TEST (same gate as in hu_service_run), so they are left at the
     * caller's zero and asserted unchanged. */
    f.rt.contact_ctx_len = 99;
    f.rt.history_count = 99;
    f.rt.convo_ctx_len = 99;

    load(&f);

    HU_ASSERT_NULL(f.rt.contact_ctx);
    HU_ASSERT_EQ(f.rt.contact_ctx_len, 0u);
    HU_ASSERT_NULL(f.rt.convo_ctx);
    HU_ASSERT_EQ(f.rt.convo_ctx_len, 0u);
    HU_ASSERT_NULL(f.rt.history_entries);
    HU_ASSERT_EQ(f.rt.history_count, 0u);
    HU_ASSERT_NULL(f.rt.contact_for_tapback);
    HU_ASSERT_NULL(f.rt.ctx_entries);
    HU_ASSERT_EQ(f.rt.ctx_count, 0u);
    fixture_free(&f);
}

static void test_missing_store_leaves_history_empty(void) {
    fixture_t f;
    fixture_init(&f, false);

    load(&f);

    HU_ASSERT_EQ(f.agent.history_count, 0u);
    HU_ASSERT_EQ(f.store_ctx.load_calls, 0u);
    fixture_free(&f);
}

void run_daemon_reactive_context_tests(void) {
    HU_TEST_SUITE("Daemon Reactive Context (slice A)");
    HU_RUN_TEST(test_restores_session_history_with_roles);
    HU_RUN_TEST(test_sets_active_channel_from_vtable_name);
    HU_RUN_TEST(test_clears_prior_history_before_restore);
    HU_RUN_TEST(test_outputs_start_empty);
    HU_RUN_TEST(test_missing_store_leaves_history_empty);
}
