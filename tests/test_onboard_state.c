#include "human/onboard/state.h"
#include "test_framework.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void test_onboard_state_init_defaults(void) {
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    HU_ASSERT_EQ(state.schema_version, 1);
    HU_ASSERT_EQ(state.current, HU_ONBOARD_STEP_WELCOME);
    HU_ASSERT_EQ(state.history_depth, 0);
    HU_ASSERT_EQ(state.provider.provider_name[0], '\0');
    HU_ASSERT_FALSE(state.provider.provider_smoke_passed);
    HU_ASSERT_EQ(state.persona.template_choice, '\0');
    HU_ASSERT_FALSE(state.channels.imessage_enabled);
    HU_ASSERT_FALSE(state.testsend.test_send_succeeded);
}

static void test_onboard_state_save_and_load_roundtrip(void) {
    char tmpl[] = "/tmp/hu_onboard_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);

    char path[256];
    snprintf(path, sizeof(path), "%s/onboard-state.json", dir);

    /* Create a state with distinguishable values */
    hu_onboard_state_t original;
    hu_onboard_state_init(&original);
    original.current = HU_ONBOARD_STEP_PROVIDER;
    original.history_depth = 2;
    original.history[0] = HU_ONBOARD_STEP_WELCOME;
    original.history[1] = HU_ONBOARD_STEP_PROVIDER;
    strcpy(original.provider.provider_name, "gemini");
    original.provider.provider_smoke_passed = true;
    original.persona.template_choice = '2';
    strcpy(original.persona.markdown_path, "/tmp/my_persona.md");
    original.channels.imessage_enabled = true;
    original.channels.slack_enabled = true;
    original.channels.discord_enabled = false;
    original.channels.telegram_enabled = true;
    original.channels.imessage_fda_pending = true;
    strcpy(original.testsend.contact_handle, "alice@example.com");
    original.testsend.test_send_succeeded = true;

    /* Save and load */
    HU_ASSERT_EQ(hu_onboard_state_save(&original, path), HU_OK);

    hu_onboard_state_t loaded;
    memset(&loaded, 0xAA, sizeof(loaded)); /* pre-fill with garbage */
    HU_ASSERT_EQ(hu_onboard_state_load(&loaded, path), HU_OK);

    /* Verify all fields match */
    HU_ASSERT_EQ(loaded.schema_version, 1);
    HU_ASSERT_EQ(loaded.current, HU_ONBOARD_STEP_PROVIDER);
    HU_ASSERT_EQ(loaded.history_depth, 2);
    HU_ASSERT_EQ(loaded.history[0], HU_ONBOARD_STEP_WELCOME);
    HU_ASSERT_EQ(loaded.history[1], HU_ONBOARD_STEP_PROVIDER);
    HU_ASSERT(strcmp(loaded.provider.provider_name, "gemini") == 0);
    HU_ASSERT_TRUE(loaded.provider.provider_smoke_passed);
    HU_ASSERT_EQ(loaded.persona.template_choice, '2');
    HU_ASSERT(strcmp(loaded.persona.markdown_path, "/tmp/my_persona.md") == 0);
    HU_ASSERT_TRUE(loaded.channels.imessage_enabled);
    HU_ASSERT_TRUE(loaded.channels.slack_enabled);
    HU_ASSERT_FALSE(loaded.channels.discord_enabled);
    HU_ASSERT_TRUE(loaded.channels.telegram_enabled);
    HU_ASSERT_TRUE(loaded.channels.imessage_fda_pending);
    HU_ASSERT(strcmp(loaded.testsend.contact_handle, "alice@example.com") == 0);
    HU_ASSERT_TRUE(loaded.testsend.test_send_succeeded);

    /* Cleanup */
    (void)unlink(path);
    (void)rmdir(dir);
}

static void test_onboard_state_save_invalid_args(void) {
    hu_onboard_state_t state;
    hu_onboard_state_init(&state);

    /* NULL state */
    HU_ASSERT_EQ(hu_onboard_state_save(NULL, "/tmp/test"), HU_ERR_INVALID_ARGUMENT);

    /* NULL path */
    HU_ASSERT_EQ(hu_onboard_state_save(&state, NULL), HU_ERR_INVALID_ARGUMENT);

    /* Empty path */
    HU_ASSERT_EQ(hu_onboard_state_save(&state, ""), HU_ERR_INVALID_ARGUMENT);
}

static void test_onboard_state_load_invalid_args(void) {
    hu_onboard_state_t state;

    /* NULL out */
    HU_ASSERT_EQ(hu_onboard_state_load(NULL, "/tmp/test"), HU_ERR_INVALID_ARGUMENT);

    /* NULL path */
    HU_ASSERT_EQ(hu_onboard_state_load(&state, NULL), HU_ERR_INVALID_ARGUMENT);

    /* Empty path */
    HU_ASSERT_EQ(hu_onboard_state_load(&state, ""), HU_ERR_INVALID_ARGUMENT);
}

