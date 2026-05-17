/* US-9.4: tests for `human doctor --install` gate predicate.
 *
 * Adversarial intent — these tests pin the contract that ANY red sub-check
 * MUST cause the predicate to return a non-OK error code.
 * Per `.claude/rules/tests-that-pin-bugs.md`, red-path tests use
 * HU_ASSERT_NE(rc, HU_OK) — NOT equality with a fixed integer — so a future
 * change that "fixes" the constant cannot silently re-introduce the bug.
 *
 * Each sub-check reads PRIMARY EVIDENCE on the live filesystem (not a
 * cached flag), so the tests synthesize that primary evidence directly:
 *
 *   binary       — HU_TEST_BINARY_PATH env override (HU_IS_TEST seam)
 *   config_dir   — temp $HOME with/without ~/.human/ present
 *   channel      — hu_config_t::channels.channel_config_{keys,counts,len}
 *   persona      — write/omit JSON at $HU_PERSONA_DIR/<name>.json
 */
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int s_tests_run = 0;
static int s_tests_passed = 0;

#define HU_ASSERT_EQ(a, b) assert((a) == (b))
#define HU_ASSERT_NE(a, b) assert((a) != (b))
#define HU_ASSERT_TRUE(c)  assert(c)
#define HU_ASSERT_FALSE(c) assert(!(c))

#define RUN(fn)                          \
    do {                                 \
        s_tests_run++;                   \
        fn();                            \
        s_tests_passed++;                \
        printf("    %s: passed\n", #fn); \
    } while (0)

static hu_allocator_t s_alloc;

/* --- helpers -------------------------------------------------------- */

static void free_items(hu_diag_item_t *items, size_t count, size_t cap) {
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            free((void *)items[i].category);
        if (items[i].message)
            free((void *)items[i].message);
    }
    free(items);
    (void)cap;
}

/* Build a temp directory tree under /tmp, return strdup'd path; caller
 * must rm -rf afterwards. */
static char *make_tempdir(const char *suffix) {
    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/hu_doctor_install_%s_XXXXXX", suffix);
    char *p = mkdtemp(tmpl);
    if (!p)
        return NULL;
    return strdup(p);
}

static void rm_rf(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(content, f);
    fclose(f);
}

/* Setup a fake $HOME tree with ~/.human present + a persona-dir set */
typedef struct env_sandbox {
    char *home;        /* temp dir used as $HOME */
    char *persona_dir; /* HU_PERSONA_DIR override */
    char *binary_path; /* HU_TEST_BINARY_PATH override (a real file) */
    char *prior_home;
    char *prior_pd;
    char *prior_bin;
} env_sandbox_t;

static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

static void env_set(const char *k, const char *v) {
    if (v)
        setenv(k, v, 1);
    else
        unsetenv(k);
}

static void sandbox_init(env_sandbox_t *sb, const char *suffix) {
    sb->prior_home = dup_or_null(getenv("HOME"));
    sb->prior_pd = dup_or_null(getenv("HU_PERSONA_DIR"));
    sb->prior_bin = dup_or_null(getenv("HU_TEST_BINARY_PATH"));

    sb->home = make_tempdir(suffix);
    assert(sb->home);

    /* Default: create ~/.human and personas dir so individual tests can
     * choose which piece to remove to make red. */
    char human_dir[1024];
    snprintf(human_dir, sizeof(human_dir), "%s/.human", sb->home);
    assert(mkdir(human_dir, 0700) == 0);

    char pd[1024];
    snprintf(pd, sizeof(pd), "%s/.human/personas", sb->home);
    assert(mkdir(pd, 0700) == 0);
    sb->persona_dir = strdup(pd);

    /* Touch a real file in $HOME and point HU_TEST_BINARY_PATH at it.
     * The binary check resolves and stats this path. */
    char bin[1024];
    snprintf(bin, sizeof(bin), "%s/human-bin", sb->home);
    write_file(bin, "#!/bin/sh\n");
    sb->binary_path = strdup(bin);

    env_set("HOME", sb->home);
    env_set("HU_PERSONA_DIR", sb->persona_dir);
    env_set("HU_TEST_BINARY_PATH", sb->binary_path);
}

static void sandbox_destroy(env_sandbox_t *sb) {
    if (sb->home) {
        rm_rf(sb->home);
        free(sb->home);
    }
    free(sb->persona_dir);
    free(sb->binary_path);
    env_set("HOME", sb->prior_home);
    env_set("HU_PERSONA_DIR", sb->prior_pd);
    env_set("HU_TEST_BINARY_PATH", sb->prior_bin);
    free(sb->prior_home);
    free(sb->prior_pd);
    free(sb->prior_bin);
    memset(sb, 0, sizeof(*sb));
}

