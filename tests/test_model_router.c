#include "human/agent/model_router.h"
#include "human/core/allocator.h"
#include "human/provider.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static hu_model_router_config_t cfg;

static void default_config_has_all_models(void) {
    cfg = hu_model_router_default_config();
    HU_ASSERT(cfg.reflexive_model != NULL);
    HU_ASSERT(cfg.conversational_model != NULL);
    HU_ASSERT(cfg.analytical_model != NULL);
    HU_ASSERT(cfg.deep_model != NULL);
    HU_ASSERT(cfg.reflexive_model_len > 0);
    HU_ASSERT(cfg.conversational_model_len > 0);
}

static void default_models_are_not_deprecated(void) {
    /* CLAUDE.md "AI Model Versions": gemini-3-flash-preview is superseded by
     * gemini-3.5-flash; gemini-3-pro-preview is discontinued. The router's
     * default config must not hand out a deprecated model id. */
    cfg = hu_model_router_default_config();
    HU_ASSERT_STR_EQ(cfg.conversational_model, "gemini-3.5-flash");
    HU_ASSERT_EQ(cfg.conversational_model_len, strlen(cfg.conversational_model));
    HU_ASSERT_STR_EQ(cfg.analytical_model, "gemini-3.5-flash");
    HU_ASSERT_EQ(cfg.analytical_model_len, strlen(cfg.analytical_model));
    HU_ASSERT(strstr(cfg.conversational_model, "gemini-3-flash-preview") == NULL);
    HU_ASSERT(strstr(cfg.conversational_model, "gemini-3-pro-preview") == NULL);
    HU_ASSERT(strstr(cfg.analytical_model, "gemini-3-flash-preview") == NULL);
    /* deep tier stays on the canonical pro-preview id (not the discontinued one) */
    HU_ASSERT(strstr(cfg.deep_model, "gemini-3-pro-preview") == NULL);
}

