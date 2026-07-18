/* test_somatic.c — pins the somatic interoception state (src/persona/somatic.c).
 *
 * Somatic energy/battery feed prompt text today and will GATE behavior under A4
 * (docs/plans/2026-05-29-interoception-gated-warmth/ — hu_somatic_behavior_gate
 * reads these exact bands). These tests pin the label thresholds, the
 * init/update dynamics, the forced-TIRED rule, and that build_context still
 * emits the energy/battery labels (A4 AC-8 regression guard). Pure +
 * deterministic (now_ts passed in); build_context output is freed (ASan). */
#include "human/core/allocator.h"
#include "human/persona/somatic.h"
#include "test_framework.h"

#include <string.h>

/* ── energy_label bands: >0.7 high, >0.4 moderate, >0.2 low, else depleted ── */
static void somatic_energy_label_bands(void) {
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.80f), "high");
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.50f), "moderate");
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.30f), "low");
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.10f), "depleted");
    /* strict-inequality boundaries: a value AT the threshold falls to the
     * lower band (the gate is `>`), so 0.7 is moderate, 0.4 is low, 0.2 is
     * depleted. Pinning these guards A4's band reads against silent drift. */
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.70f), "moderate");
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.40f), "low");
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.20f), "depleted");
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(0.0f), "depleted");
}

/* ── battery_label bands: >0.7 charged, >0.4 winding down, >0.2 drained, else empty ── */
static void somatic_battery_label_bands(void) {
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.90f), "charged");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.50f), "winding down");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.30f), "drained");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.05f), "empty");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.70f), "winding down");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.40f), "drained");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(0.20f), "empty");
}

/* init starts fully charged: energy/battery/focus = 1.0, arousal = 0. */
static void somatic_init_is_full(void) {
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(s.energy), "high");
    HU_ASSERT_STR_EQ(hu_somatic_battery_label(s.social_battery), "charged");
    HU_ASSERT_TRUE(s.arousal == 0.f);
    HU_ASSERT_EQ((int)s.physical, (int)HU_PHYSICAL_NORMAL);
}

/* update with no elapsed time (last_interaction_ts=0 -> hours_since=0) applies
 * the flat per-turn decay: energy 1.0 -> 0.97; arousal mirrors emotional
 * intensity (clamped); a single update keeps energy in the "high" band. */
static void somatic_update_decays_energy_deterministically(void) {
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    hu_somatic_update(&s, /*now_ts=*/1000, /*emotional_intensity=*/0.5f,
                      /*topic_switches=*/0, HU_PHYSICAL_NORMAL);
    /* 1.0 - 0.03 = 0.97 -> still "high" */
    HU_ASSERT_STR_EQ(hu_somatic_energy_label(s.energy), "high");
    HU_ASSERT_TRUE(s.energy < 1.0f && s.energy > 0.9f);
    /* arousal tracks emotional intensity */
    HU_ASSERT_TRUE(s.arousal > 0.49f && s.arousal < 0.51f);
    HU_ASSERT_TRUE(s.last_interaction_ts == 1000U);
}

/* arousal clamps to [0,1] even when emotional intensity overshoots. */
static void somatic_update_clamps_arousal(void) {
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    hu_somatic_update(&s, 1000, /*emotional_intensity=*/3.0f, 0, HU_PHYSICAL_NORMAL);
    HU_ASSERT_TRUE(s.arousal == 1.0f);
}

/* very low energy forces the physical state to TIRED regardless of the
 * scheduled physical state (the interoception override). */
static void somatic_update_forces_tired_when_depleted(void) {
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    s.energy = 0.1f; /* below the 0.2 floor */
    s.last_interaction_ts = 1000;
    hu_somatic_update(&s, 1000, 0.0f, 0, HU_PHYSICAL_ENERGIZED);
    HU_ASSERT_EQ((int)s.physical, (int)HU_PHYSICAL_TIRED);
}

/* build_context returns OK, emits the energy + battery labels (A4 AC-8 guard),
 * and the caller frees the allocation (ASan checks the free). */
static void somatic_build_context_emits_labels(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_somatic_state_t s;
    hu_somatic_init(&s); /* energy/battery full -> "high"/"charged" */
    char *ctx = NULL;
    size_t len = 0;
    HU_ASSERT_EQ(hu_somatic_build_context(&alloc, &s, &ctx, &len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "SOMATIC STATE") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "high") != NULL);    /* energy label present */
    HU_ASSERT_TRUE(strstr(ctx, "charged") != NULL); /* battery label present */
    alloc.free(alloc.ctx, ctx, len + 1);
}

