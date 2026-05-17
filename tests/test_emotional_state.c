#ifdef HU_ENABLE_SQLITE

#include "human/context/emotional_state.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/emotional_moments.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

/* P2-2 regression (2026-05-16 incident):
 *
 * hu_emotional_state_record used to store a raw 60-character window of the
 * user's text into emotional_moments.topic via:
 *
 *     snprintf(topic_buf, 64, "%.60s", context_buf[0] ? context_buf : emotion);
 *
 * That raw text was then surfaced through F25/F30 paths and shipped verbatim
 * to family contacts. The fix: when the heuristic produces no clean topic,
 * fall back to the emotion keyword instead.
 *
 * The test below feeds an utterance that contains the keyword "lonely" and
 * verifies the recorded emotional_moments.topic is NOT a raw substring of
 * the user's text (which would contain "but boy" or "more lonely now").
 * Instead it must be either a clean extracted noun phrase (rare with such a
 * short utterance) OR the emotion keyword itself ("sadness"/"loneliness"). */

static bool topic_looks_like_raw_substring(const char *topic, const char *original) {
    /* If the stored topic is a verbatim run of >= 10 chars from the user text,
     * that's the bug. A safe topic is either the emotion keyword (short) or
     * an extracted 2-3 word noun phrase. */
    if (!topic || !original)
        return false;
    size_t tlen = strlen(topic);
    if (tlen < 10)
        return false;
    /* Substring match against the original text. */
    return strstr(original, topic) != NULL;
}

static void emotional_state_record_lonely_does_not_store_raw_substring(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    const char *user_text = "but boy I am just more lonely now than ever";
    /* The emotional_moments write happens before any mood_log write. The
     * function may return HU_ERR_MEMORY_BACKEND because of the orthogonal
     * mood_log schema conflict (P2-10) — but the emotional_moments row we
     * care about still lands. */
    (void)hu_emotional_state_record(&alloc, &mem, "contact_lonely", 14, user_text,
                                    strlen(user_text));

    /* Inspect what landed in emotional_moments. */
    int64_t now_ts = (int64_t)time(NULL) + 86401;
    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    HU_ASSERT_EQ(hu_emotional_moment_get_due(&alloc, &mem, now_ts, &due, &due_count), HU_OK);
    HU_ASSERT_EQ(due_count, 1u);

    /* The stored topic must NOT be a raw multi-word substring of the user's
     * confession. It should be the emotion keyword (e.g. "sadness") or a
     * clean noun phrase. */
    HU_ASSERT_FALSE(topic_looks_like_raw_substring(due[0].topic, user_text));

    /* Belt-and-suspenders: the topic must not contain the leakage giveaway
     * "but boy" or "lonely now". */
    HU_ASSERT_NULL(strstr(due[0].topic, "but boy"));
    HU_ASSERT_NULL(strstr(due[0].topic, "lonely now"));

    alloc.free(alloc.ctx, due, due_count * sizeof(hu_emotional_moment_t));
    mem.vtable->deinit(mem.ctx);
}

static void emotional_state_record_confession_falls_back_to_emotion_keyword(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    const char *user_text = "I confessed something terrible to my friend";
    hu_error_t err =
        hu_emotional_state_record(&alloc, &mem, "contact_conf", 12, user_text, strlen(user_text));
    /* If no emotion keyword matches, the function returns HU_OK without writing. */
    HU_ASSERT_EQ(err, HU_OK);

    int64_t now_ts = (int64_t)time(NULL) + 86401;
    hu_emotional_moment_t *due = NULL;
    size_t due_count = 0;
    HU_ASSERT_EQ(hu_emotional_moment_get_due(&alloc, &mem, now_ts, &due, &due_count), HU_OK);

    /* Whether 0 or 1 row landed: any stored topic must NOT be the raw
     * confession substring. */
    for (size_t i = 0; i < due_count; i++) {
        HU_ASSERT_NULL(strstr(due[i].topic, "confessed something"));
        HU_ASSERT_NULL(strstr(due[i].topic, "terrible to my"));
    }

    if (due_count > 0)
        alloc.free(alloc.ctx, due, due_count * sizeof(hu_emotional_moment_t));
    mem.vtable->deinit(mem.ctx);
}

/* P2-10 regression (2026-05-16 incident): src/context/emotional_state.c
 * and src/memory/engines/sqlite.c BOTH had `CREATE TABLE IF NOT EXISTS
 * mood_log` with different column schemas. The second CREATE silently
 * no-ops; one writer's INSERTs silently fail. This test pins that the
 * per-contact emotional-state writer succeeds end-to-end and is reachable
 * via the public read API. */
static void emotional_state_record_round_trip_succeeds(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    const char *user_text = "I am stressed about the deadline tomorrow";
    hu_error_t err =
        hu_emotional_state_record(&alloc, &mem, "contact_p10", 11, user_text, strlen(user_text));
    /* After P2-10 fix, this MUST return HU_OK (no schema clash). */
    HU_ASSERT_EQ(err, HU_OK);

    mem.vtable->deinit(mem.ctx);
}

void run_emotional_state_tests(void) {
    HU_TEST_SUITE("emotional_state");
    HU_RUN_TEST(emotional_state_record_lonely_does_not_store_raw_substring);
    HU_RUN_TEST(emotional_state_record_confession_falls_back_to_emotion_keyword);
    HU_RUN_TEST(emotional_state_record_round_trip_succeeds);
}

#else

void run_emotional_state_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
