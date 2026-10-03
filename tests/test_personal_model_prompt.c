/* hu_turn_personal_model_prompt (src/agent/turn/personal_model_prompt.c): the
 * personal-model block agent_turn_run puts in the prompt. Carved out of
 * agent_turn.c in the 2026-10-02 train; these pin that the carve renders what
 * the inline block did (OFF) and keeps #608's confidence view (LIVE). */
#include "test_framework.h"

#include "human/agent.h"
#include "human/agent/turn.h"
#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include "human/memory/confidence_boundary.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_pmp_a[] = "+15550000011"; /* told the twin something */
static const char k_pmp_b[] = "+15550000012"; /* is texting now */

static void pmp_add_fact(hu_personal_model_t *pm, const char *subj, const char *obj,
                         const char *handle, const char *channel) {
    hu_heuristic_fact_t *f = &pm->facts[pm->fact_count++];
    memset(f, 0, sizeof(*f));
    snprintf(f->subject, sizeof(f->subject), "%s", subj);
    snprintf(f->predicate, sizeof(f->predicate), "%s", "is");
    snprintf(f->object, sizeof(f->object), "%s", obj);
    f->confidence = 0.9f;
    f->last_seen_at = pm->updated_at;
    if (handle)
        snprintf(f->provenance.contact_handle, sizeof(f->provenance.contact_handle), "%s", handle);
    snprintf(f->provenance.channel, sizeof(f->provenance.channel), "%s", channel);
}

static hu_agent_t *pmp_agent(hu_allocator_t *a) {
    hu_agent_t *agent = (hu_agent_t *)calloc(1, sizeof(*agent));
    HU_ASSERT_NOT_NULL(agent);
    agent->alloc = a;
    hu_personal_model_init(&agent->personal_model);
    agent->personal_model.updated_at = 1767225600;
    agent->memory_session_id = k_pmp_b;
    agent->memory_session_id_len = sizeof(k_pmp_b) - 1;
    return agent;
}

static void personal_model_prompt_empty_model_renders_nothing(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_t *agent = pmp_agent(&a);
    static char buf[8192];
    HU_ASSERT_EQ(hu_turn_personal_model_prompt(agent, buf, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_turn_personal_model_prompt(agent, NULL, sizeof(buf)), 0u);
    HU_ASSERT_EQ(hu_turn_personal_model_prompt(NULL, buf, sizeof(buf)), 0u);
    free(agent);
}

static void personal_model_prompt_off_is_the_plain_build(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_t *agent = pmp_agent(&a);
    pmp_add_fact(&agent->personal_model, "sister", "pregnant", k_pmp_a, "imessage_dm");
    pmp_add_fact(&agent->personal_model, "user", "an oat latte person", NULL, "cli");
    static char want[8192], got[8192];
    size_t want_n = hu_personal_model_build_prompt(&agent->personal_model, want, sizeof(want));
    HU_ASSERT_TRUE(want_n > 0);
    hu_confidence_set_mode_for_test(HU_GATE_OFF);
    size_t got_n = hu_turn_personal_model_prompt(agent, got, sizeof(got));
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_EQ(got_n, want_n);
    HU_ASSERT_TRUE(memcmp(got, want, want_n) == 0);
    HU_ASSERT_NOT_NULL(strstr(got, "pregnant"));
    free(agent);
}

static void personal_model_prompt_live_keeps_another_contacts_fact_out(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_t *agent = pmp_agent(&a);
    pmp_add_fact(&agent->personal_model, "sister", "pregnant", k_pmp_a, "imessage_dm");
    pmp_add_fact(&agent->personal_model, "user", "an oat latte person", NULL, "cli");
    static char got[8192];
    hu_confidence_set_mode_for_test(HU_GATE_LIVE);
    size_t got_n = hu_turn_personal_model_prompt(agent, got, sizeof(got));
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_TRUE(got_n > 0);
    HU_ASSERT_NULL(strstr(got, "pregnant"));
    HU_ASSERT_NOT_NULL(strstr(got, "oat latte"));
    HU_ASSERT_EQ(agent->personal_model.fact_count, 2); /* the agent's model is untouched */
    free(agent);
}

#ifdef HU_ENABLE_SQLITE
#include "human/config.h"
#include "human/memory.h"
#include "human/reflection.h"
#include <sqlite3.h>
#include <time.h>

/* One live, unsurfaced reflection pattern for `channel` (the shape
 * hu_reflection_query_for_system_prompt reads; see
 * tests/test_personal_model_reflection_slice.c). */
static void pmp_seed_reflection(sqlite3 *db, const char *observation) {
    HU_ASSERT_EQ(hu_reflection_storage_migrate(db), HU_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "INSERT INTO reflection_runs (run_id, provider, started_at_ms, "
                              "completed_at_ms, input_turns, status, prose_summary) VALUES "
                              "('run_seed', 'mock', 1000, 2000, 5, 'ok', 'quiet week')",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(
                     db,
                     "INSERT INTO reflection_patterns (id, type, subject, observation, confidence, "
                     "evidence_json, channels_json, first_seen_run_id, last_seen_run_id, "
                     "observation_count, created_at_ms, last_observed_at_ms, expires_at_ms, "
                     "surfaced_to_user, retired) VALUES ('p1', 'preference', 'Seth', ?, 0.85, "
                     "'[]', '[\"imessage\"]', 'run_seed', 'run_seed', 1, ?, ?, ?, 0, 0)",
                     -1, &st, NULL),
                 SQLITE_OK);
    sqlite3_bind_text(st, 1, observation, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)now_ms);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now_ms);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)(now_ms + 30ULL * 86400000ULL));
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_DONE);
    sqlite3_finalize(st);
}

