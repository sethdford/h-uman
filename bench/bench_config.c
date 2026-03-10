/* Config parsing and access benchmarks. */
#include "bench.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
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

static void setup_config(hu_config_t *cfg) {
    hu_allocator_t backing = hu_system_allocator();
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = hu_arena_create(backing);
    cfg->allocator = hu_arena_allocator(cfg->arena);
}

static void teardown_config(hu_config_t *cfg) {
    hu_config_deinit(cfg);
}

void run_bench_config(void) {
    hu_config_t cfg;
    size_t json_len = strlen(typical_config_json);

    setup_config(&cfg);

    /* Parse typical config JSON */
    BENCH_RUN("config_parse_json", CONFIG_ITERATIONS, {
        hu_config_t c;
        memset(&c, 0, sizeof(c));
        c.arena = hu_arena_create(hu_system_allocator());
        c.allocator = hu_arena_allocator(c.arena);
        hu_config_parse_json(&c, typical_config_json, json_len);
        hu_config_deinit(&c);
    });

    /* Parse once, then access fields many times */
    hu_config_parse_json(&cfg, typical_config_json, json_len);

    BENCH_RUN("config_get_default_provider_key", CONFIG_ITERATIONS,
              { (void)hu_config_default_provider_key(&cfg); });

    BENCH_RUN("config_get_provider_key", CONFIG_ITERATIONS,
              { (void)hu_config_get_provider_key(&cfg, "openai"); });

    BENCH_RUN("config_get_provider_base_url", CONFIG_ITERATIONS,
              { (void)hu_config_get_provider_base_url(&cfg, "openai"); });

    teardown_config(&cfg);
}
