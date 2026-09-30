/* Config writes preserve what they do not touch.
 *
 * hu_config_save re-serialized an hu_config_t, so every top-level key its
 * serializer did not model (25 of the 57 the parser reads, including
 * initiative, reaction_collection, router, voice and policy) was erased from
 * config.json by `human workspace set` and by the gateway's config.set.
 * Writes now go through hu_config_mutator_mutate_at / replace_at, which edit
 * the parsed file. These tests run the real file I/O against scratch files. */
/* mkstemp is POSIX: glibc hides it under -std=c11 without this. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "human/config.h"
#include "human/config_mutator.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/json.h"
#include "human/gateway/control_protocol.h"
#include "test_framework.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HU_GATEWAY_POSIX
#include "cp_internal.h"
#endif

/* A config holding keys hu_config_save never wrote. */
static const char *rich_config = "{\"initiative\":{\"enabled\":true,\"threshold\":0.85},"
                                 "\"reaction_collection\":{\"enabled\":true},"
                                 "\"security\":{\"autonomy_level\":1},"
                                 "\"gateway\":{\"port\":3000}}";

static void scratch_config(char *path, size_t cap, const char *content) {
    snprintf(path, cap, "/tmp/hu-cfg-writes-XXXXXX");
    int fd = mkstemp(path);
    HU_ASSERT_TRUE(fd >= 0);
    if (content) {
        size_t n = strlen(content);
        HU_ASSERT_EQ((size_t)write(fd, content, n), n);
    }
    close(fd);
}

/* Returns malloc'd file contents, or NULL. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    if (buf)
        buf[n] = '\0';
    return buf;
}

static void remove_scratch(const char *path) {
    char bak[600];
    snprintf(bak, sizeof(bak), "%s.bak", path);
    unlink(path);
    unlink(bak);
}

static hu_error_t set_at(const char *path, const char *key, const char *value) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mutation_result_t res = {0};
    hu_mutation_options_t opts = {.apply = true};
    hu_error_t err =
        hu_config_mutator_mutate_at(&alloc, path, HU_MUTATION_SET, key, value, opts, &res);
    hu_config_mutator_free_result(&alloc, &res);
    return err;
}

static void mutate_set_keeps_keys_it_does_not_touch(void) {
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    HU_ASSERT_EQ(set_at(path, "security.autonomy_level", "2"), HU_OK);
    char *after = slurp(path);
    HU_ASSERT_NOT_NULL(after);
    HU_ASSERT_NOT_NULL(strstr(after, "\"initiative\""));
    HU_ASSERT_NOT_NULL(strstr(after, "\"threshold\":0.85"));
    HU_ASSERT_NOT_NULL(strstr(after, "\"reaction_collection\""));
    HU_ASSERT_NOT_NULL(strstr(after, "\"autonomy_level\":2"));
    free(after);
    remove_scratch(path);
}

static void mutate_set_keeps_the_previous_file_as_backup(void) {
    char path[512], bak[600];
    scratch_config(path, sizeof(path), rich_config);
    HU_ASSERT_EQ(set_at(path, "security.autonomy_level", "2"), HU_OK);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    char *saved = slurp(bak);
    HU_ASSERT_NOT_NULL(saved);
    HU_ASSERT_STR_EQ(saved, rich_config);
    free(saved);
    remove_scratch(path);
}

static void mutate_refuses_a_path_outside_the_allowlist(void) {
    /* The control protocol must not be able to switch off its own auth. */
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    HU_ASSERT_EQ(set_at(path, "gateway.require_pairing", "false"), HU_ERR_PERMISSION_DENIED);
    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, rich_config);
    free(after);
    remove_scratch(path);
}

static void mutate_refuses_a_value_of_the_wrong_type(void) {
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    HU_ASSERT_EQ(set_at(path, "gateway.port", "\"not-a-port\""), HU_ERR_INVALID_ARGUMENT);
    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, rich_config); /* nothing written */
    free(after);
    remove_scratch(path);
}

