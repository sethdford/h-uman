#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include "test_framework.h"
#include <string.h>

static void test_persona_types_exist(void) {
    sc_persona_t p;
    sc_persona_overlay_t ov;
    sc_persona_example_t ex;
    sc_persona_example_bank_t bank;

    memset(&p, 0, sizeof(p));
    memset(&ov, 0, sizeof(ov));
    memset(&ex, 0, sizeof(ex));
    memset(&bank, 0, sizeof(bank));

    SC_ASSERT_NULL(p.name);
    SC_ASSERT_NULL(p.identity);
    SC_ASSERT_NULL(p.overlays);
    SC_ASSERT_EQ(p.overlays_count, 0);
    SC_ASSERT_NULL(ov.channel);
    SC_ASSERT_NULL(ex.context);
    SC_ASSERT_NULL(bank.examples);
}

static void test_persona_find_overlay_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    persona.overlays = (sc_persona_overlay_t *)alloc.alloc(alloc.ctx, sizeof(sc_persona_overlay_t));
    SC_ASSERT_NOT_NULL(persona.overlays);
    persona.overlays_count = 1;

    persona.overlays[0].channel = sc_strndup(&alloc, "telegram", 8);
    persona.overlays[0].formality = NULL;
    persona.overlays[0].avg_length = NULL;
    persona.overlays[0].emoji_usage = NULL;
    persona.overlays[0].style_notes = NULL;
    persona.overlays[0].style_notes_count = 0;

    const sc_persona_overlay_t *found = sc_persona_find_overlay(&persona, "telegram", 8);
    SC_ASSERT_NOT_NULL(found);
    SC_ASSERT_STR_EQ(found->channel, "telegram");

    sc_persona_deinit(&alloc, &persona);
}

static void test_persona_find_overlay_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    persona.overlays = (sc_persona_overlay_t *)alloc.alloc(alloc.ctx, sizeof(sc_persona_overlay_t));
    SC_ASSERT_NOT_NULL(persona.overlays);
    persona.overlays_count = 1;
    persona.overlays[0].channel = sc_strndup(&alloc, "telegram", 8);
    persona.overlays[0].formality = NULL;
    persona.overlays[0].avg_length = NULL;
    persona.overlays[0].emoji_usage = NULL;
    persona.overlays[0].style_notes = NULL;
    persona.overlays[0].style_notes_count = 0;

    const sc_persona_overlay_t *found = sc_persona_find_overlay(&persona, "discord", 7);
    SC_ASSERT_NULL(found);

    found = sc_persona_find_overlay(&persona, "tel", 3);
    SC_ASSERT_NULL(found);

    sc_persona_deinit(&alloc, &persona);
}

static void test_persona_deinit_null_safe(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    sc_persona_deinit(&alloc, &persona);

    SC_ASSERT_NULL(persona.name);
    SC_ASSERT_EQ(persona.overlays_count, 0);
}

static void test_persona_load_json_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{"
                       "  \"version\": 1,"
                       "  \"name\": \"test-user\","
                       "  \"core\": {"
                       "    \"identity\": \"A test persona\","
                       "    \"traits\": [\"direct\", \"curious\"],"
                       "    \"vocabulary\": {"
                       "      \"preferred\": [\"solid\", \"cool\"],"
                       "      \"avoided\": [\"synergy\"],"
                       "      \"slang\": [\"ngl\"]"
                       "    },"
                       "    \"communication_rules\": [\"Keep it short\"],"
                       "    \"values\": [\"honesty\"],"
                       "    \"decision_style\": \"Decides fast\""
                       "  },"
                       "  \"channel_overlays\": {"
                       "    \"imessage\": {"
                       "      \"formality\": \"casual\","
                       "      \"avg_length\": \"short\","
                       "      \"emoji_usage\": \"minimal\","
                       "      \"style_notes\": [\"drops punctuation\"]"
                       "    }"
                       "  }"
                       "}";

    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(p.name, "test-user");
    SC_ASSERT_EQ(p.traits_count, 2);
    SC_ASSERT_STR_EQ(p.traits[0], "direct");
    SC_ASSERT_STR_EQ(p.identity, "A test persona");
    SC_ASSERT_EQ(p.preferred_vocab_count, 2);
    SC_ASSERT_EQ(p.avoided_vocab_count, 1);
    SC_ASSERT_EQ(p.slang_count, 1);
    SC_ASSERT_EQ(p.communication_rules_count, 1);
    SC_ASSERT_EQ(p.values_count, 1);
    SC_ASSERT_STR_EQ(p.decision_style, "Decides fast");
    SC_ASSERT_EQ(p.overlays_count, 1);
    SC_ASSERT_STR_EQ(p.overlays[0].channel, "imessage");
    SC_ASSERT_STR_EQ(p.overlays[0].formality, "casual");
    SC_ASSERT_EQ(p.overlays[0].style_notes_count, 1);
    sc_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, "{}", 2, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(p.name);
    SC_ASSERT_NULL(p.identity);
    SC_ASSERT_EQ(p.traits_count, 0);
    SC_ASSERT_EQ(p.overlays_count, 0);
    sc_persona_deinit(&alloc, &p);
}

