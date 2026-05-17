/* Tests for newly ported modules (capabilities, channel_catalog, config_mutator, update, etc.) */
#include "human/agent/commands.h"
#include "human/agent/scheduler_status_json.h"
#include "human/capabilities.h"
#include "human/channel_adapters.h"
#include "human/channel_catalog.h"
#include "human/config.h"
#include "human/config_mutator.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/doctor.h"
#include "human/security.h"
#include "human/security/sandbox.h"
#include "human/service.h"
#include "human/update.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void doctor_free_semantics_result(hu_allocator_t *alloc, hu_diag_item_t *items,
                                         size_t count) {
    if (!items)
        return;
    for (size_t i = 0; i < count; i++) {
        if (items[i].category)
            alloc->free(alloc->ctx, (void *)items[i].category, strlen(items[i].category) + 1);
        if (items[i].message)
            alloc->free(alloc->ctx, (void *)items[i].message, strlen(items[i].message) + 1);
    }
    /* hu_doctor may realloc the buffer; system allocator ignores the byte count here. */
    alloc->free(alloc->ctx, items, sizeof(hu_diag_item_t) * count);
}

static bool doctor_diag_has_substr(hu_diag_item_t *items, size_t count, const char *needle) {
    for (size_t i = 0; i < count; i++) {
        if (items[i].message && strstr(items[i].message, needle))
            return true;
    }
    return false;
}

static void test_channel_catalog_all(void) {
    size_t n = 0;
    const hu_channel_meta_t *m = hu_channel_catalog_all(&n);
    HU_ASSERT_NOT_NULL(m);
    HU_ASSERT(n >= 1); /* at least cli when HU_HAS_CLI */
}

static void test_channel_catalog_find_by_key(void) {
    /* cli is always in catalog when built with CLI */
    const hu_channel_meta_t *t = hu_channel_catalog_find_by_key("cli");
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_STR_EQ(t->key, "cli");
}

static void test_channel_catalog_parse_peer_kind(void) {
    int r = hu_channel_adapters_parse_peer_kind("direct", 6);
    HU_ASSERT_EQ(r, (int)HU_CHAT_DIRECT);
    r = hu_channel_adapters_parse_peer_kind("group", 5);
    HU_ASSERT_EQ(r, (int)HU_CHAT_GROUP);
    r = hu_channel_adapters_parse_peer_kind("invalid", 7);
    HU_ASSERT_EQ(r, -1);
}

static void test_config_mutator_path_requires_restart(void) {
    HU_ASSERT_TRUE(hu_config_mutator_path_requires_restart("channels.telegram"));
    HU_ASSERT_TRUE(hu_config_mutator_path_requires_restart("memory.backend"));
    HU_ASSERT_FALSE(hu_config_mutator_path_requires_restart("default_temperature"));
}

static void test_doctor_parse_df(void) {
    const char *df =
        "Filesystem 1M-blocks Used Available Use% Mounted on\n/dev/sda1 1000 500 500 50% /\n";
    unsigned long mb = hu_doctor_parse_df_available_mb(df, strlen(df));
    HU_ASSERT_EQ(mb, 500ul);
}

static void test_scheduler_status_json_canonical(void) {
    unsigned long long jp = 0, jc = 0;
    long long bat = 0, ue = 0;
    char ac[16] = {0};
    const char *j = "{\n"
                    "  \"jobs_pending\": 3,\n"
                    "  \"jobs_completed_today\": 9,\n"
                    "  \"battery_pct\": 42,\n"
                    "  \"on_ac_power\": true,\n"
                    "  \"updated_epoch\": 1700000000\n"
                    "}\n";
    HU_ASSERT_EQ(hu_scheduler_status_parse_json(j, &jp, &jc, &bat, ac, sizeof(ac), &ue), HU_OK);
    HU_ASSERT_EQ(jp, 3ULL);
    HU_ASSERT_EQ(jc, 9ULL);
    HU_ASSERT_EQ(bat, 42LL);
    HU_ASSERT_STR_EQ(ac, "true");
    HU_ASSERT_EQ(ue, 1700000000LL);
}