static void mutate_refuses_to_replace_an_unparseable_file(void) {
    /* Parsing garbage as "{}" and writing one key would erase the user's file. */
    const char *garbage = "{\"initiative\": {\"enabled\": tru";
    char path[512];
    scratch_config(path, sizeof(path), garbage);
    HU_ASSERT_EQ(set_at(path, "security.autonomy_level", "2"), HU_ERR_INVALID_ARGUMENT);
    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, garbage);
    free(after);
    remove_scratch(path);
}

static void mutate_unchanged_value_writes_nothing(void) {
    char path[512], bak[600];
    scratch_config(path, sizeof(path), rich_config);
    HU_ASSERT_EQ(set_at(path, "security.autonomy_level", "1"), HU_OK);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    HU_ASSERT_TRUE(access(bak, F_OK) != 0); /* no write, so no backup */
    remove_scratch(path);
}

#ifndef _WIN32
/* A failed write leaves the previous config.json byte-for-byte intact (the
 * atomic writer never truncates in place). A read-only directory forces the
 * failure: the file itself stays writable, so an in-place writer succeeds. */
static void failed_write_preserves_existing_file(void) {
    HU_SKIP_IF(geteuid() == 0, "root bypasses directory permissions");
    char dir[] = "/tmp/hu_cfg_atomic_XXXXXX";
    HU_ASSERT_NOT_NULL(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof(path), "%s/config.json", dir);
    int seed = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    HU_ASSERT_TRUE(seed >= 0);
    HU_ASSERT_EQ((size_t)write(seed, rich_config, strlen(rich_config)), strlen(rich_config));
    close(seed);

    HU_ASSERT_EQ(chmod(dir, 0500), 0);
    hu_error_t err = set_at(path, "security.autonomy_level", "2");
    HU_ASSERT_EQ(chmod(dir, 0700), 0);
    HU_ASSERT_NEQ(err, HU_OK);

    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, rich_config);
    free(after);
    unlink(path);
    rmdir(dir);
}
/* An unreadable config must be refused, not treated as missing: reading it
 * as "{}" would make the write replace the user's file with one key. */
static void mutate_refuses_an_unreadable_file(void) {
    HU_SKIP_IF(geteuid() == 0, "root reads mode-000 files");
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    HU_ASSERT_EQ(chmod(path, 0000), 0);
    hu_error_t err = set_at(path, "security.autonomy_level", "2");
    HU_ASSERT_EQ(chmod(path, 0600), 0);
    HU_ASSERT_EQ(err, HU_ERR_IO);
    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, rich_config);
    free(after);
    remove_scratch(path);
}
#endif

static void replace_writes_the_document_verbatim(void) {
    const char *doc =
        "{\"security\":{\"autonomy_level\":2},\n  \"initiative\":{\"enabled\":false}}";
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_config_mutator_replace_at(&alloc, path, doc, strlen(doc)), HU_OK);
    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, doc);
    free(after);
    remove_scratch(path);
}

static void replace_refuses_an_invalid_document(void) {
    const char *doc = "{\"gateway\":{\"port\":\"not-a-port\"}}";
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_config_mutator_replace_at(&alloc, path, doc, strlen(doc)),
                 HU_ERR_INVALID_ARGUMENT);
    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, rich_config);
    free(after);
    remove_scratch(path);
}

static void build_patch_nests_the_value_under_its_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *patch = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_config_mutator_build_patch(&alloc, "security.sandbox_config.enabled", "true",
                                               &patch, &len),
                 HU_OK);
    HU_ASSERT_STR_EQ(patch, "{\"security\":{\"sandbox_config\":{\"enabled\":true}}}");
    alloc.free(alloc.ctx, patch, len + 1);
}

#ifdef HU_GATEWAY_POSIX
static hu_error_t gateway_config_set(hu_config_t *cfg, const char *request, char **out,
                                     size_t *out_len) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_app_context_t app;
    memset(&app, 0, sizeof(app));
    app.config = cfg;
    app.alloc = &alloc;
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, request, strlen(request), &root), HU_OK);
    hu_error_t err = cp_config_set(&alloc, &app, NULL, NULL, root, out, out_len);
    hu_json_free(&alloc, root);
    return err;
}