static void short_message_routes_reflexive(void) {
    cfg = hu_model_router_default_config();
    hu_model_selection_t sel = hu_model_route(&cfg, "lol", 3, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT(sel.thinking_budget == 0);
    HU_ASSERT(sel.source == HU_ROUTE_HEURISTIC);
}

static void simple_ack_routes_reflexive(void) {
    cfg = hu_model_router_default_config();
    hu_model_selection_t sel = hu_model_route(&cfg, "ok cool", 7, NULL, 0, 10, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
}

static void normal_question_routes_higher(void) {
    cfg = hu_model_router_default_config();
    const char *msg = "hey what are you up to today? want to grab lunch or something";
    hu_model_selection_t sel = hu_model_route(&cfg, msg, strlen(msg), NULL, 0, 12, 2);
    HU_ASSERT(sel.tier >= HU_TIER_CONVERSATIONAL);
    HU_ASSERT(sel.thinking_budget > 0);
}

static void emotional_heavy_routes_deep(void) {
    cfg = hu_model_router_default_config();
    const char *msg =
        "I don't know what to do. Mom passed away last night and I'm terrified of what comes next";
    hu_model_selection_t sel = hu_model_route(&cfg, msg, strlen(msg), "family", 6, 2, 5);
    HU_ASSERT(sel.tier >= HU_TIER_ANALYTICAL);
    HU_ASSERT(sel.thinking_budget >= 4096);
}

static void family_gets_upgraded(void) {
    cfg = hu_model_router_default_config();
    const char *msg = "hey can you help me figure out this insurance thing it's confusing";
    hu_model_selection_t sel_family = hu_model_route(&cfg, msg, strlen(msg), "family", 6, 14, 2);
    hu_model_selection_t sel_regular = hu_model_route(&cfg, msg, strlen(msg), "regular", 7, 14, 2);
    HU_ASSERT(sel_family.tier >= sel_regular.tier);
}

static void advice_question_gets_analytical(void) {
    cfg = hu_model_router_default_config();
    const char *msg =
        "should i take the job offer or stay where i am? what do you think about the trade-offs";
    hu_model_selection_t sel = hu_model_route(&cfg, msg, strlen(msg), "sister", 6, 20, 3);
    HU_ASSERT(sel.tier >= HU_TIER_ANALYTICAL);
    HU_ASSERT(sel.thinking_budget >= 4096);
}

static void late_night_emotional_upgraded(void) {
    cfg = hu_model_router_default_config();
    const char *msg = "i can't sleep, feeling really stressed about everything going on";
    hu_model_selection_t sel_night = hu_model_route(&cfg, msg, strlen(msg), "friend", 6, 2, 1);
    hu_model_selection_t sel_day = hu_model_route(&cfg, msg, strlen(msg), "friend", 6, 14, 1);
    HU_ASSERT(sel_night.tier >= sel_day.tier);
}

static void null_msg_returns_conversational(void) {
    cfg = hu_model_router_default_config();
    hu_model_selection_t sel = hu_model_route(&cfg, NULL, 0, NULL, 0, 12, 0);
    HU_ASSERT(sel.tier == HU_TIER_CONVERSATIONAL);
    HU_ASSERT(sel.model != NULL);
}

static void null_cfg_returns_default(void) {
    hu_model_selection_t sel = hu_model_route(NULL, "hello", 5, NULL, 0, 12, 0);
    HU_ASSERT(sel.model != NULL);
    HU_ASSERT(sel.model_len > 0);
}

static void frustrated_message_not_reflexive(void) {
    cfg = hu_model_router_default_config();
    const char *msg = "ugh this budget is not working at all, the numbers are broken and i'm so "
                      "frustrated with it";
    hu_model_selection_t sel = hu_model_route(&cfg, msg, strlen(msg), "family", 6, 10, 3);
    HU_ASSERT(sel.tier > HU_TIER_REFLEXIVE);
    HU_ASSERT(sel.thinking_budget > 0);
}

static void thinking_budget_scales_with_tier(void) {
    cfg = hu_model_router_default_config();
    hu_model_selection_t reflexive = hu_model_route(&cfg, "ok", 2, NULL, 0, 12, 0);
    hu_model_selection_t deep = hu_model_route(
        &cfg, "I don't know what to do, I'm terrified and need help deciding about the divorce", 80,
        "family", 6, 2, 5);
    HU_ASSERT(deep.thinking_budget > reflexive.thinking_budget);
}

/* ── Route cache tests ────────────────────────────────────────────────── */

static void cache_init_all_empty(void) {
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);
    hu_cognitive_tier_t tier;
    HU_ASSERT(!hu_route_cache_get(&cache, "hello", 5, 1000, &tier));
}

static void cache_put_and_get(void) {
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);
    hu_route_cache_put(&cache, "hello world", 11, 1000, HU_TIER_ANALYTICAL);
    hu_cognitive_tier_t tier;
    HU_ASSERT(hu_route_cache_get(&cache, "hello world", 11, 1001, &tier));
    HU_ASSERT(tier == HU_TIER_ANALYTICAL);
}

static void cache_expires_after_ttl(void) {
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);
    hu_route_cache_put(&cache, "test msg", 8, 1000, HU_TIER_DEEP);
    hu_cognitive_tier_t tier;
    HU_ASSERT(hu_route_cache_get(&cache, "test msg", 8, 1299, &tier));
    HU_ASSERT(!hu_route_cache_get(&cache, "test msg", 8, 1301, &tier));
}

static void cache_different_messages_different_slots(void) {
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);
    hu_route_cache_put(&cache, "msg A", 5, 1000, HU_TIER_REFLEXIVE);
    hu_route_cache_put(&cache, "msg B", 5, 1000, HU_TIER_DEEP);
    hu_cognitive_tier_t tier;
    HU_ASSERT(hu_route_cache_get(&cache, "msg B", 5, 1001, &tier));
    HU_ASSERT(tier == HU_TIER_DEEP);
}

static void cache_null_safety(void) {
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);
    hu_cognitive_tier_t tier;
    HU_ASSERT(!hu_route_cache_get(NULL, "x", 1, 0, &tier));
    HU_ASSERT(!hu_route_cache_get(&cache, NULL, 0, 0, &tier));
    hu_route_cache_put(NULL, "x", 1, 0, HU_TIER_DEEP);
    hu_route_cache_put(&cache, NULL, 0, 0, HU_TIER_DEEP);
}