static void test_scheduler_status_json_minified_reordered(void) {
    unsigned long long jp = 0, jc = 0;
    long long bat = 0, ue = 0;
    char ac[16] = {0};
    const char *j = "{\"updated_epoch\":100,\"jobs_pending\":1,\"on_ac_power\":false,"
                    "\"battery_pct\":-1,\"jobs_completed_today\":2}";
    HU_ASSERT_EQ(hu_scheduler_status_parse_json(j, &jp, &jc, &bat, ac, sizeof(ac), &ue), HU_OK);
    HU_ASSERT_EQ(jp, 1ULL);
    HU_ASSERT_EQ(jc, 2ULL);
    HU_ASSERT_EQ(bat, -1LL);
    HU_ASSERT_STR_EQ(ac, "false");
    HU_ASSERT_EQ(ue, 100LL);
}

static void test_scheduler_status_json_bad_json(void) {
    unsigned long long jp = 0, jc = 0;
    long long bat = 0, ue = 0;
    char ac[16] = {0};
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json("{ not json", &jp, &jc, &bat, ac, sizeof(ac), &ue),
                  HU_OK);
}

static void test_scheduler_status_json_null_args(void) {
    unsigned long long jp = 0, jc = 0;
    long long bat = 0, ue = 0;
    char ac[16] = {0};
    const char *ok = "{\"jobs_pending\":0,\"jobs_completed_today\":0,\"battery_pct\":0,"
                     "\"on_ac_power\":true,\"updated_epoch\":0}";
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json(NULL, &jp, &jc, &bat, ac, sizeof(ac), &ue), HU_OK);
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json(ok, NULL, &jc, &bat, ac, sizeof(ac), &ue), HU_OK);
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json(ok, &jp, NULL, &bat, ac, sizeof(ac), &ue), HU_OK);
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json(ok, &jp, &jc, NULL, ac, sizeof(ac), &ue), HU_OK);
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json(ok, &jp, &jc, &bat, NULL, sizeof(ac), &ue), HU_OK);
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json(ok, &jp, &jc, &bat, ac, 2, &ue), HU_OK);
    HU_ASSERT_NEQ(hu_scheduler_status_parse_json(ok, &jp, &jc, &bat, ac, sizeof(ac), NULL), HU_OK);
}

static void test_doctor_deprecated_scheduler_status_matches_shared(void) {
    unsigned long long jp_d = 0, jp_n = 0;
    unsigned long long jc_d = 0, jc_n = 0;
    long long bat_d = 0, bat_n = 0;
    long long ue_d = 0, ue_n = 0;
    char ac_d[16] = {0};
    char ac_n[16] = {0};
    const char *j = "{\"jobs_pending\":5,\"jobs_completed_today\":6,\"battery_pct\":7,"
                    "\"on_ac_power\":false,\"updated_epoch\":8}";
    HU_ASSERT_EQ(
        hu_doctor_parse_scheduler_status_json(j, &jp_d, &jc_d, &bat_d, ac_d, sizeof(ac_d), &ue_d),
        HU_OK);
    HU_ASSERT_EQ(hu_scheduler_status_parse_json(j, &jp_n, &jc_n, &bat_n, ac_n, sizeof(ac_n), &ue_n),
                 HU_OK);
    HU_ASSERT_EQ(jp_d, jp_n);
    HU_ASSERT_EQ(jc_d, jc_n);
    HU_ASSERT_EQ(bat_d, bat_n);
    HU_ASSERT_STR_EQ(ac_d, ac_n);
    HU_ASSERT_EQ(ue_d, ue_n);
}

static void doctor_sch_write_status(const char *home, const char *body) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.human", home);
    mkdir(home, 0700);
    mkdir(dir, 0700);
    char path[512];
    snprintf(path, sizeof(path), "%s/.human/scheduler.status", home);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(body, f);
        fclose(f);
    }
}

static void doctor_sch_remove_status(const char *home) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.human/scheduler.status", home);
    unlink(path);
}

static void doctor_sch_swap_home(const char *new_home, char **old_out) {
    const char *h = getenv("HOME");
    *old_out = h ? strdup(h) : NULL;
    setenv("HOME", new_home, 1);
}

static void doctor_sch_restore_home(char *old) {
    if (old) {
        setenv("HOME", old, 1);
        free(old);
    } else {
        unsetenv("HOME");
    }
}