/* Build a hu_config_t suitable for the green path. The persona name is
 * "tester"; the channel key is "imessage" with count 1. */
static void cfg_make_green(hu_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->agent.persona = strdup("tester");
    /* Mark one channel as configured. We allocate the key with strdup —
     * hu_config_deinit will not free this since we never called
     * hu_config_load. */
    cfg->channels.channel_config_keys[0] = strdup("imessage");
    cfg->channels.channel_config_counts[0] = 1;
    cfg->channels.channel_config_len = 1;
}

static void cfg_free_green(hu_config_t *cfg) {
    free(cfg->agent.persona);
    free(cfg->channels.channel_config_keys[0]);
    memset(cfg, 0, sizeof(*cfg));
}

static void write_persona_json(const env_sandbox_t *sb, const char *name, const char *json) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.json", sb->persona_dir, name);
    write_file(path, json);
}

/* Minimal-but-parseable persona file. The schema is lenient; an empty
 * object loads as a default persona. */
static const char *kGoodPersonaJson = "{\"name\":\"tester\",\"identity\":\"Tester\"}";
static const char *kBadPersonaJson = "{not valid json";

/* Truth-table helpers: count items by severity. */
static size_t count_severity(const hu_diag_item_t *items, size_t n, hu_diag_severity_t sev) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (items[i].severity == sev)
            c++;
    return c;
}

static bool category_is_err(const hu_diag_item_t *items, size_t n, const char *cat) {
    for (size_t i = 0; i < n; i++) {
        if (items[i].category && strcmp(items[i].category, cat) == 0)
            return items[i].severity == HU_DIAG_ERR;
    }
    return false;
}

static const char *find_message_for_category(const hu_diag_item_t *items, size_t n,
                                             const char *cat) {
    for (size_t i = 0; i < n; i++) {
        if (items[i].category && strcmp(items[i].category, cat) == 0)
            return items[i].message;
    }
    return NULL;
}

/* --- tests ---------------------------------------------------------- */

static void install_check_all_green_returns_ok_and_marks_ready(void) {
    env_sandbox_t sb;
    sandbox_init(&sb, "green");
    write_persona_json(&sb, "tester", kGoodPersonaJson);

    hu_config_t cfg;
    cfg_make_green(&cfg);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 8;
    items = malloc(sizeof(hu_diag_item_t) * cap);
    assert(items);

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &cfg, &items, &count, &cap);

    HU_ASSERT_EQ(rc, HU_OK);
    /* All four categories appear, all OK. */
    HU_ASSERT_EQ(count, 4u);
    HU_ASSERT_EQ(count_severity(items, count, HU_DIAG_OK), 4u);
    HU_ASSERT_EQ(count_severity(items, count, HU_DIAG_ERR), 0u);

    free_items(items, count, cap);
    cfg_free_green(&cfg);
    sandbox_destroy(&sb);
}

static void install_check_missing_binary_returns_not_found(void) {
    env_sandbox_t sb;
    sandbox_init(&sb, "binmiss");
    write_persona_json(&sb, "tester", kGoodPersonaJson);

    /* Point HU_TEST_BINARY_PATH at a path that does not exist. */
    char bogus[1024];
    snprintf(bogus, sizeof(bogus), "%s/does-not-exist", sb.home);
    env_set("HU_TEST_BINARY_PATH", bogus);

    hu_config_t cfg;
    cfg_make_green(&cfg);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 8;
    items = malloc(sizeof(hu_diag_item_t) * cap);

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &cfg, &items, &count, &cap);

    /* Adversarial: NOT equality with a magic integer — the rule is
     * "anything but HU_OK". */
    HU_ASSERT_NE(rc, HU_OK);
    HU_ASSERT_TRUE(category_is_err(items, count, "binary"));

    free_items(items, count, cap);
    cfg_free_green(&cfg);
    sandbox_destroy(&sb);
}

static void install_check_missing_config_dir_returns_not_found(void) {
    env_sandbox_t sb;
    sandbox_init(&sb, "cfgmiss");
    write_persona_json(&sb, "tester", kGoodPersonaJson);

    /* Remove ~/.human/ to break the config_dir check. */
    char human_dir[1024];
    snprintf(human_dir, sizeof(human_dir), "%s/.human", sb.home);
    rm_rf(human_dir);

    hu_config_t cfg;
    cfg_make_green(&cfg);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 8;
    items = malloc(sizeof(hu_diag_item_t) * cap);

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &cfg, &items, &count, &cap);

    HU_ASSERT_NE(rc, HU_OK);
    HU_ASSERT_TRUE(category_is_err(items, count, "config_dir"));

    free_items(items, count, cap);
    cfg_free_green(&cfg);
    sandbox_destroy(&sb);
}