/* The reflection-loop branch: with reflection_loop.enabled, a SQLite memory
 * and an active channel, the carve renders exactly what the pre-carve inline
 * block did — hu_personal_model_build_prompt_with_reflection(model, overlay,
 * db, channel, 5) — and the confidence gate OFF leaves it byte-identical. */
static void personal_model_prompt_reflection_branch_matches_the_inline_build(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_t *agent = pmp_agent(&a);
    pmp_add_fact(&agent->personal_model, "sister", "pregnant", k_pmp_a, "imessage_dm");
    pmp_add_fact(&agent->personal_model, "user", "an oat latte person", NULL, "cli");
    static const char k_obs[] = "shifts to one-word replies after 9pm";

    /* The expected bytes come from a twin db: building with reflection marks
     * the pattern surfaced, so each side needs its own copy. */
    hu_memory_t want_mem = hu_sqlite_memory_create(&a, ":memory:");
    hu_memory_t got_mem = hu_sqlite_memory_create(&a, ":memory:");
    HU_ASSERT_NOT_NULL(want_mem.vtable);
    HU_ASSERT_NOT_NULL(got_mem.vtable);
    pmp_seed_reflection(hu_sqlite_memory_get_db(&want_mem), k_obs);
    pmp_seed_reflection(hu_sqlite_memory_get_db(&got_mem), k_obs);

    static char want[8192], plain[8192], got[8192];
    size_t want_n = hu_personal_model_build_prompt_with_reflection(
        &agent->personal_model, NULL, hu_sqlite_memory_get_db(&want_mem), "imessage",
        /*max_patterns=*/5, want, sizeof(want));
    size_t plain_n = hu_personal_model_build_prompt(&agent->personal_model, plain, sizeof(plain));
    HU_ASSERT_NOT_NULL(strstr(want, k_obs)); /* the branch adds the slice... */
    HU_ASSERT_NULL(strstr(plain, k_obs));    /* ...the plain build does not */
    HU_ASSERT_TRUE(want_n > plain_n);

    static hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.reflection_loop.enabled = true;
    agent->config = &cfg;
    agent->memory = &got_mem;
    agent->active_channel = "imessage";
    agent->active_channel_len = 8;
    hu_confidence_set_mode_for_test(HU_GATE_OFF);
    size_t got_n = hu_turn_personal_model_prompt(agent, got, sizeof(got));
    hu_confidence_set_mode_for_test(-1);
    HU_ASSERT_EQ(got_n, want_n);
    HU_ASSERT_TRUE(memcmp(got, want, want_n) == 0);

    want_mem.vtable->deinit(want_mem.ctx);
    got_mem.vtable->deinit(got_mem.ctx);
    free(agent);
}
#endif

void run_personal_model_prompt_tests(void) {
    HU_TEST_SUITE("personal_model_prompt");
    HU_RUN_TEST(personal_model_prompt_empty_model_renders_nothing);
    HU_RUN_TEST(personal_model_prompt_off_is_the_plain_build);
    HU_RUN_TEST(personal_model_prompt_live_keeps_another_contacts_fact_out);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(personal_model_prompt_reflection_branch_matches_the_inline_build);
#endif
}