static void test_doctor_check_scheduler_minified_file(void) {
    char *old = NULL;
    doctor_sch_swap_home("/tmp/hu_doctor_sch_parse", &old);
    doctor_sch_write_status("/tmp/hu_doctor_sch_parse",
                            "{\"updated_epoch\":5000,\"jobs_pending\":7,\"on_ac_power\":true,"
                            "\"battery_pct\":88,\"jobs_completed_today\":3}");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    /* status_age = 100s — threshold must exceed age or we take the STALE branch */
    HU_ASSERT_EQ(hu_doctor_check_scheduler(&alloc, 5100, 3600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "pending=7"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "completed_24h=3"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "battery_pct=88"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "on_ac=true"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "fresh (status_age=100s)"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_sch_remove_status("/tmp/hu_doctor_sch_parse");
    doctor_sch_restore_home(old);
}

static void test_doctor_check_scheduler_stale_warn(void) {
    char *old = NULL;
    doctor_sch_swap_home("/tmp/hu_doctor_sch_stale", &old);
    doctor_sch_write_status("/tmp/hu_doctor_sch_stale",
                            "{\"jobs_pending\":0,\"jobs_completed_today\":0,\"battery_pct\":100,"
                            "\"on_ac_power\":false,\"updated_epoch\":1000}");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    HU_ASSERT_EQ(hu_doctor_check_scheduler(&alloc, 6000, 3600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "STALE"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_sch_remove_status("/tmp/hu_doctor_sch_stale");
    doctor_sch_restore_home(old);
}

static void test_doctor_truncate_null_alloc(void) {
    char *out = (char *)0x1;
    hu_error_t err = hu_doctor_truncate_for_display(NULL, "hello", 5, 10, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_truncate_null_string(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = (char *)0x1;
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, NULL, 0, 10, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
}

static void test_doctor_truncate_zero_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    const char *s = "hello";
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, s, 0, 10, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "hello");
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_doctor_truncate_normal_truncation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    const char *s = "hello world";
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, s, 11, 5, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_EQ(strlen(out), 5u);
    HU_ASSERT_TRUE(strncmp(out, "hello", 5) == 0);
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_doctor_truncate_shorter_than_max(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    const char *s = "hi";
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, s, 2, 10, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_STR_EQ(out, "hi");
    alloc.free(alloc.ctx, out, strlen(out) + 1);
}

static void test_doctor_truncate_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_doctor_truncate_for_display(&alloc, "hello", 5, 10, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_null_alloc(void) {
    hu_config_t cfg = {0};
    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(NULL, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_null_cfg(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, NULL, &items, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "openai";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, NULL, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_doctor_check_config_valid_with_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "openai";
    char key[] = "telegram";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;
    cfg.channels.channel_config_keys[0] = key;
    cfg.channels.channel_config_counts[0] = 1;
    cfg.channels.channel_config_len = 1;

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_TRUE(count > 0);

    doctor_free_semantics_result(&alloc, items, count);
}

static void test_doctor_semantics_sqlite_backend_line(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "ollama";
    char mem[] = "sqlite";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;
    cfg.memory_backend = mem;

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "[doctor] SQLite:"));
#ifdef HU_ENABLE_SQLITE
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "available"));
#else
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "not compiled in"));
#endif
    doctor_free_semantics_result(&alloc, items, count);
}

static void test_doctor_semantics_http_line_when_gateway_enabled(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "ollama";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;
    cfg.gateway.enabled = true;

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "[doctor] HTTP client:"));
#if HU_IS_TEST
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "OK"));
#endif
    doctor_free_semantics_result(&alloc, items, count);
}

static void test_doctor_semantics_persona_line_when_configured(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "ollama";
    char persona[] = "assistant";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;
    cfg.agent.persona = persona;

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "[doctor] Persona dir:"));
    doctor_free_semantics_result(&alloc, items, count);
}

