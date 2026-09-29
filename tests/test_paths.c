/* tests/test_paths.c — contract tests for human/core/paths.h.
 *
 * These pin the two documented overrides that 74 of 79 call sites were
 * silently ignoring before the migration (HU_CHATDB: 13 of 17 files;
 * HU_STATE_DIR: 61 of 62). Each override test would FAIL against the old
 * hand-rolled idiom, which is the point: the helpers exist to make the
 * documented contract actually hold.
 *
 * Environment is saved and restored around every case so no test leaks a
 * HOME/override into its neighbours. */
#include "human/core/paths.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *home, *state, *chatdb;
} env_save_t;
static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}
static void env_save(env_save_t *e) {
    e->home = dup_or_null(getenv("HOME"));
    e->state = dup_or_null(getenv("HU_STATE_DIR"));
    e->chatdb = dup_or_null(getenv("HU_CHATDB"));
}
static void env_restore_one(const char *k, char *v) {
    if (v) {
        setenv(k, v, 1);
        free(v);
    } else
        unsetenv(k);
}
static void env_restore(env_save_t *e) {
    env_restore_one("HOME", e->home);
    env_restore_one("HU_STATE_DIR", e->state);
    env_restore_one("HU_CHATDB", e->chatdb);
}

static void paths_home_returns_snprintf_shaped_length(void) {
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/tmp/hu-home", 1);
    char buf[64];
    int n = hu_paths_home(buf, sizeof(buf));
    HU_ASSERT_EQ(n, (int)strlen("/tmp/hu-home"));
    HU_ASSERT_STR_EQ(buf, "/tmp/hu-home");
    env_restore(&e);
}

static void paths_home_unset_returns_negative(void) {
    env_save_t e;
    env_save(&e);
    unsetenv("HOME");
#if defined(_WIN32)
    unsetenv("USERPROFILE");
    unsetenv("HOMEDRIVE");
    unsetenv("HOMEPATH");
#endif
    char buf[64] = "sentinel";
    HU_ASSERT_LT(hu_paths_home(buf, sizeof(buf)), 0);
    env_restore(&e);
}

static void paths_home_truncation_reports_like_snprintf(void) {
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/a/very/long/home/directory/path", 1);
    char buf[8];
    int n = hu_paths_home(buf, sizeof(buf));
    /* snprintf contract: returns the full length, >= cap signals truncation,
     * and the buffer is still NUL-terminated. */
    HU_ASSERT_GE(n, (int)sizeof(buf));
    HU_ASSERT_EQ(buf[sizeof(buf) - 1], '\0');
    env_restore(&e);
}

static void paths_state_defaults_to_home_dot_human(void) {
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/tmp/hu-home", 1);
    unsetenv("HU_STATE_DIR");
    char buf[128];
    HU_ASSERT_GT(hu_paths_state(buf, sizeof(buf), NULL), 0);
    HU_ASSERT_STR_EQ(buf, "/tmp/hu-home/.human");
    HU_ASSERT_GT(hu_paths_state(buf, sizeof(buf), "memory.db"), 0);
    HU_ASSERT_STR_EQ(buf, "/tmp/hu-home/.human/memory.db");
    env_restore(&e);
}

static void paths_state_honors_hu_state_dir_override(void) {
    /* The contract doctor.h:198 documents and 61 of 62 files ignored. */
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/tmp/hu-home", 1);
    setenv("HU_STATE_DIR", "/var/hu-state", 1);
    char buf[128];
    HU_ASSERT_GT(hu_paths_state(buf, sizeof(buf), NULL), 0);
    HU_ASSERT_STR_EQ(buf, "/var/hu-state");
    HU_ASSERT_GT(hu_paths_state(buf, sizeof(buf), "graph.db"), 0);
    HU_ASSERT_STR_EQ(buf, "/var/hu-state/graph.db");
    HU_ASSERT_STR_NOT_CONTAINS(buf, ".human"); /* override must fully replace */
    env_restore(&e);
}

static void paths_state_empty_override_falls_through(void) {
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/tmp/hu-home", 1);
    setenv("HU_STATE_DIR", "", 1); /* set-but-empty must NOT mean "/" */
    char buf[128];
    HU_ASSERT_GT(hu_paths_state(buf, sizeof(buf), "x"), 0);
    HU_ASSERT_STR_EQ(buf, "/tmp/hu-home/.human/x");
    env_restore(&e);
}

static void paths_state_formats_relative_args(void) {
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/h", 1);
    unsetenv("HU_STATE_DIR");
    char buf[128];
    HU_ASSERT_GT(hu_paths_state(buf, sizeof(buf), "skills/%.256s.skill.json", "nifty"), 0);
    HU_ASSERT_STR_EQ(buf, "/h/.human/skills/nifty.skill.json");
    env_restore(&e);
}

static void paths_chatdb_defaults_to_library_messages(void) {
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/Users/x", 1);
    unsetenv("HU_CHATDB");
    char buf[128];
    HU_ASSERT_GT(hu_paths_chatdb(buf, sizeof(buf)), 0);
    HU_ASSERT_STR_EQ(buf, "/Users/x/Library/Messages/chat.db");
    env_restore(&e);
}

static void paths_chatdb_honors_hu_chatdb_override(void) {
    /* The contract config.h:294 documents and 13 of 17 files ignored. */
    env_save_t e;
    env_save(&e);
    setenv("HOME", "/Users/x", 1);
    setenv("HU_CHATDB", "/fixtures/chat.db", 1);
    char buf[128];
    HU_ASSERT_GT(hu_paths_chatdb(buf, sizeof(buf)), 0);
    HU_ASSERT_STR_EQ(buf, "/fixtures/chat.db");
    env_restore(&e);
}

