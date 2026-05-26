#include "human/persona/sticker.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Helper to touch an empty file at path. */
static void touch_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

/* Setup: create tmpdir + 6 fixture stickers spanning the tag space. */
static char *setup_fixture_dir(void) {
    static char tmpdir[256];
    strcpy(tmpdir, "/tmp/human-stickers-XXXXXX");
    char *r = mkdtemp(tmpdir);
    if (!r)
        return NULL;

    char fp[512];
    snprintf(fp, sizeof(fp), "%s/casual-happy-warm_001.png", tmpdir);
    touch_file(fp);
    snprintf(fp, sizeof(fp), "%s/casual-happy-warm_002.png", tmpdir);
    touch_file(fp);
    snprintf(fp, sizeof(fp), "%s/casual-happy-dry_001.png", tmpdir);
    touch_file(fp);
    snprintf(fp, sizeof(fp), "%s/formal-acknowledgment-earnest_001.png", tmpdir);
    touch_file(fp);
    snprintf(fp, sizeof(fp), "%s/intimate-support-warm_001.heic", tmpdir);
    touch_file(fp);
    snprintf(fp, sizeof(fp), "%s/playful-laugh-dry_001.jpg", tmpdir);
    touch_file(fp);

    /* Set LRU path to a tmp file inside the dir (so tests don't pollute ~/). */
    char lru_path[512];
    snprintf(lru_path, sizeof(lru_path), "%s/_lru.txt", tmpdir);
    hu_persona_sticker_set_test_lru_path(lru_path);
    return tmpdir;
}

static void teardown_fixture_dir(char *tmpdir) {
    /* rm -rf the tmpdir contents + dir itself. */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    (void)system(cmd);
    hu_persona_sticker_set_test_lru_path(NULL);
}

/* AC: exact tag match picks the right file. */
static void exact_tag_match_picks_matching_sticker(void) {
    char *dir = setup_fixture_dir();
    HU_ASSERT_NOT_NULL(dir);

    hu_sticker_query_t q = {.context_tag = "intimate", .mood_tag = "support", .tone_tag = "warm"};
    char out[512];
    HU_ASSERT(hu_persona_pick_sticker(dir, &q, out, sizeof(out)));
    HU_ASSERT(strstr(out, "intimate-support-warm_001.heic") != NULL);

    teardown_fixture_dir(dir);
}

/* AC: NULL tone matches any tone (broadens the pick set). */
static void null_tone_matches_any_tone(void) {
    char *dir = setup_fixture_dir();
    HU_ASSERT_NOT_NULL(dir);

    hu_sticker_query_t q = {.context_tag = "casual", .mood_tag = "happy", .tone_tag = NULL};
    char out[512];
    HU_ASSERT(hu_persona_pick_sticker(dir, &q, out, sizeof(out)));
    /* Should be one of casual-happy-warm_001, casual-happy-warm_002,
     * casual-happy-dry_001 (any of the 3 matches). */
    HU_ASSERT(strstr(out, "casual-happy-") != NULL);

    teardown_fixture_dir(dir);
}

/* AC: no match returns false without crashing. */
static void no_match_returns_false(void) {
    char *dir = setup_fixture_dir();
    HU_ASSERT_NOT_NULL(dir);

    hu_sticker_query_t q = {
        .context_tag = "formal", .mood_tag = "laugh", .tone_tag = "warm"}; /* no such file */
    char out[512];
    HU_ASSERT(!hu_persona_pick_sticker(dir, &q, out, sizeof(out)));

    teardown_fixture_dir(dir);
}

/* AC: missing dir returns false without crashing. */
static void missing_dir_returns_false(void) {
    hu_sticker_query_t q = {.context_tag = "casual", .mood_tag = "happy", .tone_tag = "warm"};
    char out[512];
    HU_ASSERT(!hu_persona_pick_sticker("/tmp/nonexistent_sticker_dir_XYZ", &q, out, sizeof(out)));
}

/* AC: LRU rotation — after picking a file, the next pick with the same
 * query SHOULD prefer a different file (when alternatives exist). */
static void lru_avoids_recent_picks_when_alternatives_exist(void) {
    char *dir = setup_fixture_dir();
    HU_ASSERT_NOT_NULL(dir);

    hu_sticker_query_t q = {.context_tag = "casual",
                            .mood_tag = "happy",
                            .tone_tag = "warm"}; /* 2 matches: _001, _002 */
    char first[512], second[512];
    HU_ASSERT(hu_persona_pick_sticker(dir, &q, first, sizeof(first)));
    HU_ASSERT(hu_persona_pick_sticker(dir, &q, second, sizeof(second)));
    /* With LRU rotation and 2 matching files, second pick should be
     * different from first. */
    HU_ASSERT(strcmp(first, second) != 0);

    teardown_fixture_dir(dir);
}

void run_persona_sticker_tests(void) {
    HU_TEST_SUITE("persona_sticker");
    HU_RUN_TEST(exact_tag_match_picks_matching_sticker);
    HU_RUN_TEST(null_tone_matches_any_tone);
    HU_RUN_TEST(no_match_returns_false);
    HU_RUN_TEST(missing_dir_returns_false);
    HU_RUN_TEST(lru_avoids_recent_picks_when_alternatives_exist);
}