#if HU_IS_TEST
static void test_doctor_semantics_local_inference_ok_in_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_config_t cfg = {0};
    char prov[] = "ollama";
    cfg.default_provider = prov;
    cfg.default_temperature = 0.7;
    cfg.gateway.port = 3000;

    hu_diag_item_t *items = NULL;
    size_t count = 0;
    hu_error_t err = hu_doctor_check_config_semantics(&alloc, &cfg, &items, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "[doctor] Ollama (localhost:11434): OK"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "[doctor] llama-cli (PATH): OK"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "[doctor] Embedded model provider:"));
    doctor_free_semantics_result(&alloc, items, count);
}
#endif

static void test_agent_commands_parse(void) {
    const hu_slash_cmd_t *c = hu_agent_commands_parse("/new", 4);
    HU_ASSERT_NOT_NULL(c);
    HU_ASSERT_STR_EQ(c->name, "new");
}

static void test_agent_commands_bare_reset_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    hu_error_t err = hu_agent_commands_bare_session_reset_prompt(&alloc, "/new", 4, &prompt);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT(strstr(prompt, "Session Startup") != NULL);
    alloc.free(alloc.ctx, prompt, strlen(prompt) + 1);
}

static void test_rate_tracker(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_rate_tracker_t *t = hu_rate_tracker_create(&alloc, 3);
    HU_ASSERT_NOT_NULL(t);
    HU_ASSERT_FALSE(hu_rate_tracker_is_limited(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_record_action(t));
    HU_ASSERT_FALSE(hu_rate_tracker_record_action(t));
    HU_ASSERT_TRUE(hu_rate_tracker_is_limited(t));
    hu_rate_tracker_destroy(t);
}

static void test_sandbox_create_noop(void) {
    hu_sandbox_t sb = hu_sandbox_create_noop();
    HU_ASSERT_TRUE(hu_sandbox_is_available(&sb));
    HU_ASSERT_STR_EQ(hu_sandbox_name(&sb), "none");
}

static void test_capabilities_manifest(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    hu_error_t err = hu_capabilities_build_manifest_json(&alloc, NULL, NULL, 0, &json);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT(strstr(json, "\"channels\"") != NULL);
    HU_ASSERT(strstr(json, "\"memory_engines\"") != NULL);
    alloc.free(alloc.ctx, json, strlen(json) + 1);
}

static void test_config_mutator_mutate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mutation_result_t res = {0};
    hu_error_t err = hu_config_mutator_mutate(&alloc, HU_MUTATION_SET, "default_temperature", "0.5",
                                              (hu_mutation_options_t){.apply = false}, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(res.path);
    HU_ASSERT_STR_EQ(res.path, "default_temperature");
    hu_config_mutator_free_result(&alloc, &res);
}

static void test_config_mutator_mutate_denied_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mutation_result_t res = {0};
    hu_error_t err = hu_config_mutator_mutate(&alloc, HU_MUTATION_SET, "identity.format", "compact",
                                              (hu_mutation_options_t){.apply = false}, &res);
    HU_ASSERT_EQ(err, HU_ERR_PERMISSION_DENIED);
}

static void test_config_mutator_get_path_denied(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *json = NULL;
    hu_error_t err = hu_config_mutator_get_path_value_json(&alloc, "identity.format", &json);
    HU_ASSERT_EQ(err, HU_ERR_PERMISSION_DENIED);
    HU_ASSERT_NULL(json);
}

static void test_config_mutator_mutate_unset(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mutation_result_t res = {0};
    hu_error_t err = hu_config_mutator_mutate(&alloc, HU_MUTATION_UNSET, "memory.backend", NULL,
                                              (hu_mutation_options_t){.apply = false}, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(res.path, "memory.backend");
    HU_ASSERT_TRUE(res.requires_restart);
    hu_config_mutator_free_result(&alloc, &res);
}

static void test_update_check_mock(void) {
    /* In HU_IS_TEST mode, returns mock version without network calls */
    char buf[64];
    hu_error_t err = hu_update_check(buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strlen(buf) > 0);
    HU_ASSERT_TRUE(strstr(buf, "mock") != NULL || strstr(buf, "99") != NULL);
}

static void test_update_apply_mock(void) {
    /* In HU_IS_TEST mode, returns HU_OK without applying */
    hu_error_t err = hu_update_apply();
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_service_start_stop(void) {
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_service_status());
    hu_service_stop();
    HU_ASSERT_FALSE(hu_service_status());
}

static void test_service_configure_null(void) {
    hu_service_configure(NULL, NULL);
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    hu_service_stop();
}

static void test_service_double_start(void) {
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    hu_service_stop();
}

static void test_service_configure_with_ctx(void) {
    hu_channel_loop_ctx_t ctx = {0};
    hu_channel_loop_state_t state = {0};
    hu_service_configure(&ctx, &state);
    hu_error_t err = hu_service_start();
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_service_status());
    hu_service_stop();
    hu_service_configure(NULL, NULL);
}