static void paths_null_buffer_is_rejected(void) {
    HU_ASSERT_LT(hu_paths_home(NULL, 16), 0);
    HU_ASSERT_LT(hu_paths_state_dir(NULL, 16), 0);
    HU_ASSERT_LT(hu_paths_chatdb(NULL, 16), 0);
    char b[4];
    HU_ASSERT_LT(hu_paths_home(b, 0), 0);
}

static void paths_state_or_uses_fallback_only_when_unresolvable(void) {
    env_save_t e;
    env_save(&e);
    char buf[256];
    unsetenv("HOME");
    unsetenv("USERPROFILE");
    unsetenv("HOMEDRIVE");
    unsetenv("HOMEPATH");
    unsetenv("HU_STATE_DIR");
    int n = hu_paths_state_or(buf, sizeof(buf), "/tmp", "proofs/%d", 7);
    HU_ASSERT(n > 0);
    HU_ASSERT_STR_EQ(buf, "/tmp/.human/proofs/7");
    HU_ASSERT_EQ(hu_paths_state_or(buf, sizeof(buf), ".", NULL), (int)strlen("./.human"));
    HU_ASSERT_STR_EQ(buf, "./.human");
    /* strict form still refuses on the same environment */
    HU_ASSERT(hu_paths_state(buf, sizeof(buf), "x") < 0);
    /* resolvable → the fallback is ignored, HU_STATE_DIR wins as usual */
    setenv("HU_STATE_DIR", "/var/state", 1);
    HU_ASSERT(hu_paths_state_or(buf, sizeof(buf), "/tmp", "a") > 0);
    HU_ASSERT_STR_EQ(buf, "/var/state/a");
    env_restore(&e);
}

static void paths_chatdb_or_uses_fallback_only_when_unresolvable(void) {
    env_save_t e;
    env_save(&e);
    char buf[256];
    unsetenv("HOME");
    unsetenv("USERPROFILE");
    unsetenv("HOMEDRIVE");
    unsetenv("HOMEPATH");
    unsetenv("HU_CHATDB");
    HU_ASSERT(hu_paths_chatdb(buf, sizeof(buf)) < 0);
    HU_ASSERT(hu_paths_chatdb_or(buf, sizeof(buf), "/tmp") > 0);
    HU_ASSERT_STR_EQ(buf, "/tmp/Library/Messages/chat.db");
    setenv("HU_CHATDB", "/x/chat.db", 1);
    HU_ASSERT(hu_paths_chatdb_or(buf, sizeof(buf), "/tmp") > 0);
    HU_ASSERT_STR_EQ(buf, "/x/chat.db");
    env_restore(&e);
}

static void paths_state_or_null_fallback_is_strict(void) {
    env_save_t e;
    env_save(&e);
    char buf[64];
    unsetenv("HOME");
    unsetenv("USERPROFILE");
    unsetenv("HOMEDRIVE");
    unsetenv("HOMEPATH");
    unsetenv("HU_STATE_DIR");
    HU_ASSERT(hu_paths_state_or(buf, sizeof(buf), NULL, "x") < 0);
    HU_ASSERT(hu_paths_state_or(buf, sizeof(buf), "", "x") < 0);
    HU_ASSERT_EQ(buf[0], '\0');
    env_restore(&e);
}

static void paths_state_mkdir_creates_the_override_dir(void) {
    env_save_t e;
    env_save(&e);
    char want[256], buf[256];
    char parent[256];
    snprintf(parent, sizeof(parent), "/tmp/hu-paths-mkdir-%ld", (long)getpid());
    snprintf(want, sizeof(want), "%s/nested/.human", parent);
    (void)rmdir(want);
    setenv("HU_STATE_DIR", want, 1);
    HU_ASSERT(access(want, F_OK) != 0);
    int n = hu_paths_state_mkdir(buf, sizeof(buf));
    HU_ASSERT_EQ(n, (int)strlen(want));
    HU_ASSERT_STR_EQ(buf, want);
    HU_ASSERT_EQ(access(want, F_OK), 0);
    HU_ASSERT_EQ(hu_paths_state_mkdir(buf, sizeof(buf)), n); /* idempotent on EEXIST */
    (void)rmdir(want);
    snprintf(buf, sizeof(buf), "%s/nested", parent);
    (void)rmdir(buf);
    (void)rmdir(parent);
    env_restore(&e);
}

void run_paths_tests(void) {
    HU_TEST_SUITE("Core Paths");
    HU_RUN_TEST(paths_home_returns_snprintf_shaped_length);
    HU_RUN_TEST(paths_home_unset_returns_negative);
    HU_RUN_TEST(paths_home_truncation_reports_like_snprintf);
    HU_RUN_TEST(paths_state_defaults_to_home_dot_human);
    HU_RUN_TEST(paths_state_honors_hu_state_dir_override);
    HU_RUN_TEST(paths_state_empty_override_falls_through);
    HU_RUN_TEST(paths_state_formats_relative_args);
    HU_RUN_TEST(paths_chatdb_defaults_to_library_messages);
    HU_RUN_TEST(paths_chatdb_honors_hu_chatdb_override);
    HU_RUN_TEST(paths_null_buffer_is_rejected);
    HU_RUN_TEST(paths_state_or_uses_fallback_only_when_unresolvable);
    HU_RUN_TEST(paths_chatdb_or_uses_fallback_only_when_unresolvable);
    HU_RUN_TEST(paths_state_or_null_fallback_is_strict);
    HU_RUN_TEST(paths_state_mkdir_creates_the_override_dir);
}