/* ── Judge response parser tests ──────────────────────────────────────── */

static void parse_judge_reflexive(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"REFLEXIVE\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_REFLEXIVE);
}

static void parse_judge_conversational(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"CONVERSATIONAL\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_CONVERSATIONAL);
}

static void parse_judge_analytical(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"ANALYTICAL\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_ANALYTICAL);
}

static void parse_judge_deep(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"DEEP\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_DEEP);
}

static void parse_judge_edgeclaw_simple_maps_reflexive(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"SIMPLE\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_REFLEXIVE);
}

static void parse_judge_edgeclaw_medium_maps_conversational(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"MEDIUM\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_CONVERSATIONAL);
}

static void parse_judge_edgeclaw_complex_maps_analytical(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"COMPLEX\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_ANALYTICAL);
}

static void parse_judge_edgeclaw_reasoning_maps_deep(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"REASONING\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_DEEP);
}

static void parse_judge_with_whitespace(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "  { \"tier\" : \"ANALYTICAL\" }  ";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_ANALYTICAL);
}

static void parse_judge_invalid_returns_false(void) {
    hu_cognitive_tier_t tier;
    HU_ASSERT(!hu_route_parse_judge_response("garbage", 7, &tier));
    HU_ASSERT(!hu_route_parse_judge_response("{\"tier\":\"UNKNOWN\"}", 18, &tier));
    HU_ASSERT(!hu_route_parse_judge_response(NULL, 0, &tier));
    HU_ASSERT(!hu_route_parse_judge_response("{}", 2, &tier));
}

static void parse_judge_case_insensitive(void) {
    hu_cognitive_tier_t tier;
    const char *resp = "{\"tier\":\"deep\"}";
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_DEEP);
}

/* ── Prompt hash tests ────────────────────────────────────────────────── */

static void hash_deterministic(void) {
    uint64_t h1 = hu_route_hash_prompt("hello world", 11);
    uint64_t h2 = hu_route_hash_prompt("hello world", 11);
    HU_ASSERT(h1 == h2);
}

static void hash_different_inputs_differ(void) {
    uint64_t h1 = hu_route_hash_prompt("hello", 5);
    uint64_t h2 = hu_route_hash_prompt("world", 5);
    HU_ASSERT(h1 != h2);
}

/* ── Decision log tests ───────────────────────────────────────────────── */

static void log_init_empty(void) {
    hu_route_decision_log_t log;
    hu_route_log_init(&log);
    HU_ASSERT(hu_route_log_count(&log) == 0);
    HU_ASSERT(hu_route_log_get(&log, 0) == NULL);
}

static void log_record_and_get(void) {
    hu_route_decision_log_t log;
    hu_route_log_init(&log);
    hu_model_selection_t sel;
    memset(&sel, 0, sizeof(sel));
    sel.tier = HU_TIER_ANALYTICAL;
    sel.source = HU_ROUTE_JUDGE;
    sel.model = "gemini-3.1-pro-preview";
    sel.model_len = 22;
    hu_route_log_record(&log, &sel, 5, 1000);
    HU_ASSERT(hu_route_log_count(&log) == 1);
    const hu_route_decision_t *d = hu_route_log_get(&log, 0);
    HU_ASSERT(d != NULL);
    HU_ASSERT(d->tier == HU_TIER_ANALYTICAL);
    HU_ASSERT(d->source == HU_ROUTE_JUDGE);
    HU_ASSERT(d->heuristic_score == 5);
    HU_ASSERT(strcmp(d->model, "gemini-3.1-pro-preview") == 0);
}

static void log_wraps_around(void) {
    hu_route_decision_log_t log;
    hu_route_log_init(&log);
    hu_model_selection_t sel;
    memset(&sel, 0, sizeof(sel));
    sel.model = "m";
    sel.model_len = 1;
    for (int i = 0; i < HU_ROUTE_LOG_SIZE + 10; i++) {
        sel.tier = (hu_cognitive_tier_t)(i % 4);
        hu_route_log_record(&log, &sel, i, (int64_t)i);
    }
    HU_ASSERT(hu_route_log_count(&log) == HU_ROUTE_LOG_SIZE);
}