/* ── hu_doctor_check_imessage red-team ─────────────────────────────────
 * Cover every failure mode we've encountered or can credibly imagine:
 *   • null arguments
 *   • status file absent (daemon never polled)
 *   • status file present and fresh (success)
 *   • status file stale (warn — silent failure)
 *   • status file shows partial failures but breaker not yet tripped (warn)
 *   • status file shows breaker tripped (error)
 *   • status file is corrupt / partial JSON (no crash, no false OK)
 *   • HOME unset (defensive)
 *   • imsg CLI absent on PATH is exercised implicitly under HU_IS_TEST. */

static void doctor_imsg_swap_home(const char *new_home, char **old_out) {
    const char *h = getenv("HOME");
    *old_out = h ? strdup(h) : NULL;
    setenv("HOME", new_home, 1);
}

static void doctor_imsg_restore_home(char *old) {
    if (old) {
        setenv("HOME", old, 1);
        free(old);
    } else {
        unsetenv("HOME");
    }
}

static void doctor_imsg_write_status(const char *home, const char *body) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.human", home);
    mkdir(home, 0700);
    mkdir(dir, 0700);
    char path[512];
    snprintf(path, sizeof(path), "%s/.human/imessage.poll_status", home);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(body, f);
        fclose(f);
    }
}

static void doctor_imsg_remove_status(const char *home) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.human/imessage.poll_status", home);
    unlink(path);
}

static void test_doctor_check_imessage_null_args_rejected(void) {
    hu_diag_item_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;
    HU_ASSERT_EQ(hu_doctor_check_imessage(NULL, 0, 0, &items, &count, &cap),
                 HU_ERR_INVALID_ARGUMENT);
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 0, 0, NULL, &count, &cap),
                 HU_ERR_INVALID_ARGUMENT);
}

#if HU_HAS_IMESSAGE
static void test_doctor_check_imessage_no_status_file_warns(void) {
    char *old = NULL;
    doctor_imsg_swap_home("/tmp/hu_doctor_imsg_no_status", &old);
    doctor_imsg_remove_status("/tmp/hu_doctor_imsg_no_status");

    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 1000, 600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "poll status: file missing"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_imsg_restore_home(old);
}

static void test_doctor_check_imessage_breaker_tripped_reports_error(void) {
    /* US-9.6: breaker output now uses the shared presentation predicate.
     * Wording shifted from a generic "TRIPPED ... re-grant FDA" line to a
     * class-aware explanation that names the underlying class AND
     * suggests `human doctor --fix`. We still assert "TRIPPED" and "AUTH"
     * (both still present) and add a substring for the new --fix
     * suggestion so any future drift is caught. */
    char *old = NULL;
    doctor_imsg_swap_home("/tmp/hu_doctor_imsg_tripped", &old);
    doctor_imsg_write_status("/tmp/hu_doctor_imsg_tripped", "{\n"
                                                            "  \"last_rowid\": 12345,\n"
                                                            "  \"last_successful_poll_epoch\": 0,\n"
                                                            "  \"consecutive_open_failures\": 9,\n"
                                                            "  \"circuit_breaker_tripped\": true,\n"
                                                            "  \"last_error_class\": \"AUTH\"\n"
                                                            "}\n");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 1000, 600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "TRIPPED"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "AUTH"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "Full Disk Access"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "human doctor --fix"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_imsg_remove_status("/tmp/hu_doctor_imsg_tripped");
    doctor_imsg_restore_home(old);
}

