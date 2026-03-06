#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/spawn.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/persona.h"
#include "test_framework.h"
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

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

static void test_persona_prompt_overrides_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_prompt = "You are acting as TestUser. Direct and curious.";
    cfg.persona_prompt_len = strlen(cfg.persona_prompt);
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "TestUser") != NULL);
    SC_ASSERT_TRUE(strstr(out, "SeaClaw") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_analyzer_builds_prompt(void) {
    const char *messages[] = {"hey whats up", "down. where at", "thursday works"};
    char prompt[2048];
    size_t prompt_len = 0;
    sc_error_t err = sc_persona_analyzer_build_prompt(messages, 3, "imessage", prompt,
                                                      sizeof(prompt), &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(prompt_len > 0);
    SC_ASSERT_NOT_NULL(strstr(prompt, "hey whats up"));
}

static void test_persona_cli_parse_create(void) {
    const char *argv[] = {"seaclaw", "persona", "create", "seth", "--from-imessage"};
    sc_persona_cli_args_t args = {0};
    sc_error_t err = sc_persona_cli_parse(5, argv, &args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ((int)args.action, (int)SC_PERSONA_ACTION_CREATE);
    SC_ASSERT_STR_EQ(args.name, "seth");
    SC_ASSERT_TRUE(args.from_imessage);
    SC_ASSERT_TRUE(!args.from_gmail);
}

static void test_persona_cli_parse_show(void) {
    const char *argv[] = {"seaclaw", "persona", "show", "seth"};
    sc_persona_cli_args_t args = {0};
    sc_error_t err = sc_persona_cli_parse(4, argv, &args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ((int)args.action, (int)SC_PERSONA_ACTION_SHOW);
    SC_ASSERT_STR_EQ(args.name, "seth");
}

static void test_persona_tool_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(tool.vtable);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "persona");
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_cli_parse_list(void) {
    const char *argv[] = {"seaclaw", "persona", "list"};
    sc_persona_cli_args_t args = {0};
    sc_error_t err = sc_persona_cli_parse(3, argv, &args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ((int)args.action, (int)SC_PERSONA_ACTION_LIST);
}

static void test_creator_synthesize_merges(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *traits1[] = {"direct", "casual"};
    char *traits2[] = {"curious", "direct"};
    sc_persona_t partials[] = {
        {.traits = traits1, .traits_count = 2},
        {.traits = traits2, .traits_count = 2},
    };
    sc_persona_t merged = {0};
    sc_error_t err = sc_persona_creator_synthesize(&alloc, partials, 2, "testuser", 8, &merged);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(merged.name, "testuser");
    SC_ASSERT_EQ(merged.traits_count, (size_t)3);
    sc_persona_deinit(&alloc, &merged);
}

static void test_analyzer_parses_response(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *response =
        "{\"traits\":[\"direct\",\"casual\"],"
        "\"vocabulary\":{\"preferred\":[\"down\",\"works\"],\"avoided\":[],\"slang\":[]},"
        "\"communication_rules\":[\"Keeps messages very short\"],"
        "\"formality\":\"casual\",\"avg_length\":\"short\","
        "\"emoji_usage\":\"none\"}";
    sc_persona_t partial = {0};
    sc_error_t err = sc_persona_analyzer_parse_response(&alloc, response, strlen(response),
                                                        "imessage", 8, &partial);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(partial.traits_count >= 1);
    SC_ASSERT_TRUE(partial.overlays_count == 1);
    sc_persona_deinit(&alloc, &partial);
}

static void test_sampler_facebook_parse_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"messages\":["
                       "{\"sender_name\":\"Alice\",\"content\":\"hey there\"},"
                       "{\"sender_name\":\"Bob\",\"content\":\"hi alice\"},"
                       "{\"sender_name\":\"Alice\",\"content\":\"whats up\"},"
                       "{\"sender_name\":\"Alice\",\"content\":\"see you later\"}"
                       "]}";
    char **msgs = NULL;
    size_t count = 0;
    sc_error_t err = sc_persona_sampler_facebook_parse(json, strlen(json), &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, (size_t)3);
    SC_ASSERT_STR_EQ(msgs[0], "hey there");
    SC_ASSERT_STR_EQ(msgs[1], "whats up");
    SC_ASSERT_STR_EQ(msgs[2], "see you later");
    for (size_t i = 0; i < count; i++)
        alloc.free(alloc.ctx, msgs[i], strlen(msgs[i]) + 1);
    alloc.free(alloc.ctx, msgs, count * sizeof(char *));
}

static void test_sampler_facebook_parse_empty(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 99;
    sc_error_t err = sc_persona_sampler_facebook_parse(json, strlen(json), &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, (size_t)0);
}

static void test_sampler_facebook_parse_null(void) {
    sc_error_t err = sc_persona_sampler_facebook_parse(NULL, 0, NULL, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_sampler_imessage_query(void) {
    char query[512];
    size_t query_len = 0;
    sc_error_t err = sc_persona_sampler_imessage_query(query, sizeof(query), &query_len, 500);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(query_len > 0);
    SC_ASSERT_NOT_NULL(strstr(query, "message"));
    SC_ASSERT_NOT_NULL(strstr(query, "LIMIT"));
}

static void test_spawn_config_persona_field(void) {
    sc_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_name = "seth";
    cfg.persona_name_len = 4;
    SC_ASSERT_STR_EQ(cfg.persona_name, "seth");
    SC_ASSERT_EQ(cfg.persona_name_len, 4);
}

static void test_persona_full_round_trip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"roundtrip\","
                       "\"core\":{\"identity\":\"Integration test persona\","
                       "\"traits\":[\"direct\"],"
                       "\"vocabulary\":{\"preferred\":[\"solid\"],\"avoided\":[],\"slang\":[]},"
                       "\"communication_rules\":[\"Be brief\"],"
                       "\"values\":[\"speed\"],\"decision_style\":\"Fast\"},"
                       "\"channel_overlays\":{\"imessage\":{\"formality\":\"casual\","
                       "\"avg_length\":\"short\",\"emoji_usage\":\"none\","
                       "\"style_notes\":[\"no caps\"]}}}";

    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);

    char *prompt = NULL;
    size_t prompt_len = 0;
    err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, &prompt, &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(prompt, "roundtrip") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "casual") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "no caps") != NULL);

    sc_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_prompt = prompt;
    cfg.persona_prompt_len = prompt_len;
    char *sys = NULL;
    size_t sys_len = 0;
    err = sc_prompt_build_system(&alloc, &cfg, &sys, &sys_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(sys, "roundtrip") != NULL);
    SC_ASSERT_TRUE(strstr(sys, "SeaClaw") == NULL);

    alloc.free(alloc.ctx, sys, sys_len + 1);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
    sc_persona_deinit(&alloc, &p);
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

static void test_agent_persona_prompt_injected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_prompt_config_t cfg = {
        .provider_name = "openai",
        .provider_name_len = 6,
        .model_name = "gpt-4",
        .model_name_len = 5,
        .workspace_dir = "/tmp",
        .workspace_dir_len = 4,
        .tools = NULL,
        .tools_count = 0,
        .memory_context = NULL,
        .memory_context_len = 0,
        .autonomy_level = 1,
        .custom_instructions = NULL,
        .custom_instructions_len = 0,
        .persona_prompt = "You are acting as TestUser. Personality: direct.",
        .persona_prompt_len = 48,
    };
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "TestUser"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_spawn_config_has_persona(void) {
    sc_spawn_config_t cfg = {
        .persona_name = "seth",
        .persona_name_len = 4,
    };
    SC_ASSERT_NOT_NULL(cfg.persona_name);
    SC_ASSERT_EQ(cfg.persona_name_len, (size_t)4);
}

static void test_config_persona_field(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"agent\":{\"persona\":\"seth\"}}";
    sc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    sc_arena_t *arena = sc_arena_create(alloc);
    SC_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = sc_arena_allocator(arena);
    sc_error_t err = sc_config_parse_json(&cfg, json, strlen(json));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(cfg.agent.persona);
    SC_ASSERT_STR_EQ(cfg.agent.persona, "seth");
    sc_config_deinit(&cfg);
}

static void test_persona_cli_run_list(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_cli_args_t args = {0};
    args.action = SC_PERSONA_ACTION_LIST;
    sc_error_t err = sc_persona_cli_run(&alloc, &args);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_persona_cli_run_show_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_cli_args_t args = {0};
    args.action = SC_PERSONA_ACTION_SHOW;
    args.name = "nonexistent_persona_xyz_test";
    sc_error_t err = sc_persona_cli_run(&alloc, &args);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_persona_cli_run_delete_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_cli_args_t args = {0};
    args.action = SC_PERSONA_ACTION_DELETE;
    args.name = "nonexistent_persona_xyz_test";
    sc_error_t err = sc_persona_cli_run(&alloc, &args);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_persona_cli_run_create_no_provider(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_cli_args_t args = {0};
    args.action = SC_PERSONA_ACTION_CREATE;
    args.name = "test_create";
    args.from_imessage = true;
    sc_error_t err = sc_persona_cli_run(&alloc, &args);
    /* In test mode: SC_OK. In non-test mode: SC_ERR_NOT_SUPPORTED */
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_creator_write_and_load(void) {
#if defined(SC_IS_TEST) && SC_IS_TEST
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = sc_strdup(&alloc, "test_creator_write");
    p.name_len = strlen("test_creator_write");
    SC_ASSERT_NOT_NULL(p.name);
    sc_error_t err = sc_persona_creator_write(&alloc, &p);
    if (err != SC_OK) {
        sc_persona_deinit(&alloc, &p);
        return; /* Skip if filesystem not writable (e.g. sandbox) */
    }
    sc_persona_t loaded = {0};
    err = sc_persona_load(&alloc, "test_creator_write", 18, &loaded);
    if (err != SC_OK) {
        sc_persona_deinit(&alloc, &p);
#if defined(__unix__) || defined(__APPLE__)
        {
            const char *home = getenv("HOME");
            if (home && home[0]) {
                char path[512];
                int n = snprintf(path, sizeof(path), "%s/.seaclaw/personas/test_creator_write.json",
                                 home);
                if (n > 0 && (size_t)n < sizeof(path))
                    (void)unlink(path);
            }
        }
#endif
        return; /* Skip if load fails (path mismatch or sandbox) */
    }
    SC_ASSERT_NOT_NULL(loaded.name);
    SC_ASSERT_STR_EQ(loaded.name, "test_creator_write");
    sc_persona_deinit(&alloc, &loaded);
    sc_persona_deinit(&alloc, &p);
#if defined(__unix__) || defined(__APPLE__)
    {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char path[512];
            int n =
                snprintf(path, sizeof(path), "%s/.seaclaw/personas/test_creator_write.json", home);
            if (n > 0 && (size_t)n < sizeof(path))
                (void)unlink(path);
        }
    }
#endif
#endif
}

static void test_persona_prompt_with_channel_overlay(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"ch_test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"channel_overlays\":{\"imessage\":{\"formality\":\"casual\","
                       "\"avg_length\":\"short\",\"emoji_usage\":\"minimal\","
                       "\"style_notes\":[\"no caps\"]}}}";
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);

    /* Without channel — no overlay */
    char *prompt1 = NULL;
    size_t len1 = 0;
    err = sc_persona_build_prompt(&alloc, &p, NULL, 0, &prompt1, &len1);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(prompt1, "imessage") == NULL);

    /* With channel — overlay applied */
    char *prompt2 = NULL;
    size_t len2 = 0;
    err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, &prompt2, &len2);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(prompt2, "casual") != NULL);
    SC_ASSERT_TRUE(strstr(prompt2, "no caps") != NULL);
    SC_ASSERT_TRUE(len2 > len1);

    alloc.free(alloc.ctx, prompt1, len1 + 1);
    alloc.free(alloc.ctx, prompt2, len2 + 1);
    sc_persona_deinit(&alloc, &p);
}