static void log_tier_counts_correct(void) {
    hu_route_decision_log_t log;
    hu_route_log_init(&log);
    hu_model_selection_t sel;
    memset(&sel, 0, sizeof(sel));
    sel.model = "m";
    sel.model_len = 1;

    sel.tier = HU_TIER_REFLEXIVE;
    hu_route_log_record(&log, &sel, 0, 1);
    hu_route_log_record(&log, &sel, 0, 2);
    sel.tier = HU_TIER_DEEP;
    hu_route_log_record(&log, &sel, 0, 3);

    size_t counts[4];
    hu_route_log_tier_counts(&log, counts);
    HU_ASSERT(counts[HU_TIER_REFLEXIVE] == 2);
    HU_ASSERT(counts[HU_TIER_CONVERSATIONAL] == 0);
    HU_ASSERT(counts[HU_TIER_DEEP] == 1);
}

/* ── String conversion tests ──────────────────────────────────────────── */

static void tier_str_all_values(void) {
    HU_ASSERT(strcmp(hu_cognitive_tier_str(HU_TIER_REFLEXIVE), "reflexive") == 0);
    HU_ASSERT(strcmp(hu_cognitive_tier_str(HU_TIER_CONVERSATIONAL), "conversational") == 0);
    HU_ASSERT(strcmp(hu_cognitive_tier_str(HU_TIER_ANALYTICAL), "analytical") == 0);
    HU_ASSERT(strcmp(hu_cognitive_tier_str(HU_TIER_DEEP), "deep") == 0);
}

static void source_str_all_values(void) {
    HU_ASSERT(strcmp(hu_route_source_str(HU_ROUTE_HEURISTIC), "heuristic") == 0);
    HU_ASSERT(strcmp(hu_route_source_str(HU_ROUTE_JUDGE), "judge") == 0);
    HU_ASSERT(strcmp(hu_route_source_str(HU_ROUTE_JUDGE_CACHED), "judge_cached") == 0);
    HU_ASSERT(strcmp(hu_route_source_str(HU_ROUTE_JUDGE_FALLBACK), "judge_fallback") == 0);
}

static void judge_system_prompt_not_null(void) {
    const char *prompt = hu_route_judge_system_prompt();
    HU_ASSERT(prompt != NULL);
    HU_ASSERT(strlen(prompt) > 100);
}

static void route_global_log_reset_yields_zero_count(void) {
    /* Sprint 38 — contract test: any suite test touching the global log
     * must reset via hu_route_log_init before asserting on counts. */
    hu_route_decision_log_t *log = hu_route_global_log();
    hu_route_log_init(log);
    HU_ASSERT_EQ(hu_route_log_count(log), (size_t)0);
    hu_model_router_config_t c = hu_model_router_default_config();
    hu_model_route(&c, "ping", 4, NULL, 0, 12, 0);
    HU_ASSERT_EQ(hu_route_log_count(log), (size_t)1);
    hu_route_log_init(log);
    HU_ASSERT_EQ(hu_route_log_count(log), (size_t)0);
}

static void route_populates_global_log(void) {
    /* Sprint 37 — robustness fix: the global log is a 100-entry ring
     * shared across the whole test suite. As more agent_turn-driven
     * tests are added (e.g. response_guard retry integration tests),
     * the log can saturate at count = 100 by the time this test runs,
     * causing `count > before` to spuriously fail. Reset the log here
     * so the assertion is about *this* call, not cumulative state. */
    hu_route_decision_log_t *log = hu_route_global_log();
    hu_route_log_init(log);
    size_t before = hu_route_log_count(log);
    HU_ASSERT_EQ(before, (size_t)0);

    hu_model_router_config_t c = hu_model_router_default_config();
    hu_model_route(&c, "hello there", 11, NULL, 0, 12, 0);

    HU_ASSERT(hu_route_log_count(log) == 1);
    const hu_route_decision_t *d = hu_route_log_get(log, hu_route_log_count(log) - 1);
    HU_ASSERT(d != NULL);
    HU_ASSERT(d->source == HU_ROUTE_HEURISTIC);
}