static void test_doctor_check_imessage_fresh_poll_reports_ok(void) {
    char *old = NULL;
    doctor_imsg_swap_home("/tmp/hu_doctor_imsg_fresh", &old);
    doctor_imsg_write_status("/tmp/hu_doctor_imsg_fresh",
                             "{\n"
                             "  \"last_rowid\": 5000,\n"
                             "  \"last_successful_poll_epoch\": 1000,\n"
                             "  \"consecutive_open_failures\": 0,\n"
                             "  \"circuit_breaker_tripped\": false,\n"
                             "  \"last_error_class\": \"NONE\"\n"
                             "}\n");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    /* now=1010, threshold=600 → age=10s → fresh.
     * US-9.6: when `last_error_class=NONE` the predicate emits a positive
     * "healthy" line in place of the old "circuit breaker: OK" — the
     * latter is now reserved for the no-state-recorded path. */
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 1010, 600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "chat.db: healthy"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "poll: fresh"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_imsg_remove_status("/tmp/hu_doctor_imsg_fresh");
    doctor_imsg_restore_home(old);
}

static void test_doctor_check_imessage_stale_poll_reports_warn(void) {
    char *old = NULL;
    doctor_imsg_swap_home("/tmp/hu_doctor_imsg_stale", &old);
    doctor_imsg_write_status("/tmp/hu_doctor_imsg_stale",
                             "{\n"
                             "  \"last_rowid\": 5000,\n"
                             "  \"last_successful_poll_epoch\": 1000,\n"
                             "  \"consecutive_open_failures\": 0,\n"
                             "  \"circuit_breaker_tripped\": false,\n"
                             "  \"last_error_class\": \"NONE\"\n"
                             "}\n");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    /* now=2000, threshold=600 → age=1000s → stale */
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 2000, 600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "poll: STALE"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_imsg_remove_status("/tmp/hu_doctor_imsg_stale");
    doctor_imsg_restore_home(old);
}

static void test_doctor_check_imessage_partial_failures_warns(void) {
    /* US-9.6: when consecutive_open_failures>0 but the breaker has not
     * tripped, the doctor now routes through the shared presentation
     * predicate using the underlying `last_error_class`. For AUTH (this
     * fixture) the output points at Full Disk Access — that's
     * actionable and per AC-9.6.1. The legacy "circuit breaker: OK (N
     * recent failures, last=X)" wording was replaced because it didn't
     * tell the user what to fix. */
    char *old = NULL;
    doctor_imsg_swap_home("/tmp/hu_doctor_imsg_partial", &old);
    doctor_imsg_write_status("/tmp/hu_doctor_imsg_partial",
                             "{\n"
                             "  \"last_rowid\": 5000,\n"
                             "  \"last_successful_poll_epoch\": 990,\n"
                             "  \"consecutive_open_failures\": 3,\n"
                             "  \"circuit_breaker_tripped\": false,\n"
                             "  \"last_error_class\": \"AUTH\"\n"
                             "}\n");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 1000, 600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "Full Disk Access"));
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "System Settings"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_imsg_remove_status("/tmp/hu_doctor_imsg_partial");
    doctor_imsg_restore_home(old);
}

static void test_doctor_check_imessage_corrupt_status_does_not_crash(void) {
    char *old = NULL;
    doctor_imsg_swap_home("/tmp/hu_doctor_imsg_corrupt", &old);
    /* Truncated / garbage JSON. Must not crash, must not falsely report fresh. */
    doctor_imsg_write_status("/tmp/hu_doctor_imsg_corrupt", "{ this is not json");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 1000, 600, &items, &count, &cap), HU_OK);
    /* With unparseable fields, last_success stays 0 → must report "never recorded". */
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "never recorded a successful poll"));
    doctor_free_semantics_result(&alloc, items, count);
    doctor_imsg_remove_status("/tmp/hu_doctor_imsg_corrupt");
    doctor_imsg_restore_home(old);
}

static void test_doctor_check_imessage_no_home_reports_error(void) {
    const char *h = getenv("HOME");
    char *old = h ? strdup(h) : NULL;
    unsetenv("HOME");
    hu_allocator_t alloc = hu_system_allocator();
    hu_diag_item_t *items = (hu_diag_item_t *)alloc.alloc(alloc.ctx, sizeof(hu_diag_item_t) * 8);
    size_t count = 0;
    size_t cap = 8;
    HU_ASSERT_EQ(hu_doctor_check_imessage(&alloc, 0, 600, &items, &count, &cap), HU_OK);
    HU_ASSERT_TRUE(doctor_diag_has_substr(items, count, "$HOME is not set"));
    doctor_free_semantics_result(&alloc, items, count);
    if (old) {
        setenv("HOME", old, 1);
        free(old);
    }
}
#endif

