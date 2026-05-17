#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/experience.h"
#include "human/memory.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

/* Regression guard: writers that go through hu_memory_t->vtable->store MUST
 * use the memory backend's current_session_id when scoping the write. Without
 * this, per-contact memory recall (which keys on session_id) can't find the
 * write and the agent loses continuity with the contact who triggered it.
 *
 * Bug context: prior to the fix, two writers passed literal "", 0:
 *   - src/intelligence/experience.c:165  (53 of 61 unscoped rows in prod db)
 *   - src/context/context_engine_rag.c:97 (every RAG-indexed message)
 *
 * Strategy: stand up a minimal mock memory backend that captures every
 * (key, session_id) pair passed to its store function. Then drive the public
 * writer (hu_experience_record) with the memory's current_session_id set,
 * and assert the captured session_id matches. */

#define CAPTURE_CAP 16

typedef struct {
    char key[256];
    size_t key_len;
    char session_id[256];
    size_t session_id_len;
} captured_write_t;

typedef struct {
    captured_write_t writes[CAPTURE_CAP];
    size_t write_count;
} mock_memory_ctx_t;

static const char *mock_name(void *ctx) {
    (void)ctx;
    return "mock";
}

static hu_error_t mock_store(void *ctx, const char *key, size_t key_len, const char *content,
                             size_t content_len, const hu_memory_category_t *category,
                             const char *session_id, size_t session_id_len) {
    (void)content;
    (void)content_len;
    (void)category;
    mock_memory_ctx_t *m = (mock_memory_ctx_t *)ctx;
    if (m->write_count >= CAPTURE_CAP)
        return HU_OK;
    captured_write_t *w = &m->writes[m->write_count++];
    size_t kn = key_len < sizeof(w->key) - 1 ? key_len : sizeof(w->key) - 1;
    memcpy(w->key, key, kn);
    w->key[kn] = '\0';
    w->key_len = kn;
    size_t sn =
        session_id_len < sizeof(w->session_id) - 1 ? session_id_len : sizeof(w->session_id) - 1;
    if (session_id && sn > 0)
        memcpy(w->session_id, session_id, sn);
    w->session_id[sn] = '\0';
    w->session_id_len = sn;
    return HU_OK;
}

static const struct hu_legacy_memory_vtable mock_vt = {
    .name = mock_name,
    .store = mock_store,
    /* All other slots NULL — hu_experience_record only calls .store. */
};

static void setup_mock_memory(hu_memory_t *mem, mock_memory_ctx_t *mctx) {
    memset(mctx, 0, sizeof(*mctx));
    memset(mem, 0, sizeof(*mem));
    mem->ctx = mctx;
    mem->vtable = &mock_vt;
}

/* ── core regression: experience_record propagates current_session_id ───── */

static void experience_record_uses_current_session_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    mock_memory_ctx_t mctx;
    setup_mock_memory(&mem, &mctx);

    /* Set the memory's current_session_id to Mindy's routed key. */
    const char *sid = "agent:seth:imessage:direct:+18018285260";
    mem.current_session_id = sid;
    mem.current_session_id_len = strlen(sid);

    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, &mem, &store), HU_OK);

    HU_ASSERT_EQ(hu_experience_record(&store, "ask sister about birthday plans", 31,
                                      "checked calendar", 16, "planned dinner saturday", 24, 0.9),
                 HU_OK);

    /* Exactly one write should have landed via the mock vtable. */
    HU_ASSERT_EQ(mctx.write_count, 1u);

    /* The captured session_id MUST match what the memory backend was set to —
     * not empty, not arbitrary, not truncated. */
    HU_ASSERT_EQ(mctx.writes[0].session_id_len, strlen(sid));
    HU_ASSERT_STR_EQ(mctx.writes[0].session_id, sid);

    /* The key should be the experience prefix; sanity check we wrote the
     * intended record (defends against a future refactor that breaks
     * key formatting and accidentally moves writes elsewhere). */
    HU_ASSERT(strstr(mctx.writes[0].key, "experience:") != NULL);

    hu_experience_store_deinit(&store);
}

/* ── boundary: NULL current_session_id falls back to empty (legacy behavior) */

static void experience_record_with_null_session_writes_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    mock_memory_ctx_t mctx;
    setup_mock_memory(&mem, &mctx);

    /* Memory backend has NO current_session_id set — write should not crash
     * and should pass empty session_id (preserves legacy unscoped behavior
     * for callers that genuinely have no contact context, e.g. early startup). */
    mem.current_session_id = NULL;
    mem.current_session_id_len = 0;

    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, &mem, &store), HU_OK);

    HU_ASSERT_EQ(hu_experience_record(&store, "global task", 11, "did the thing", 13, "ok", 2, 0.5),
                 HU_OK);

    HU_ASSERT_EQ(mctx.write_count, 1u);
    HU_ASSERT_EQ(mctx.writes[0].session_id_len, 0u);

    hu_experience_store_deinit(&store);
}

/* ── boundary: switching session between writes scopes each correctly ────── */

static void experience_record_picks_up_session_changes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem;
    mock_memory_ctx_t mctx;
    setup_mock_memory(&mem, &mctx);

    hu_experience_store_t store;
    HU_ASSERT_EQ(hu_experience_store_init(&alloc, &mem, &store), HU_OK);

    /* Write 1: scoped to Mindy. */
    const char *sid_mindy = "agent:seth:imessage:direct:+18018285260";
    mem.current_session_id = sid_mindy;
    mem.current_session_id_len = strlen(sid_mindy);
    HU_ASSERT_EQ(hu_experience_record(&store, "task A", 6, "action A", 8, "outcome A", 9, 0.8),
                 HU_OK);

    /* Write 2: switch to a different contact. */
    const char *sid_marc = "agent:seth:imessage:direct:+18019137087";
    mem.current_session_id = sid_marc;
    mem.current_session_id_len = strlen(sid_marc);
    HU_ASSERT_EQ(hu_experience_record(&store, "task B", 6, "action B", 8, "outcome B", 9, 0.7),
                 HU_OK);

    /* Write 3: clear the session — should write empty (global). */
    mem.current_session_id = NULL;
    mem.current_session_id_len = 0;
    HU_ASSERT_EQ(hu_experience_record(&store, "task C", 6, "action C", 8, "outcome C", 9, 0.6),
                 HU_OK);

    HU_ASSERT_EQ(mctx.write_count, 3u);
    HU_ASSERT_STR_EQ(mctx.writes[0].session_id, sid_mindy);
    HU_ASSERT_STR_EQ(mctx.writes[1].session_id, sid_marc);
    HU_ASSERT_EQ(mctx.writes[2].session_id_len, 0u);

    hu_experience_store_deinit(&store);
}

/* ── runner ──────────────────────────────────────────────────────────────── */

void run_memory_session_scoping_tests(void);

void run_memory_session_scoping_tests(void) {
    HU_TEST_SUITE("Memory Session Scoping (regression for Mindy-context bug)");

    HU_RUN_TEST(experience_record_uses_current_session_id);
    HU_RUN_TEST(experience_record_with_null_session_writes_empty);
    HU_RUN_TEST(experience_record_picks_up_session_changes);
}
