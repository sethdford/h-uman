/* tests/test_local_only_config.c
 *
 * privacy.local_only — parse and resolution against real configs:
 *   - absent key: ON exactly when the primary provider's endpoint is local
 *     (the owner's mlx_local on 127.0.0.1 resolves ON; a cloud-primary
 *     install resolves OFF and is never bricked by a default it never chose);
 *   - explicit true / false win over that default;
 *   - HU_LOCAL_ONLY=0|1|audit wins over config;
 *   - hu_config_apply_local_only sets the process mode the backstop reads.
 * The primary is found through the composite the daemon actually uses
 * ("reliable" -> reliability.primary_provider).
 */

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/local_only_guard.h"
#include "human/providers/local_only_config.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#define PROD_SHAPE                                                                             \
    "\"default_provider\":\"reliable\",\"default_model\":\"GLM-4.5-Air-4bit\","                \
    "\"providers\":[{\"name\":\"gemini\",\"base_url\":\"https://aiplatform.googleapis.com/v1/" \
    "projects/p/locations/global/publishers/google/models\"},"                                 \
    "{\"name\":\"mlx_local\",\"base_url\":\"http://127.0.0.1:8741/v1\"}],"                     \
    "\"reliability\":{\"primary_provider\":\"mlx_local\",\"fallback_providers\":[]}"

#define CLOUD_SHAPE                                                                            \
    "\"default_provider\":\"gemini\",\"default_model\":\"gemini-3.1-flash-lite\","             \
    "\"providers\":[{\"name\":\"gemini\",\"base_url\":\"https://aiplatform.googleapis.com/v1/" \
    "projects/p/locations/global/publishers/google/models\"}]"

static hu_config_t *lo_cfg(const char *json) {
    hu_allocator_t backing = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(backing);
    HU_ASSERT_NOT_NULL(arena);
    hu_config_t *cfg = (hu_config_t *)backing.alloc(backing.ctx, sizeof(hu_config_t));
    HU_ASSERT_NOT_NULL(cfg);
    memset(cfg, 0, sizeof(*cfg));
    cfg->arena = arena;
    cfg->allocator = hu_arena_allocator(arena);
    HU_ASSERT_EQ(hu_config_parse_json(cfg, json, strlen(json)), HU_OK);
    return cfg;
}

static void lo_cfg_free(hu_config_t *cfg) {
    hu_allocator_t backing = hu_system_allocator();
    hu_config_deinit(cfg);
    backing.free(backing.ctx, cfg, sizeof(*cfg));
}

static void lo_clean(void) {
    unsetenv("HU_LOCAL_ONLY");
    hu_local_only_reset();
}

static void parse_absent_key_is_unset(void) {
    hu_config_t *cfg = lo_cfg("{" PROD_SHAPE "}");
    HU_ASSERT_FALSE(cfg->privacy.local_only_set);
    lo_cfg_free(cfg);
}

static void parse_explicit_true_and_false(void) {
    hu_config_t *t = lo_cfg("{\"privacy\":{\"local_only\":true}}");
    HU_ASSERT_TRUE(t->privacy.local_only_set);
    HU_ASSERT_TRUE(t->privacy.local_only);
    lo_cfg_free(t);
    hu_config_t *f = lo_cfg("{\"privacy\":{\"local_only\":false}}");
    HU_ASSERT_TRUE(f->privacy.local_only_set);
    HU_ASSERT_FALSE(f->privacy.local_only);
    lo_cfg_free(f);
}

static void primary_is_found_through_reliable(void) {
    hu_config_t *cfg = lo_cfg("{" PROD_SHAPE "}");
    HU_ASSERT_STR_EQ(hu_config_primary_provider_name(cfg), "mlx_local");
    HU_ASSERT_STR_EQ(hu_config_primary_endpoint(cfg), "http://127.0.0.1:8741/v1");
    HU_ASSERT_TRUE(hu_config_primary_is_local(cfg));
    lo_cfg_free(cfg);
    hu_config_t *cloud = lo_cfg("{" CLOUD_SHAPE "}");
    HU_ASSERT_STR_EQ(hu_config_primary_provider_name(cloud), "gemini");
    HU_ASSERT_FALSE(hu_config_primary_is_local(cloud));
    lo_cfg_free(cloud);
}

static void absent_key_defaults_on_for_local_primary_off_for_cloud(void) {
    lo_clean();
    hu_config_t *local = lo_cfg("{" PROD_SHAPE "}");
    HU_ASSERT_EQ((int)hu_config_local_only_mode(local), (int)HU_GATE_LIVE);
    lo_cfg_free(local);
    hu_config_t *cloud = lo_cfg("{" CLOUD_SHAPE "}");
    HU_ASSERT_EQ((int)hu_config_local_only_mode(cloud), (int)HU_GATE_OFF);
    lo_cfg_free(cloud);
}

static void explicit_value_and_env_override(void) {
    lo_clean();
    hu_config_t *off = lo_cfg("{" PROD_SHAPE ",\"privacy\":{\"local_only\":false}}");
    HU_ASSERT_EQ((int)hu_config_local_only_mode(off), (int)HU_GATE_OFF);
    setenv("HU_LOCAL_ONLY", "1", 1);
    HU_ASSERT_EQ((int)hu_config_local_only_mode(off), (int)HU_GATE_LIVE);
    setenv("HU_LOCAL_ONLY", "audit", 1);
    HU_ASSERT_EQ((int)hu_config_local_only_mode(off), (int)HU_GATE_SHADOW);
    lo_cfg_free(off);
    unsetenv("HU_LOCAL_ONLY");
    hu_config_t *on = lo_cfg("{" CLOUD_SHAPE ",\"privacy\":{\"local_only\":true}}");
    HU_ASSERT_EQ((int)hu_config_local_only_mode(on), (int)HU_GATE_LIVE);
    setenv("HU_LOCAL_ONLY", "0", 1);
    HU_ASSERT_EQ((int)hu_config_local_only_mode(on), (int)HU_GATE_OFF);
    lo_cfg_free(on);
    lo_clean();
}

static void apply_sets_the_process_mode(void) {
    lo_clean();
    HU_ASSERT_FALSE(hu_local_only_enforced());
    hu_config_t *cfg = lo_cfg("{" PROD_SHAPE "}");
    HU_ASSERT_EQ((int)hu_config_apply_local_only(cfg), (int)HU_GATE_LIVE);
    HU_ASSERT_TRUE(hu_local_only_enforced());
    lo_cfg_free(cfg);
    lo_clean();
}

void run_local_only_config_tests(void) {
    HU_TEST_SUITE("local_only_config");
    HU_RUN_TEST(parse_absent_key_is_unset);
    HU_RUN_TEST(parse_explicit_true_and_false);
    HU_RUN_TEST(primary_is_found_through_reliable);
    HU_RUN_TEST(absent_key_defaults_on_for_local_primary_off_for_cloud);
    HU_RUN_TEST(explicit_value_and_env_override);
    HU_RUN_TEST(apply_sets_the_process_mode);
}