void run_ported_modules_tests(void) {
    HU_TEST_SUITE("Ported Modules");
    HU_RUN_TEST(test_channel_catalog_all);
    HU_RUN_TEST(test_channel_catalog_find_by_key);
    HU_RUN_TEST(test_channel_catalog_parse_peer_kind);
    HU_RUN_TEST(test_config_mutator_path_requires_restart);
    HU_RUN_TEST(test_config_mutator_get_path_denied);
    HU_RUN_TEST(test_config_mutator_mutate_denied_path);
    HU_RUN_TEST(test_config_mutator_mutate_unset);
    HU_RUN_TEST(test_doctor_parse_df);
    HU_RUN_TEST(test_scheduler_status_json_canonical);
    HU_RUN_TEST(test_scheduler_status_json_minified_reordered);
    HU_RUN_TEST(test_scheduler_status_json_bad_json);
    HU_RUN_TEST(test_scheduler_status_json_null_args);
    HU_RUN_TEST(test_doctor_deprecated_scheduler_status_matches_shared);
    HU_RUN_TEST(test_doctor_check_scheduler_minified_file);
    HU_RUN_TEST(test_doctor_check_scheduler_stale_warn);
    HU_RUN_TEST(test_doctor_truncate_null_alloc);
    HU_RUN_TEST(test_doctor_truncate_null_string);
    HU_RUN_TEST(test_doctor_truncate_zero_len);
    HU_RUN_TEST(test_doctor_truncate_normal_truncation);
    HU_RUN_TEST(test_doctor_truncate_shorter_than_max);
    HU_RUN_TEST(test_doctor_truncate_null_out);
    HU_RUN_TEST(test_doctor_check_config_null_alloc);
    HU_RUN_TEST(test_doctor_check_config_null_cfg);
    HU_RUN_TEST(test_doctor_check_config_null_out);
    HU_RUN_TEST(test_doctor_check_config_valid_with_defaults);
    HU_RUN_TEST(test_doctor_semantics_sqlite_backend_line);
    HU_RUN_TEST(test_doctor_semantics_http_line_when_gateway_enabled);
    HU_RUN_TEST(test_doctor_semantics_persona_line_when_configured);
#if HU_IS_TEST
    HU_RUN_TEST(test_doctor_semantics_local_inference_ok_in_test_mode);
#endif
    HU_RUN_TEST(test_agent_commands_parse);
    HU_RUN_TEST(test_agent_commands_bare_reset_prompt);
    HU_RUN_TEST(test_rate_tracker);
    HU_RUN_TEST(test_sandbox_create_noop);
    HU_RUN_TEST(test_capabilities_manifest);
    HU_RUN_TEST(test_config_mutator_mutate);
    HU_RUN_TEST(test_update_check_mock);
    HU_RUN_TEST(test_update_apply_mock);
    HU_RUN_TEST(test_service_start_stop);
    HU_RUN_TEST(test_service_configure_null);
    HU_RUN_TEST(test_service_double_start);
    HU_RUN_TEST(test_service_configure_with_ctx);

    /* hu_doctor_check_imessage red-team */
    HU_RUN_TEST(test_doctor_check_imessage_null_args_rejected);
#if HU_HAS_IMESSAGE
    HU_RUN_TEST(test_doctor_check_imessage_no_status_file_warns);
    HU_RUN_TEST(test_doctor_check_imessage_breaker_tripped_reports_error);
    HU_RUN_TEST(test_doctor_check_imessage_fresh_poll_reports_ok);
    HU_RUN_TEST(test_doctor_check_imessage_stale_poll_reports_warn);
    HU_RUN_TEST(test_doctor_check_imessage_partial_failures_warns);
    HU_RUN_TEST(test_doctor_check_imessage_corrupt_status_does_not_crash);
    HU_RUN_TEST(test_doctor_check_imessage_no_home_reports_error);
#endif
}