static void test_onboard_state_load_missing_file(void) {
    hu_onboard_state_t state;
    HU_ASSERT_EQ(hu_onboard_state_load(&state, "/tmp/nonexistent_onboard_state_12345"), HU_ERR_IO);
}

static void test_onboard_state_load_version_mismatch(void) {
    char tmpl[] = "/tmp/hu_onboard_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);

    char path[256];
    snprintf(path, sizeof(path), "%s/onboard-state.json", dir);

    /* Create a file with wrong version */
    FILE *fp = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(fp);

    uint32_t magic = 0x48554F42; /* "HUOB" */
    int version = 999;           /* Wrong version */
    HU_ASSERT_EQ((int)fwrite(&magic, sizeof(magic), 1, fp), 1);
    HU_ASSERT_EQ((int)fwrite(&version, sizeof(version), 1, fp), 1);
    fclose(fp);

    hu_onboard_state_t state;
    HU_ASSERT_EQ(hu_onboard_state_load(&state, path), HU_ERR_INVALID_ARGUMENT);

    /* Cleanup */
    (void)unlink(path);
    (void)rmdir(dir);
}

static void test_onboard_state_load_bad_magic(void) {
    char tmpl[] = "/tmp/hu_onboard_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);

    char path[256];
    snprintf(path, sizeof(path), "%s/onboard-state.json", dir);

    /* Create a file with wrong magic */
    FILE *fp = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(fp);

    uint32_t bad_magic = 0xDEADBEEF;
    int version = 1;
    HU_ASSERT_EQ((int)fwrite(&bad_magic, sizeof(bad_magic), 1, fp), 1);
    HU_ASSERT_EQ((int)fwrite(&version, sizeof(version), 1, fp), 1);
    fclose(fp);

    hu_onboard_state_t state;
    HU_ASSERT_EQ(hu_onboard_state_load(&state, path), HU_ERR_INVALID_ARGUMENT);

    /* Cleanup */
    (void)unlink(path);
    (void)rmdir(dir);
}

static void test_onboard_state_atomic_save_preserves_prior_on_tmp_blocked(void) {
    char tmpl[] = "/tmp/hu_onboard_atomic_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);

    char path[256];
    char tmp_path[260];
    snprintf(path, sizeof(path), "%s/onboard-state.json", dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    /* Step 1 — write a known-good state so <path> has previous state to preserve.
     * Use a recognizable current value so any mix-up shows up immediately. */
    hu_onboard_state_t known_good;
    hu_onboard_state_init(&known_good);
    known_good.current = HU_ONBOARD_STEP_WELCOME;
    strcpy(known_good.provider.provider_name, "known-good-provider");
    HU_ASSERT_EQ(hu_onboard_state_save(&known_good, path), HU_OK);

    /* Step 2 — block the atomic tmp slot by creating a directory at
     * <path>.tmp. fopen("wb") on a directory always fails with EISDIR (POSIX),
     * which forces the atomic save into its error path before any data has
     * been written to <path>. */
    HU_ASSERT_EQ(mkdir(tmp_path, 0755), 0);

    /* Step 3 — try to overwrite with a completely different state.
     * The atomic implementation must not touch <path> when tmp can't be opened.
     * A non-atomic implementation would open <path> directly via fopen("wb"),
     * truncating the prior state immediately. */
    hu_onboard_state_t big;
    hu_onboard_state_init(&big);
    big.current = HU_ONBOARD_STEP_COMPLETE;
    strcpy(big.provider.provider_name, "big-new-provider");
    /* We don't assert on the return value — both atomic and non-atomic
     * implementations could legitimately return HU_OK or HU_ERR_IO depending
     * on their internal layering. The contract being tested is FILE STATE. */
    (void)hu_onboard_state_save(&big, path);

    /* Step 4 — the file at <path> must still contain the known-good state.
     * Pre-fix (non-atomic), this fails because fopen(<path>, "wb") truncated
     * the file at the start of the save attempt. */
    hu_onboard_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    HU_ASSERT_EQ(hu_onboard_state_load(&loaded, path), HU_OK);
    HU_ASSERT_EQ(loaded.current, HU_ONBOARD_STEP_WELCOME);
    HU_ASSERT(strcmp(loaded.provider.provider_name, "known-good-provider") == 0);

    /* Cleanup */
    (void)rmdir(tmp_path);
    (void)unlink(path);
    (void)rmdir(dir);
}

void run_onboard_state_tests(void) {
    HU_TEST_SUITE("onboard_state");
    HU_RUN_TEST(test_onboard_state_init_defaults);
    HU_RUN_TEST(test_onboard_state_save_and_load_roundtrip);
    HU_RUN_TEST(test_onboard_state_save_invalid_args);
    HU_RUN_TEST(test_onboard_state_load_invalid_args);
    HU_RUN_TEST(test_onboard_state_load_missing_file);
    HU_RUN_TEST(test_onboard_state_load_version_mismatch);
    HU_RUN_TEST(test_onboard_state_load_bad_magic);
    HU_RUN_TEST(test_onboard_state_atomic_save_preserves_prior_on_tmp_blocked);
}
