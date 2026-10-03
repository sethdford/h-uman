/* tests/turn_test_fixture.h — shared fixture for the tests/test_turn_*.c stage
 * contract tests (hu_agent_turn carve, phase 1). Header-only static inline,
 * like test_tmpdir.h: an agent built by hu_agent_from_config over the
 * scripted recording provider (tests/turn_recording_provider.h), one
 * READ_ONLY tool named memory_list, an optional in-memory SQLite memory, and
 * HU_STATE_DIR / HOME / the workspace pointed at a scratch dir so nothing
 * reads or writes the developer's ~/.human. */
#ifndef HU_TESTS_TURN_TEST_FIXTURE_H
#define HU_TESTS_TURN_TEST_FIXTURE_H

#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/security.h"
#include "human/tool.h"
#include "test_tmpdir.h"
#include "turn_recording_provider.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct tf_fixture {
    hu_allocator_t alloc;
    trp_t trp;
    hu_memory_t mem;
    bool have_mem;
    hu_tool_t tool;
    hu_agent_t agent;
    bool agent_ok;
    char dir[512];
    bool env_set;
    char *saved_state;
    char *saved_home;
    char *resp; /* out-param for stage calls; tf_close frees it */
    size_t resp_len;
} tf_fixture_t;

static inline hu_error_t tf_tool_execute(void *ctx, hu_allocator_t *alloc,
                                         const hu_json_value_t *args, hu_tool_result_t *out) {
    (void)ctx;
    (void)alloc;
    (void)args;
    *out = hu_tool_result_ok("listed 2 items: alpha, beta", 27);
    return HU_OK;
}
static inline const char *tf_tool_name(void *ctx) {
    (void)ctx;
    return "memory_list";
}
static inline const char *tf_tool_desc(void *ctx) {
    (void)ctx;
    return "List stored items";
}
static inline const char *tf_tool_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{\"q\":{\"type\":\"string\"}}}";
}

static inline char *tf_save_env(const char *name) {
    const char *v = getenv(name);
    return v ? strdup(v) : NULL;
}

static inline void tf_restore_env(const char *name, char *saved) {
    if (saved) {
        setenv(name, saved, 1);
        free(saved);
    } else {
        unsetenv(name);
    }
}

/* script may be NULL: every chat() then answers "ok.". `alloc` becomes the
 * agent's (and the memory's) allocator — pass a tracking allocator to count
 * leaks across a turn and hu_agent_deinit. */
static inline bool tf_open_alloc(tf_fixture_t *f, hu_allocator_t alloc, const trp_step_t *script,
                                 size_t script_count, bool memory, uint8_t autonomy) {
    static const hu_tool_vtable_t vt = {
        .execute = tf_tool_execute,
        .name = tf_tool_name,
        .description = tf_tool_desc,
        .parameters_json = tf_tool_params,
    };
    memset(f, 0, sizeof(*f));
    f->alloc = alloc;
    trp_init(&f->trp, script, script_count, "ok.");
    if (!hu_test_mkdtemp(NULL, f->dir, sizeof(f->dir))) {
        f->dir[0] = '\0';
        return false;
    }
    f->saved_state = tf_save_env("HU_STATE_DIR");
    f->saved_home = tf_save_env("HOME");
    f->env_set = true;
    setenv("HU_STATE_DIR", f->dir, 1);
    setenv("HOME", f->dir, 1);
    f->tool.ctx = NULL;
    f->tool.vtable = &vt;
    if (memory) {
#ifdef HU_ENABLE_SQLITE
        f->mem = hu_sqlite_memory_create(&f->alloc, ":memory:");
        f->have_mem = f->mem.vtable != NULL;
        if (!f->have_mem)
            return false;
#else
        return false;
#endif
    }
    f->agent_ok = hu_agent_from_config(&f->agent, &f->alloc, trp_provider(&f->trp), &f->tool, 1,
                                       f->have_mem ? &f->mem : NULL, NULL, NULL, NULL, "turn-model",
                                       10, "turn", 4, 0.7, f->dir, strlen(f->dir), 4, 50, false,
                                       autonomy, NULL, 0, NULL, 0, NULL) == HU_OK;
    return f->agent_ok;
}

static inline bool tf_open(tf_fixture_t *f, const trp_step_t *script, size_t script_count,
                           bool memory, uint8_t autonomy) {
    return tf_open_alloc(f, hu_system_allocator(), script, script_count, memory, autonomy);
}

static inline void tf_close(tf_fixture_t *f) {
    if (f->resp)
        f->alloc.free(f->alloc.ctx, f->resp, f->resp_len + 1);
    if (f->agent_ok)
        hu_agent_deinit(&f->agent);
    if (f->have_mem && f->mem.vtable && f->mem.vtable->deinit)
        f->mem.vtable->deinit(f->mem.ctx);
    trp_deinit(&f->trp);
    if (f->env_set) {
        tf_restore_env("HU_STATE_DIR", f->saved_state);
        tf_restore_env("HOME", f->saved_home);
    }
    if (f->dir[0])
        hu_test_rm_rf(f->dir);
    memset(f, 0, sizeof(*f));
}

static inline bool tf_store(tf_fixture_t *f, const char *key, const char *content,
                            const char *session) {
    if (!f->have_mem)
        return false;
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    return f->mem.vtable->store(f->mem.ctx, key, strlen(key), content, strlen(content), &cat,
                                session, session ? strlen(session) : 0) == HU_OK;
}

#endif /* HU_TESTS_TURN_TEST_FIXTURE_H */