static void test_persona_load_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load(&alloc, "nonexistent-persona-xyz", 23, &p);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void free_example_bank(sc_allocator_t *a, sc_persona_example_bank_t *bank) {
    if (!bank)
        return;
    if (bank->channel) {
        size_t len = strlen(bank->channel);
        a->free(a->ctx, bank->channel, len + 1);
    }
    if (bank->examples) {
        for (size_t i = 0; i < bank->examples_count; i++) {
            sc_persona_example_t *e = &bank->examples[i];
            if (e->context) {
                size_t len = strlen(e->context);
                a->free(a->ctx, e->context, len + 1);
            }
            if (e->incoming) {
                size_t len = strlen(e->incoming);
                a->free(a->ctx, e->incoming, len + 1);
            }
            if (e->response) {
                size_t len = strlen(e->response);
                a->free(a->ctx, e->response, len + 1);
            }
        }
        a->free(a->ctx, bank->examples, bank->examples_count * sizeof(sc_persona_example_t));
    }
}

static void test_persona_examples_load_json(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"examples\":["
        "  {\"context\":\"casual\",\"incoming\":\"hey\",\"response\":\"yo\"},"
        "  {\"context\":\"work\",\"incoming\":\"meeting?\",\"response\":\"sure 3pm\"}"
        "]}";
    sc_persona_example_bank_t bank = {0};
    sc_error_t err =
        sc_persona_examples_load_json(&alloc, "imessage", 8, json, strlen(json), &bank);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(bank.examples_count, 2);
    SC_ASSERT_STR_EQ(bank.examples[0].context, "casual");
    SC_ASSERT_STR_EQ(bank.examples[1].response, "sure 3pm");
    free_example_bank(&alloc, &bank);
}

static void test_persona_select_examples_match(void) {
    sc_persona_example_t imsg_examples[] = {
        {.context = "casual greeting", .incoming = "hey", .response = "yo"},
        {.context = "making plans", .incoming = "dinner?", .response = "down"},
        {.context = "tech question", .incoming = "what lang?", .response = "C obviously"},
    };
    sc_persona_example_bank_t banks[] = {
        {.channel = "imessage", .examples = imsg_examples, .examples_count = 3},
    };
    sc_persona_t p = {.example_banks = banks, .example_banks_count = 1};

    const sc_persona_example_t *selected[2];
    size_t count = 0;
    sc_error_t err =
        sc_persona_select_examples(&p, "imessage", 8, "plans dinner", 12, selected, &count, 2);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count <= 2);
    SC_ASSERT_TRUE(count > 0);
}

static void test_persona_select_examples_no_channel(void) {
    sc_persona_t p = {0};
    const sc_persona_example_t *selected[2];
    size_t count = 99;
    sc_error_t err = sc_persona_select_examples(&p, "slack", 5, NULL, 0, selected, &count, 2);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0);
}

static void test_persona_select_examples_no_match(void) {
    sc_persona_example_t imsg_examples[] = {
        {.context = "casual greeting", .incoming = "hey", .response = "yo"},
    };
    sc_persona_example_bank_t banks[] = {
        {.channel = "imessage", .examples = imsg_examples, .examples_count = 1},
    };
    sc_persona_t p = {.example_banks = banks, .example_banks_count = 1};

    const sc_persona_example_t *selected[2];
    size_t count = 99;
    sc_error_t err = sc_persona_select_examples(&p, "slack", 5, "greeting", 8, selected, &count, 2);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0);
}

static void test_persona_build_prompt_core(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *traits[] = {"direct", "curious"};
    char *rules[] = {"Keep it short"};
    char *values[] = {"honesty"};
    sc_persona_t p = {
        .name = "testuser",
        .name_len = 8,
        .identity = "A test persona",
        .traits = traits,
        .traits_count = 2,
        .communication_rules = rules,
        .communication_rules_count = 1,
        .values = values,
        .values_count = 1,
        .decision_style = "Decides fast",
    };

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_NOT_NULL(strstr(out, "testuser"));
    SC_ASSERT_NOT_NULL(strstr(out, "direct"));
    SC_ASSERT_NOT_NULL(strstr(out, "Keep it short"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_build_prompt_with_overlay(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *notes[] = {"drops punctuation"};
    sc_persona_overlay_t overlays[] = {
        {.channel = "imessage",
         .formality = "casual",
         .avg_length = "short",
         .emoji_usage = "minimal",
         .style_notes = notes,
         .style_notes_count = 1},
    };
    sc_persona_t p = {
        .name = "testuser",
        .name_len = 8,
        .identity = "A test persona",
        .overlays = overlays,
        .overlays_count = 1,
    };

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "imessage"));
    SC_ASSERT_NOT_NULL(strstr(out, "casual"));
    SC_ASSERT_NOT_NULL(strstr(out, "drops punctuation"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

void run_persona_tests(void) {
    SC_TEST_SUITE("Persona");

    SC_RUN_TEST(test_persona_types_exist);
    SC_RUN_TEST(test_persona_find_overlay_found);
    SC_RUN_TEST(test_persona_find_overlay_not_found);
    SC_RUN_TEST(test_persona_deinit_null_safe);
    SC_RUN_TEST(test_persona_load_json_basic);
    SC_RUN_TEST(test_persona_load_json_empty);
    SC_RUN_TEST(test_persona_load_not_found);
    SC_RUN_TEST(test_persona_build_prompt_core);
    SC_RUN_TEST(test_persona_build_prompt_with_overlay);
    SC_RUN_TEST(test_persona_examples_load_json);
    SC_RUN_TEST(test_persona_select_examples_match);
    SC_RUN_TEST(test_persona_select_examples_no_channel);
    SC_RUN_TEST(test_persona_select_examples_no_match);
}