static void loaded_config(hu_config_t *cfg, const char *path) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    memset(cfg, 0, sizeof(*cfg));
    hu_allocator_t arena_alloc = hu_arena_allocator(arena);
    hu_config_apply_defaults(cfg, &arena_alloc);
    cfg->arena = arena;
    cfg->allocator = arena_alloc;
    HU_ASSERT_EQ(hu_config_parse_json(cfg, rich_config, strlen(rich_config)), HU_OK);
    cfg->config_path = (char *)path; /* not arena-owned: the test clears it before deinit */
}

static void gateway_key_value_set_is_saved_and_applied(void) {
    /* The dashboard sends {key, value}; the handler used to read only "raw",
     * answer saved:false, and the dashboard toasted success anyway. */
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    hu_config_t cfg;
    loaded_config(&cfg, path);
    HU_ASSERT_EQ(cfg.security.autonomy_level, 1);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(gateway_config_set(&cfg,
                                    "{\"params\":{\"key\":\"security.autonomy_level\","
                                    "\"value\":2}}",
                                    &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "\"saved\":true"));
    HU_ASSERT_EQ(cfg.security.autonomy_level, 2); /* live config updated */
    char *after = slurp(path);
    HU_ASSERT_NOT_NULL(strstr(after, "\"autonomy_level\":2"));
    HU_ASSERT_NOT_NULL(strstr(after, "\"reaction_collection\"")); /* not erased */
    free(after);
    hu_allocator_t alloc = hu_system_allocator();
    alloc.free(alloc.ctx, out, out_len + 1);
    cfg.config_path = NULL;
    hu_config_deinit(&cfg);
    remove_scratch(path);
}

static void gateway_refusal_reports_not_saved_with_a_reason(void) {
    char path[512];
    scratch_config(path, sizeof(path), rich_config);
    hu_config_t cfg;
    loaded_config(&cfg, path);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(gateway_config_set(&cfg,
                                    "{\"params\":{\"key\":\"gateway.require_pairing\","
                                    "\"value\":false}}",
                                    &out, &out_len),
                 HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "\"saved\":false"));
    HU_ASSERT_NOT_NULL(strstr(out, "\"error\""));
    char *after = slurp(path);
    HU_ASSERT_STR_EQ(after, rich_config);
    free(after);
    hu_allocator_t alloc = hu_system_allocator();
    alloc.free(alloc.ctx, out, out_len + 1);
    cfg.config_path = NULL;
    hu_config_deinit(&cfg);
    remove_scratch(path);
}

static void gateway_without_a_config_file_writes_nothing(void) {
    /* An unloaded config must not fall back to the real ~/.human/config.json. */
    hu_config_t cfg;
    loaded_config(&cfg, NULL);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(gateway_config_set(&cfg, "{\"params\":{\"raw\":\"{}\"}}", &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "\"saved\":false"));
    hu_allocator_t alloc = hu_system_allocator();
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_config_deinit(&cfg);
}
#endif

void run_config_mutator_writes_tests(void) {
    HU_TEST_SUITE("config mutator writes");
    HU_RUN_TEST(mutate_set_keeps_keys_it_does_not_touch);
    HU_RUN_TEST(mutate_set_keeps_the_previous_file_as_backup);
    HU_RUN_TEST(mutate_refuses_a_path_outside_the_allowlist);
    HU_RUN_TEST(mutate_refuses_a_value_of_the_wrong_type);
    HU_RUN_TEST(mutate_refuses_to_replace_an_unparseable_file);
    HU_RUN_TEST(mutate_unchanged_value_writes_nothing);
#ifndef _WIN32
    HU_RUN_TEST(failed_write_preserves_existing_file);
    HU_RUN_TEST(mutate_refuses_an_unreadable_file);
#endif
    HU_RUN_TEST(replace_writes_the_document_verbatim);
    HU_RUN_TEST(replace_refuses_an_invalid_document);
    HU_RUN_TEST(build_patch_nests_the_value_under_its_path);
#ifdef HU_GATEWAY_POSIX
    HU_RUN_TEST(gateway_key_value_set_is_saved_and_applied);
    HU_RUN_TEST(gateway_refusal_reports_not_saved_with_a_reason);
    HU_RUN_TEST(gateway_without_a_config_file_writes_nothing);
#endif
}
