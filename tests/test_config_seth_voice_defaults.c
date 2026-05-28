/* AC-1 & AC-4: Seth-voice default config (mlx_local_routing tri-state + reaction_collection).
 * Tests for the Phase 1 config changes: tri-state routing policy for local-voice LoRA,
 * and reaction_collection enabled by default for fresh configs. */
#include "human/config.h"
#include "human/config_parse.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hu_config_t *test_config_create(const char *json) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    /* Mirror the production load path: static defaults first, then overlay
     * parsed JSON. hu_config_parse_json alone does NOT seed set_defaults()
     * values (routing=AUTO, reaction_collection.enabled=true).
     *
     * set_defaults() memsets the whole config, so we MUST (a) pass a standalone
     * allocator — never &cfg->allocator, which the memset would zero mid-call,
     * NULLing the alloc fn — and (b) wire cfg->arena/allocator AFTER defaults so
     * they survive the memset and test_config_destroy can free the arena. */
    hu_allocator_t arena_alloc = hu_arena_allocator(arena);
    hu_config_apply_defaults(cfg, &arena_alloc);
    cfg->arena = arena;
    cfg->allocator = arena_alloc;
    if (json && json[0]) {
        hu_error_t err = hu_config_parse_json(cfg, json, strlen(json));
        HU_ASSERT_EQ(err, HU_OK);
    }
    return cfg;
}

static void test_config_destroy(hu_config_t *cfg) {
    if (!cfg)
        return;
    hu_config_deinit(cfg);
    hu_allocator_t backing = hu_system_allocator();
    backing.free(backing.ctx, cfg, sizeof(hu_config_t));
}

/* AC-1: Fresh config (empty JSON) seeds mlx_local_routing = AUTO. */
static void fresh_config_routing_defaults_to_auto(void) {
    hu_config_t *cfg = test_config_create("{}");
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_AUTO);
    test_config_destroy(cfg);
}

/* AC-1: Explicit mlx_local_routing = "OFF" is parsed correctly. */
static void mlx_local_routing_off_parses(void) {
    const char *json = "{\"agent\":{\"model_router\":{\"mlx_local_routing\":\"OFF\"}}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_OFF);
    test_config_destroy(cfg);
}

/* AC-1: Explicit mlx_local_routing = "AUTO" is parsed correctly. */
static void mlx_local_routing_auto_parses(void) {
    const char *json = "{\"agent\":{\"model_router\":{\"mlx_local_routing\":\"AUTO\"}}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_AUTO);
    test_config_destroy(cfg);
}

/* AC-1: Explicit mlx_local_routing = "FORCE" is parsed correctly. */
static void mlx_local_routing_force_parses(void) {
    const char *json = "{\"agent\":{\"model_router\":{\"mlx_local_routing\":\"FORCE\"}}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_FORCE);
    test_config_destroy(cfg);
}

/* AC-1: Legacy compat—mlx_local_enabled=true maps to FORCE. */
static void legacy_mlx_local_enabled_true_maps_to_force(void) {
    const char *json = "{\"agent\":{\"model_router\":{\"mlx_local_enabled\":true}}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_FORCE);
    test_config_destroy(cfg);
}

/* AC-1: Legacy compat—mlx_local_enabled=false maps to AUTO. */
static void legacy_mlx_local_enabled_false_maps_to_auto(void) {
    const char *json = "{\"agent\":{\"model_router\":{\"mlx_local_enabled\":false}}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_AUTO);
    test_config_destroy(cfg);
}

/* AC-1: mlx_local_routing takes precedence over legacy mlx_local_enabled. */
static void mlx_local_routing_takes_precedence(void) {
    const char *json =
        "{\"agent\":{\"model_router\":{\"mlx_local_routing\":\"OFF\",\"mlx_local_enabled\":true}}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_OFF);
    test_config_destroy(cfg);
}

/* AC-1: Unknown mlx_local_routing value logs warning and defaults to AUTO. */
static void unknown_mlx_local_routing_defaults_to_auto(void) {
    const char *json = "{\"agent\":{\"model_router\":{\"mlx_local_routing\":\"UNKNOWN\"}}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_EQ(cfg->agent.mlx_local_routing, HU_MLX_LOCAL_ROUTING_AUTO);
    test_config_destroy(cfg);
}

/* AC-4: Fresh config seeds reaction_collection.enabled = true. */
static void fresh_config_reaction_collection_enabled_by_default(void) {
    hu_config_t *cfg = test_config_create("{}");
    HU_ASSERT_TRUE(cfg->reaction_collection.enabled);
    test_config_destroy(cfg);
}

/* AC-4: Explicit reaction_collection.enabled = false is preserved. */
static void explicit_reaction_collection_false_preserved(void) {
    const char *json = "{\"reaction_collection\":{\"enabled\":false}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_FALSE(cfg->reaction_collection.enabled);
    test_config_destroy(cfg);
}

/* AC-4: Explicit reaction_collection.enabled = true is preserved. */
static void explicit_reaction_collection_true_preserved(void) {
    const char *json = "{\"reaction_collection\":{\"enabled\":true}}";
    hu_config_t *cfg = test_config_create(json);
    HU_ASSERT_TRUE(cfg->reaction_collection.enabled);
    test_config_destroy(cfg);
}

void run_config_seth_voice_defaults_tests(void) {
    HU_TEST_SUITE("AC-1: mlx_local_routing tri-state");
    HU_RUN_TEST(fresh_config_routing_defaults_to_auto);
    HU_RUN_TEST(mlx_local_routing_off_parses);
    HU_RUN_TEST(mlx_local_routing_auto_parses);
    HU_RUN_TEST(mlx_local_routing_force_parses);
    HU_RUN_TEST(legacy_mlx_local_enabled_true_maps_to_force);
    HU_RUN_TEST(legacy_mlx_local_enabled_false_maps_to_auto);
    HU_RUN_TEST(mlx_local_routing_takes_precedence);
    HU_RUN_TEST(unknown_mlx_local_routing_defaults_to_auto);

    HU_TEST_SUITE("AC-4: reaction_collection enabled by default");
    HU_RUN_TEST(fresh_config_reaction_collection_enabled_by_default);
    HU_RUN_TEST(explicit_reaction_collection_false_preserved);
    HU_RUN_TEST(explicit_reaction_collection_true_preserved);
}