/* ── persistence (SOTA roadmap #11, presence plan) ─────────────────────────
 * The somatic state previously died with the process — every daemon restart
 * reset energy/social-battery to full, erasing "the day so far". Save/load
 * make the interior continuous across restarts; recovery-while-away already
 * falls out of hu_somatic_update(now_ts) on the next turn. */

static void somatic_save_load_roundtrip(void) {
    const char *path = "/tmp/hu_test_somatic_roundtrip.json";
    remove(path);
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    s.energy = 0.42f;
    s.social_battery = 0.17f;
    s.focus = 0.9f;
    s.arousal = 0.33f;
    s.physical = HU_PHYSICAL_TIRED;
    s.last_interaction_ts = 1784000000ULL;
    s.last_recharge_ts = 1783990000ULL;
    s.conversation_load_accumulated = 3.5f;
    HU_ASSERT_EQ(hu_somatic_save_file(&s, path), HU_OK);

    hu_somatic_state_t r;
    hu_somatic_init(&r); /* defaults differ from s — load must overwrite */
    HU_ASSERT_EQ(hu_somatic_load_file(&r, path), HU_OK);
    HU_ASSERT_TRUE(r.energy > 0.41f && r.energy < 0.43f);
    HU_ASSERT_TRUE(r.social_battery > 0.16f && r.social_battery < 0.18f);
    HU_ASSERT_TRUE(r.focus > 0.89f && r.focus < 0.91f);
    HU_ASSERT_EQ((int)r.physical, (int)HU_PHYSICAL_TIRED);
    HU_ASSERT_EQ(r.last_interaction_ts, 1784000000ULL);
    HU_ASSERT_EQ(r.last_recharge_ts, 1783990000ULL);
    HU_ASSERT_TRUE(r.conversation_load_accumulated > 3.4f &&
                   r.conversation_load_accumulated < 3.6f);
    remove(path);
}

static void somatic_load_missing_file_keeps_state(void) {
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    s.energy = 0.55f;
    HU_ASSERT_TRUE(hu_somatic_load_file(&s, "/tmp/hu_test_somatic_missing.json") != HU_OK);
    /* State untouched on miss. */
    HU_ASSERT_TRUE(s.energy > 0.54f && s.energy < 0.56f);
}

static void somatic_load_corrupt_file_keeps_state(void) {
    const char *path = "/tmp/hu_test_somatic_corrupt.json";
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{not valid json!!", f);
    fclose(f);
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    s.energy = 0.66f;
    HU_ASSERT_TRUE(hu_somatic_load_file(&s, path) != HU_OK);
    HU_ASSERT_TRUE(s.energy > 0.65f && s.energy < 0.67f);
    remove(path);
}

static void somatic_load_clamps_out_of_range(void) {
    /* A hand-edited or corrupted-but-parseable file must not inject
     * out-of-range values into behavior gates. */
    const char *path = "/tmp/hu_test_somatic_clamp.json";
    FILE *f = fopen(path, "w");
    HU_ASSERT_NOT_NULL(f);
    fputs("{\"energy\": 9.5, \"social_battery\": -2.0, \"focus\": 0.5, \"arousal\": 0.5,"
          "\"physical\": 0, \"last_interaction_ts\": 1, \"last_recharge_ts\": 1,"
          "\"conversation_load_accumulated\": 0}",
          f);
    fclose(f);
    hu_somatic_state_t s;
    hu_somatic_init(&s);
    HU_ASSERT_EQ(hu_somatic_load_file(&s, path), HU_OK);
    HU_ASSERT_TRUE(s.energy <= 1.0f);
    HU_ASSERT_TRUE(s.social_battery >= 0.0f);
    remove(path);
}

void run_somatic_tests(void);
void run_somatic_tests(void) {
    HU_TEST_SUITE("somatic");
    HU_RUN_TEST(somatic_energy_label_bands);
    HU_RUN_TEST(somatic_battery_label_bands);
    HU_RUN_TEST(somatic_init_is_full);
    HU_RUN_TEST(somatic_update_decays_energy_deterministically);
    HU_RUN_TEST(somatic_update_clamps_arousal);
    HU_RUN_TEST(somatic_update_forces_tired_when_depleted);
    HU_RUN_TEST(somatic_build_context_emits_labels);
    HU_RUN_TEST(somatic_save_load_roundtrip);
    HU_RUN_TEST(somatic_load_missing_file_keeps_state);
    HU_RUN_TEST(somatic_load_corrupt_file_keeps_state);
    HU_RUN_TEST(somatic_load_clamps_out_of_range);
}