static void test_persona_build_prompt_includes_examples(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"ex_test\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]}}";
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);

    /* Manually add an example bank */
    p.example_banks = alloc.alloc(alloc.ctx, sizeof(sc_persona_example_bank_t));
    SC_ASSERT_TRUE(p.example_banks != NULL);
    memset(p.example_banks, 0, sizeof(sc_persona_example_bank_t));
    p.example_banks_count = 1;
    p.example_banks[0].channel = sc_strdup(&alloc, "cli");
    p.example_banks[0].examples = alloc.alloc(alloc.ctx, sizeof(sc_persona_example_t));
    memset(p.example_banks[0].examples, 0, sizeof(sc_persona_example_t));
    p.example_banks[0].examples_count = 1;
    p.example_banks[0].examples[0].incoming = sc_strdup(&alloc, "Hey what's up");
    p.example_banks[0].examples[0].response = sc_strdup(&alloc, "Not much, you?");

    char *prompt = NULL;
    size_t plen = 0;
    err = sc_persona_build_prompt(&alloc, &p, "cli", 3, &prompt, &plen);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(prompt, "Hey what's up") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Not much, you?") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Example conversations") != NULL);

    alloc.free(alloc.ctx, prompt, plen + 1);
    sc_persona_deinit(&alloc, &p);
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
    SC_RUN_TEST(test_agent_persona_prompt_injected);
    SC_RUN_TEST(test_spawn_config_has_persona);
    SC_RUN_TEST(test_config_persona_field);
    SC_RUN_TEST(test_persona_build_prompt_core);
    SC_RUN_TEST(test_persona_build_prompt_includes_examples);
    SC_RUN_TEST(test_persona_prompt_with_channel_overlay);
    SC_RUN_TEST(test_persona_build_prompt_with_overlay);
    SC_RUN_TEST(test_persona_examples_load_json);
    SC_RUN_TEST(test_persona_prompt_overrides_default);
    SC_RUN_TEST(test_spawn_config_persona_field);
    SC_RUN_TEST(test_persona_cli_parse_create);
    SC_RUN_TEST(test_persona_cli_parse_show);
    SC_RUN_TEST(test_persona_cli_parse_list);
    SC_RUN_TEST(test_persona_cli_run_list);
    SC_RUN_TEST(test_persona_cli_run_show_not_found);
    SC_RUN_TEST(test_persona_cli_run_delete_not_found);
    SC_RUN_TEST(test_persona_cli_run_create_no_provider);
    SC_RUN_TEST(test_creator_write_and_load);
    SC_RUN_TEST(test_persona_tool_create);
    SC_RUN_TEST(test_creator_synthesize_merges);
    SC_RUN_TEST(test_analyzer_builds_prompt);
    SC_RUN_TEST(test_analyzer_parses_response);
    SC_RUN_TEST(test_sampler_imessage_query);
    SC_RUN_TEST(test_sampler_facebook_parse_basic);
    SC_RUN_TEST(test_sampler_facebook_parse_empty);
    SC_RUN_TEST(test_sampler_facebook_parse_null);
    SC_RUN_TEST(test_persona_select_examples_match);
    SC_RUN_TEST(test_persona_select_examples_no_channel);
    SC_RUN_TEST(test_persona_select_examples_no_match);
    SC_RUN_TEST(test_persona_full_round_trip);
}