static void route_with_judge_null_provider_falls_through(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    hu_model_selection_t sel =
        hu_model_route_with_judge(&c, "hello", 5, NULL, 0, 12, 0, NULL, NULL, 0, NULL, NULL);
    HU_ASSERT(sel.source == HU_ROUTE_HEURISTIC);
}

static void route_with_judge_test_mode_returns_fallback(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    hu_provider_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    hu_allocator_t alloc = hu_system_allocator();
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);

    hu_model_selection_t sel =
        hu_model_route_with_judge(&c, "explain quantum computing", 25, NULL, 0, 12, 0, &dummy,
                                  "test-model", 10, &alloc, &cache);
    HU_ASSERT(sel.source == HU_ROUTE_JUDGE_FALLBACK);
    HU_ASSERT(sel.model != NULL);
    HU_ASSERT(sel.model_len > 0);
}

static void route_with_judge_cache_hit_returns_cached(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    hu_provider_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    hu_allocator_t alloc = hu_system_allocator();
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);

    int64_t now = (int64_t)time(NULL);
    hu_route_cache_put(&cache, "cached message", 14, now, HU_TIER_DEEP);

    hu_model_selection_t sel = hu_model_route_with_judge(&c, "cached message", 14, NULL, 0, 12, 0,
                                                         &dummy, "test-model", 10, &alloc, &cache);
    HU_ASSERT(sel.source == HU_ROUTE_JUDGE_CACHED);
    HU_ASSERT(sel.tier == HU_TIER_DEEP);
}

static void analytical_and_deep_use_different_models(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    HU_ASSERT(strcmp(c.analytical_model, c.deep_model) != 0);
}

static void very_long_message_scores_higher(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    char buf[8192];
    size_t off = 0;
    for (int i = 0; i < 100 && off < sizeof(buf) - 10; i++)
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "word%d ", i);
    hu_model_selection_t sel = hu_model_route(&c, buf, off, NULL, 0, 12, 5);
    HU_ASSERT(sel.tier >= HU_TIER_CONVERSATIONAL);
}

static void parse_judge_bare_tier_name(void) {
    hu_cognitive_tier_t tier;
    HU_ASSERT(hu_route_parse_judge_response("ANALYTICAL", 10, &tier));
    HU_ASSERT(tier == HU_TIER_ANALYTICAL);
}

static void parse_judge_multiline_json(void) {
    const char *resp = "{\n  \"tier\" :\n    \"DEEP\"\n}";
    hu_cognitive_tier_t tier;
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_DEEP);
}

static void parse_judge_markdown_wrapped(void) {
    const char *resp = "```json\n{\"tier\": \"conversational\"}\n```";
    hu_cognitive_tier_t tier;
    HU_ASSERT(hu_route_parse_judge_response(resp, strlen(resp), &tier));
    HU_ASSERT(tier == HU_TIER_CONVERSATIONAL);
}

/* ── On-device routing ──────────────────────────────────────────────── */

static void default_config_has_on_device_fields(void) {
    cfg = hu_model_router_default_config();
    HU_ASSERT_NOT_NULL(cfg.on_device_model);
    HU_ASSERT(cfg.on_device_model_len > 0);
    HU_ASSERT_FALSE(cfg.on_device_available);
}

static void on_device_suitable_reflexive_only(void) {
    HU_ASSERT_TRUE(hu_model_router_on_device_suitable(HU_TIER_REFLEXIVE));
    HU_ASSERT_FALSE(hu_model_router_on_device_suitable(HU_TIER_CONVERSATIONAL));
    HU_ASSERT_FALSE(hu_model_router_on_device_suitable(HU_TIER_ANALYTICAL));
    HU_ASSERT_FALSE(hu_model_router_on_device_suitable(HU_TIER_DEEP));
}