static void install_check_no_channel_returns_not_found(void) {
    env_sandbox_t sb;
    sandbox_init(&sb, "nochan");
    write_persona_json(&sb, "tester", kGoodPersonaJson);

    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.agent.persona = strdup("tester");
    /* Channel array is empty — no configured channel. */

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 8;
    items = malloc(sizeof(hu_diag_item_t) * cap);

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &cfg, &items, &count, &cap);

    HU_ASSERT_NE(rc, HU_OK);
    HU_ASSERT_TRUE(category_is_err(items, count, "channel"));

    /* AC-9.4.2: the exact actionable message must appear. */
    const char *msg = find_message_for_category(items, count, "channel");
    HU_ASSERT_TRUE(msg != NULL);
    HU_ASSERT_TRUE(strstr(msg, "channel: NONE") != NULL);
    HU_ASSERT_TRUE(strstr(msg, "human doctor imessage") != NULL);

    free_items(items, count, cap);
    free(cfg.agent.persona);
    sandbox_destroy(&sb);
}

static void install_check_missing_persona_returns_not_found(void) {
    env_sandbox_t sb;
    sandbox_init(&sb, "permiss");
    /* Do NOT write the persona json. */

    hu_config_t cfg;
    cfg_make_green(&cfg);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 8;
    items = malloc(sizeof(hu_diag_item_t) * cap);

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &cfg, &items, &count, &cap);

    HU_ASSERT_NE(rc, HU_OK);
    HU_ASSERT_TRUE(category_is_err(items, count, "persona"));

    /* AC-9.4.3: the actionable hint text. */
    const char *msg = find_message_for_category(items, count, "persona");
    HU_ASSERT_TRUE(msg != NULL);
    HU_ASSERT_TRUE(strstr(msg, "persona: MISSING") != NULL);
    HU_ASSERT_TRUE(strstr(msg, "human doctor --fix") != NULL);

    free_items(items, count, cap);
    cfg_free_green(&cfg);
    sandbox_destroy(&sb);
}

static void install_check_unparseable_persona_returns_not_found(void) {
    env_sandbox_t sb;
    sandbox_init(&sb, "perbad");
    write_persona_json(&sb, "tester", kBadPersonaJson);

    hu_config_t cfg;
    cfg_make_green(&cfg);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 8;
    items = malloc(sizeof(hu_diag_item_t) * cap);

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &cfg, &items, &count, &cap);

    HU_ASSERT_NE(rc, HU_OK);
    HU_ASSERT_TRUE(category_is_err(items, count, "persona"));

    free_items(items, count, cap);
    cfg_free_green(&cfg);
    sandbox_destroy(&sb);
}

static void install_check_does_not_short_circuit_on_first_red(void) {
    /* Break BOTH config_dir AND persona. The contract: all four items
     * are appended even when an early one is red, so the user sees the
     * full picture in one run. */
    env_sandbox_t sb;
    sandbox_init(&sb, "multi");
    /* Don't write persona. Also nuke ~/.human/. */
    char human_dir[1024];
    snprintf(human_dir, sizeof(human_dir), "%s/.human", sb.home);
    rm_rf(human_dir);

    hu_config_t cfg;
    cfg_make_green(&cfg);

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 8;
    items = malloc(sizeof(hu_diag_item_t) * cap);

    hu_error_t rc = hu_doctor_check_install(&s_alloc, &cfg, &items, &count, &cap);

    HU_ASSERT_NE(rc, HU_OK);
    /* All four sub-checks ran. */
    HU_ASSERT_EQ(count, 4u);
    HU_ASSERT_TRUE(count_severity(items, count, HU_DIAG_ERR) >= 2u);

    free_items(items, count, cap);
    cfg_free_green(&cfg);
    sandbox_destroy(&sb);
}

int run_doctor_install_tests(void) {
    s_tests_run = 0;
    s_tests_passed = 0;
    s_alloc = hu_system_allocator();

    RUN(install_check_all_green_returns_ok_and_marks_ready);
    RUN(install_check_missing_binary_returns_not_found);
    RUN(install_check_missing_config_dir_returns_not_found);
    RUN(install_check_no_channel_returns_not_found);
    RUN(install_check_missing_persona_returns_not_found);
    RUN(install_check_unparseable_persona_returns_not_found);
    RUN(install_check_does_not_short_circuit_on_first_red);

    printf("  doctor_install: %d/%d passed\n", s_tests_passed, s_tests_run);
    return s_tests_run - s_tests_passed;
}
