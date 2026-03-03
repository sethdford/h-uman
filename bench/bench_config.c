/* Config parsing and access benchmarks. */
#include "bench.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include <stdio.h>
#include <string.h>

#define CONFIG_ITERATIONS 10000

static const char *typical_config_json =
    "{\"workspace\":\"/tmp/bench\",\"default_provider\":\"openai\","
    "\"default_model\":\"gpt-4o\",\"default_temperature\":0.7,\"memory\":{\"backend\":\"markdown\","
    "\"auto_save\":true},\"gateway\":{\"enabled\":true,\"port\":3000,\"host\":\"127.0.0.1\"},"
    "\"autonomy\":{\"level\":\"supervised\",\"workspace_only\":true},"
    "\"providers\":[{\"name\":\"openai\",\"api_key\":\"sk-test\",\"base_url\":\"https://api.openai.com/v1\"},"
    "{\"name\":\"anthropic\",\"api_key\":\"sk-ant-test\"}]}";

static void setup_config(sc_config_t *cfg) {
    sc_allocator_t backing = sc_system_allocator();
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = sc_arena_create(backing);
    cfg->allocator = sc_arena_allocator(cfg->arena);
}

static void teardown_config(sc_config_t *cfg) {
    sc_config_deinit(cfg);
}

void run_bench_config(void) {
    sc_config_t cfg;
    size_t json_len = strlen(typical_config_json);

    setup_config(&cfg);

    /* Parse typical config JSON */
    BENCH_RUN("config_parse_json", CONFIG_ITERATIONS, {
        sc_config_t c;
        memset(&c, 0, sizeof(c));
        c.arena = sc_arena_create(sc_system_allocator());
        c.allocator = sc_arena_allocator(c.arena);
        sc_config_parse_json(&c, typical_config_json, json_len);
        sc_config_deinit(&c);
    });

    /* Parse once, then access fields many times */
    sc_config_parse_json(&cfg, typical_config_json, json_len);

    BENCH_RUN("config_get_default_provider_key", CONFIG_ITERATIONS,
              { (void)sc_config_default_provider_key(&cfg); });

    BENCH_RUN("config_get_provider_key", CONFIG_ITERATIONS,
              { (void)sc_config_get_provider_key(&cfg, "openai"); });

    BENCH_RUN("config_get_provider_base_url", CONFIG_ITERATIONS,
              { (void)sc_config_get_provider_base_url(&cfg, "openai"); });

    teardown_config(&cfg);
}