static void reflexive_uses_on_device_when_available(void) {
    cfg = hu_model_router_default_config();
    cfg.on_device_available = true;
    hu_model_selection_t sel = hu_model_route(&cfg, "ok", 2, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT_STR_EQ(sel.model, cfg.on_device_model);
}

static void reflexive_uses_cloud_when_on_device_unavailable(void) {
    cfg = hu_model_router_default_config();
    cfg.on_device_available = false;
    hu_model_selection_t sel = hu_model_route(&cfg, "ok", 2, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT_STR_EQ(sel.model, cfg.reflexive_model);
}

static void reflexive_uses_custom_on_device_model(void) {
    cfg = hu_model_router_default_config();
    cfg.on_device_available = true;
    cfg.on_device_model = "custom-local-model";
    cfg.on_device_model_len = 18;
    hu_model_selection_t sel = hu_model_route(&cfg, "yep", 3, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT_STR_EQ(sel.model, "custom-local-model");
}

static void conversational_never_uses_on_device(void) {
    cfg = hu_model_router_default_config();
    cfg.on_device_available = true;
    const char *msg = "hey what are you up to today? want to grab lunch or something";
    hu_model_selection_t sel = hu_model_route(&cfg, msg, strlen(msg), NULL, 0, 12, 2);
    HU_ASSERT(sel.tier >= HU_TIER_CONVERSATIONAL);
    HU_ASSERT_STR_EQ(sel.model, cfg.conversational_model);
}

static void judge_cached_reflexive_uses_on_device(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    c.on_device_available = true;
    hu_provider_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    hu_allocator_t alloc = hu_system_allocator();
    hu_route_cache_t cache;
    hu_route_cache_init(&cache);

    int64_t now = (int64_t)time(NULL);
    hu_route_cache_put(&cache, "ok sounds good", 14, now, HU_TIER_REFLEXIVE);

    hu_model_selection_t sel = hu_model_route_with_judge(&c, "ok sounds good", 14, NULL, 0, 12, 0,
                                                         &dummy, "test-model", 10, &alloc, &cache);
    HU_ASSERT(sel.source == HU_ROUTE_JUDGE_CACHED);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT_STR_EQ(sel.model, c.on_device_model);
}

/* ── Seth-voice mlx_local routing (Dermot humanness recovery C2) ──────── */

/* Helper: a config with the Seth-voice LoRA enabled + a sentinel model name
 * distinct from every Gemini default, so equality checks are unambiguous. */
static hu_model_router_config_t mlx_cfg(bool enabled, bool healthy) {
    hu_model_router_config_t c = hu_model_router_default_config();
    c.mlx_local_enabled = enabled;
    c.mlx_local_healthy = healthy;
    c.mlx_local_model = "seth-lora-test";
    c.mlx_local_model_len = 14;
    return c;
}

/* Default config must leave mlx_local opt-in OFF and model unset. */
static void mlx_local_default_config_disabled(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    HU_ASSERT_FALSE(c.mlx_local_enabled);
    HU_ASSERT_FALSE(c.mlx_local_healthy);
    HU_ASSERT(c.mlx_local_model == NULL);
    HU_ASSERT_EQ(c.mlx_local_model_len, (size_t)0);
}

/* AC-6: REFLEXIVE routes to the local LoRA when enabled + healthy. */
static void mlx_local_reflexive_routes_when_enabled_and_healthy(void) {
    hu_model_router_config_t c = mlx_cfg(true, true);
    hu_model_selection_t sel = hu_model_route(&c, "ok", 2, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT_STR_EQ(sel.model, "seth-lora-test");
}

/* AC-5: CONVERSATIONAL routes to the local LoRA when enabled + healthy. */
static void mlx_local_conversational_routes_when_enabled_and_healthy(void) {
    hu_model_router_config_t c = mlx_cfg(true, true);
    const char *msg = "hey what are you up to today? want to grab lunch or something";
    hu_model_selection_t sel = hu_model_route(&c, msg, strlen(msg), NULL, 0, 12, 2);
    HU_ASSERT(sel.tier == HU_TIER_CONVERSATIONAL);
    HU_ASSERT_STR_EQ(sel.model, "seth-lora-test");
}

/* AC-7: ANALYTICAL / DEEP stay on the cloud model even when local is healthy.
 * The invariant is "never the local model" — those tiers must reach a cloud
 * Gemini model regardless of mlx_local config. */
static void mlx_local_analytical_deep_stay_cloud(void) {
    hu_model_router_config_t c = mlx_cfg(true, true);
    const char *msg = "I've been agonizing over whether to leave my career because I'm "
                      "terrified what it means for my family's future, and I genuinely "
                      "cannot work out how to weigh the risk against the reward here";
    hu_model_selection_t sel = hu_model_route(&c, msg, strlen(msg), "family", 6, 2, 8);
    HU_ASSERT(sel.tier >= HU_TIER_ANALYTICAL);
    HU_ASSERT(strcmp(sel.model, "seth-lora-test") != 0);
    /* It must be one of the configured cloud tiers. */
    HU_ASSERT(strcmp(sel.model, c.analytical_model) == 0 || strcmp(sel.model, c.deep_model) == 0);
}

/* AC-8: enabled but probe UNHEALTHY → fall back to the cloud model, no local. */
static void mlx_local_unhealthy_falls_back_to_cloud(void) {
    hu_model_router_config_t c = mlx_cfg(true, false);
    hu_model_selection_t sel = hu_model_route(&c, "ok", 2, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT_STR_EQ(sel.model, c.reflexive_model);
    HU_ASSERT(strcmp(sel.model, "seth-lora-test") != 0);
}

/* Disabled (even if a stale healthy=true lingers) → always cloud. */
static void mlx_local_disabled_uses_cloud(void) {
    hu_model_router_config_t c = mlx_cfg(false, true);
    hu_model_selection_t sel = hu_model_route(&c, "ok", 2, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
    HU_ASSERT_STR_EQ(sel.model, c.reflexive_model);
}

static void conversation_floor_prevents_reflexive(void) {
    cfg = hu_model_router_default_config();
    cfg.conversation_floor = HU_TIER_CONVERSATIONAL;
    hu_model_selection_t sel = hu_model_route(&cfg, "hey", 3, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier >= HU_TIER_CONVERSATIONAL);
    HU_ASSERT_STR_EQ(sel.model, cfg.conversational_model);
}

static void conversation_floor_zero_allows_reflexive(void) {
    cfg = hu_model_router_default_config();
    cfg.conversation_floor = HU_TIER_REFLEXIVE;
    hu_model_selection_t sel = hu_model_route(&cfg, "lol", 3, NULL, 0, 14, 0);
    HU_ASSERT(sel.tier == HU_TIER_REFLEXIVE);
}

static void conversation_floor_does_not_downgrade_higher(void) {
    cfg = hu_model_router_default_config();
    cfg.conversation_floor = HU_TIER_CONVERSATIONAL;
    const char *msg = "I'm really struggling with depression and feel so alone right now";
    hu_model_selection_t sel = hu_model_route(&cfg, msg, strlen(msg), NULL, 0, 2, 0);
    HU_ASSERT(sel.tier >= HU_TIER_CONVERSATIONAL);
}

static void conversation_floor_applies_to_judge_fallback(void) {
    hu_model_router_config_t c = hu_model_router_default_config();
    c.conversation_floor = HU_TIER_CONVERSATIONAL;
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t dummy = {.vtable = NULL, .ctx = NULL};
    hu_model_selection_t sel =
        hu_model_route_with_judge(&c, "ok", 2, NULL, 0, 10, 0, &dummy, "test", 4, &alloc, NULL);
    HU_ASSERT(sel.tier >= HU_TIER_CONVERSATIONAL);
}

void run_model_router_tests(void) {
    HU_TEST_SUITE("Model Router");

    HU_RUN_TEST(default_config_has_all_models);
    HU_RUN_TEST(default_models_are_not_deprecated);
    HU_RUN_TEST(short_message_routes_reflexive);
    HU_RUN_TEST(simple_ack_routes_reflexive);
    HU_RUN_TEST(normal_question_routes_higher);
    HU_RUN_TEST(emotional_heavy_routes_deep);
    HU_RUN_TEST(family_gets_upgraded);
    HU_RUN_TEST(advice_question_gets_analytical);
    HU_RUN_TEST(late_night_emotional_upgraded);
    HU_RUN_TEST(null_msg_returns_conversational);
    HU_RUN_TEST(null_cfg_returns_default);
    HU_RUN_TEST(frustrated_message_not_reflexive);
    HU_RUN_TEST(thinking_budget_scales_with_tier);

    /* Route cache */
    HU_RUN_TEST(cache_init_all_empty);
    HU_RUN_TEST(cache_put_and_get);
    HU_RUN_TEST(cache_expires_after_ttl);
    HU_RUN_TEST(cache_different_messages_different_slots);
    HU_RUN_TEST(cache_null_safety);

    /* Judge response parser */
    HU_RUN_TEST(parse_judge_reflexive);
    HU_RUN_TEST(parse_judge_conversational);
    HU_RUN_TEST(parse_judge_analytical);
    HU_RUN_TEST(parse_judge_deep);
    HU_RUN_TEST(parse_judge_edgeclaw_simple_maps_reflexive);
    HU_RUN_TEST(parse_judge_edgeclaw_medium_maps_conversational);
    HU_RUN_TEST(parse_judge_edgeclaw_complex_maps_analytical);
    HU_RUN_TEST(parse_judge_edgeclaw_reasoning_maps_deep);
    HU_RUN_TEST(parse_judge_with_whitespace);
    HU_RUN_TEST(parse_judge_invalid_returns_false);
    HU_RUN_TEST(parse_judge_case_insensitive);
    HU_RUN_TEST(parse_judge_bare_tier_name);
    HU_RUN_TEST(parse_judge_multiline_json);
    HU_RUN_TEST(parse_judge_markdown_wrapped);

    /* Prompt hash */
    HU_RUN_TEST(hash_deterministic);
    HU_RUN_TEST(hash_different_inputs_differ);

    /* Decision log */
    HU_RUN_TEST(log_init_empty);
    HU_RUN_TEST(log_record_and_get);
    HU_RUN_TEST(log_wraps_around);
    HU_RUN_TEST(log_tier_counts_correct);

    /* String conversions */
    HU_RUN_TEST(tier_str_all_values);
    HU_RUN_TEST(source_str_all_values);
    HU_RUN_TEST(judge_system_prompt_not_null);

    /* Global log integration */
    HU_RUN_TEST(route_global_log_reset_yields_zero_count);
    HU_RUN_TEST(route_populates_global_log);

    /* Judge routing */
    HU_RUN_TEST(route_with_judge_null_provider_falls_through);
    HU_RUN_TEST(route_with_judge_test_mode_returns_fallback);
    HU_RUN_TEST(route_with_judge_cache_hit_returns_cached);

    /* SOTA gap fixes */
    HU_RUN_TEST(analytical_and_deep_use_different_models);
    HU_RUN_TEST(very_long_message_scores_higher);

    /* On-device routing */
    HU_RUN_TEST(default_config_has_on_device_fields);
    HU_RUN_TEST(on_device_suitable_reflexive_only);
    HU_RUN_TEST(reflexive_uses_on_device_when_available);
    HU_RUN_TEST(reflexive_uses_cloud_when_on_device_unavailable);
    HU_RUN_TEST(reflexive_uses_custom_on_device_model);
    HU_RUN_TEST(conversational_never_uses_on_device);
    HU_RUN_TEST(judge_cached_reflexive_uses_on_device);

    /* Seth-voice mlx_local routing (Dermot C2) */
    HU_RUN_TEST(mlx_local_default_config_disabled);
    HU_RUN_TEST(mlx_local_reflexive_routes_when_enabled_and_healthy);
    HU_RUN_TEST(mlx_local_conversational_routes_when_enabled_and_healthy);
    HU_RUN_TEST(mlx_local_analytical_deep_stay_cloud);
    HU_RUN_TEST(mlx_local_unhealthy_falls_back_to_cloud);
    HU_RUN_TEST(mlx_local_disabled_uses_cloud);

    /* Conversation floor */
    HU_RUN_TEST(conversation_floor_prevents_reflexive);
    HU_RUN_TEST(conversation_floor_zero_allows_reflexive);
    HU_RUN_TEST(conversation_floor_does_not_downgrade_higher);
    HU_RUN_TEST(conversation_floor_applies_to_judge_fallback);
}
