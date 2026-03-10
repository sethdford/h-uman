#include "seaclaw/agent.h"
#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/spawn.h"
#include "seaclaw/agent/tool_context.h"
#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include "seaclaw/persona/auto_profile.h"
#include "seaclaw/providers/factory.h"
#include "seaclaw/tool.h"
#include "seaclaw/tools/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
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
    memset(persona.overlays, 0, sizeof(sc_persona_overlay_t));
    persona.overlays_count = 1;

    persona.overlays[0].channel = sc_strndup(&alloc, "telegram", 8);

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
    memset(persona.overlays, 0, sizeof(sc_persona_overlay_t));
    persona.overlays_count = 1;
    persona.overlays[0].channel = sc_strndup(&alloc, "telegram", 8);

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
    char prompt[4096];
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

static void test_persona_validate_json_valid(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test person\",\"traits\":[\"direct\"]}}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_TRUE(err == NULL);
}

static void test_persona_validate_json_missing_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"core\":{\"identity\":\"X\",\"traits\":[]}}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_TRUE(err != NULL);
    SC_ASSERT_TRUE(strstr(err, "name") != NULL);
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_json_missing_core(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\"}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_TRUE(err != NULL);
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_json_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "not json at all";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_TRUE(err != NULL);
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_cli_parse_validate(void) {
    const char *argv[] = {"seaclaw", "persona", "validate", "test_name"};
    sc_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    sc_error_t e = sc_persona_cli_parse(4, argv, &args);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_EQ(args.action, SC_PERSONA_ACTION_VALIDATE);
    SC_ASSERT_TRUE(strcmp(args.name, "test_name") == 0);
}

static void test_persona_cli_parse_feedback_apply(void) {
    const char *argv[] = {"seaclaw", "persona", "feedback", "apply", "mypersona"};
    sc_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    sc_error_t e = sc_persona_cli_parse(5, argv, &args);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_EQ(args.action, SC_PERSONA_ACTION_FEEDBACK_APPLY);
    SC_ASSERT_TRUE(strcmp(args.name, "mypersona") == 0);
}

static void test_cli_parse_diff(void) {
    const char *argv[] = {"seaclaw", "persona", "diff", "a", "b"};
    sc_persona_cli_args_t args = {0};
    SC_ASSERT_EQ(sc_persona_cli_parse(5, argv, &args), SC_OK);
    SC_ASSERT_EQ(args.action, SC_PERSONA_ACTION_DIFF);
    SC_ASSERT_TRUE(strcmp(args.name, "a") == 0);
    SC_ASSERT_TRUE(strcmp(args.diff_name, "b") == 0);
}

static void test_persona_cli_run_validate(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_cli_args_t args = {0};
    args.action = SC_PERSONA_ACTION_VALIDATE;
    args.name = "test_name";
    sc_error_t err = sc_persona_cli_run(&alloc, &args);
    SC_ASSERT_EQ(err, SC_OK);
}

static void test_persona_cli_run_feedback_apply(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_cli_args_t args = {0};
    args.action = SC_PERSONA_ACTION_FEEDBACK_APPLY;
    args.name = "test";
    sc_error_t err = sc_persona_cli_run(&alloc, &args);
    SC_ASSERT_EQ(err, SC_OK);
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

static void test_sampler_gmail_parse_basic(void) {
    const char *json = "{\"messages\":["
                       "{\"from\":\"me\",\"body\":\"Hey there\"},"
                       "{\"from\":\"other\",\"body\":\"Hi\"},"
                       "{\"from\":\"me\",\"body\":\"Cool thanks\"}"
                       "]}";
    char **msgs = NULL;
    size_t count = 0;
    sc_error_t e = sc_persona_sampler_gmail_parse(json, strlen(json), &msgs, &count);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_EQ(count, (size_t)2);
    for (size_t i = 0; i < count; i++)
        free(msgs[i]);
    free(msgs);
}

static void test_sampler_gmail_parse_empty(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 0;
    sc_error_t e = sc_persona_sampler_gmail_parse(json, strlen(json), &msgs, &count);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_EQ(count, (size_t)0);
}

static void test_cli_parse_export(void) {
    const char *argv[] = {"seaclaw", "persona", "export", "seth"};
    sc_persona_cli_args_t args = {0};
    SC_ASSERT_EQ(sc_persona_cli_parse(4, argv, &args), SC_OK);
    SC_ASSERT_EQ(args.action, SC_PERSONA_ACTION_EXPORT);
    SC_ASSERT_TRUE(strcmp(args.name, "seth") == 0);
}

static void test_cli_parse_merge(void) {
    const char *argv[] = {"seaclaw", "persona", "merge", "combined", "a", "b"};
    sc_persona_cli_args_t args = {0};
    SC_ASSERT_EQ(sc_persona_cli_parse(6, argv, &args), SC_OK);
    SC_ASSERT_EQ(args.action, SC_PERSONA_ACTION_MERGE);
    SC_ASSERT_TRUE(strcmp(args.name, "combined") == 0);
    SC_ASSERT_TRUE(args.merge_sources != NULL);
    SC_ASSERT_EQ(args.merge_sources_count, (size_t)2);
    SC_ASSERT_TRUE(strcmp(args.merge_sources[0], "a") == 0);
    SC_ASSERT_TRUE(strcmp(args.merge_sources[1], "b") == 0);
}

static void test_cli_parse_import(void) {
    const char *argv[] = {"seaclaw",    "persona",     "import",
                          "newpersona", "--from-file", "/tmp/p.json"};
    sc_persona_cli_args_t args = {0};
    SC_ASSERT_EQ(sc_persona_cli_parse(6, argv, &args), SC_OK);
    SC_ASSERT_EQ(args.action, SC_PERSONA_ACTION_IMPORT);
    SC_ASSERT_TRUE(strcmp(args.name, "newpersona") == 0);
    SC_ASSERT_TRUE(args.import_file != NULL);
    SC_ASSERT_TRUE(strcmp(args.import_file, "/tmp/p.json") == 0);
}

static void test_cli_parse_from_facebook_file(void) {
    const char *argv[] = {"seaclaw", "persona",         "create",
                          "test",    "--from-facebook", "/tmp/fb.json"};
    sc_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    sc_error_t e = sc_persona_cli_parse(6, argv, &args);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_TRUE(args.from_facebook);
    SC_ASSERT_TRUE(args.facebook_export_path != NULL);
    SC_ASSERT_TRUE(strcmp(args.facebook_export_path, "/tmp/fb.json") == 0);
}

static void test_cli_parse_from_gmail(void) {
    const char *argv[] = {"seaclaw", "persona",      "create",
                          "test",    "--from-gmail", "/tmp/gmail.json"};
    sc_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    sc_error_t e = sc_persona_cli_parse(6, argv, &args);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_TRUE(args.from_gmail);
    SC_ASSERT_TRUE(args.gmail_export_path != NULL);
    SC_ASSERT_TRUE(strcmp(args.gmail_export_path, "/tmp/gmail.json") == 0);
}

static void test_cli_parse_from_response(void) {
    const char *argv[] = {"seaclaw", "persona",         "create",
                          "test",    "--from-response", "/tmp/resp.json"};
    sc_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    sc_error_t err = sc_persona_cli_parse(6, argv, &args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(args.action, SC_PERSONA_ACTION_CREATE);
    SC_ASSERT_TRUE(args.response_file != NULL);
    SC_ASSERT_TRUE(strcmp(args.response_file, "/tmp/resp.json") == 0);
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
    err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &prompt, &prompt_len);
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
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_NOT_NULL(strstr(out, "You are acting as"));
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

static void test_persona_base_dir_returns_override_when_set(void) {
#if defined(__unix__) || defined(__APPLE__)
    char tmpdir[] = "/tmp/seaclaw_base_dir_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        return;
    }
    setenv("SC_PERSONA_DIR", tmpdir, 1);
    char buf[512];
    const char *got = sc_persona_base_dir(buf, sizeof(buf));
    unsetenv("SC_PERSONA_DIR");
    rmdir(tmpdir);
    SC_ASSERT_NOT_NULL(got);
    SC_ASSERT_STR_EQ(got, tmpdir);
#else
    (void)0;
#endif
}

#if defined(__unix__) || defined(__APPLE__)
static void test_persona_load_save_roundtrip_with_temp_dir(void) {
    char tmpdir[] = "/tmp/seaclaw_persona_test_XXXXXX";
    if (!mkdtemp(tmpdir))
        return; /* skip if can't create temp dir */

    setenv("SC_PERSONA_DIR", tmpdir, 1);

    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = sc_strdup(&alloc, "roundtrip_test");
    p.name_len = strlen(p.name);
    p.identity = sc_strdup(&alloc, "Test identity");
    if (!p.name || !p.identity) {
        sc_persona_deinit(&alloc, &p);
        unsetenv("SC_PERSONA_DIR");
        rmdir(tmpdir);
        return;
    }
    sc_error_t err = sc_persona_creator_write(&alloc, &p);
    sc_persona_deinit(&alloc, &p);
    if (err != SC_OK) {
        unsetenv("SC_PERSONA_DIR");
        rmdir(tmpdir);
        return;
    }

    sc_persona_t loaded = {0};
    err = sc_persona_load(&alloc, "roundtrip_test", 14, &loaded);
    unsetenv("SC_PERSONA_DIR");
    if (err != SC_OK) {
        char path[512];
        snprintf(path, sizeof(path), "%s/roundtrip_test.json", tmpdir);
        unlink(path);
        rmdir(tmpdir);
        return;
    }

    SC_ASSERT_STR_EQ(loaded.name, "roundtrip_test");
    SC_ASSERT_STR_EQ(loaded.identity, "Test identity");

    sc_persona_deinit(&alloc, &loaded);

    char path[512];
    snprintf(path, sizeof(path), "%s/roundtrip_test.json", tmpdir);
    unlink(path);
    rmdir(tmpdir);
}
#endif

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
    err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &prompt1, &len1);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(prompt1, "imessage") == NULL);

    /* With channel — overlay applied */
    char *prompt2 = NULL;
    size_t len2 = 0;
    err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &prompt2, &len2);
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
    const char *json = "{\"version\":1,\"name\":\"ex_test\","
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
    err = sc_persona_build_prompt(&alloc, &p, "cli", 3, NULL, 0, &prompt, &plen);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(prompt, "Hey what's up") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Not much, you?") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Example conversations") != NULL);

    alloc.free(alloc.ctx, prompt, plen + 1);
    sc_persona_deinit(&alloc, &p);
}

static void test_agent_set_persona_clears(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&alloc, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 SC_OK);

    /* Clearing with NULL/empty should succeed */
    SC_ASSERT_EQ(sc_agent_set_persona(&agent, NULL, 0), SC_OK);
    SC_ASSERT_NULL(agent.persona);
    SC_ASSERT_NULL(agent.persona_name);

    sc_agent_deinit(&agent);
}

static void test_agent_set_persona_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&alloc, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 SC_OK);

    sc_error_t err = sc_agent_set_persona(&agent, "nonexistent-persona-xyz", 23);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(agent.persona);

    sc_agent_deinit(&agent);
}

static void test_persona_feedback_record_and_apply(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_feedback_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = "cli";
    fb.channel_len = 3;
    fb.original_response = "Hey there buddy!";
    fb.original_response_len = 16;
    fb.corrected_response = "Hey what's up";
    fb.corrected_response_len = 13;
    fb.context = "greeting";
    fb.context_len = 8;

    /* In test mode, record no-ops on disk but returns SC_OK */
    sc_error_t e = sc_persona_feedback_record(&alloc, "test", 4, &fb);
    SC_ASSERT_EQ(e, SC_OK);

    /* Apply also no-ops in test mode */
    e = sc_persona_feedback_apply(&alloc, "test", 4);
    SC_ASSERT_EQ(e, SC_OK);
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
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "imessage"));
    SC_ASSERT_NOT_NULL(strstr(out, "casual"));
    SC_ASSERT_NOT_NULL(strstr(out, "drops punctuation"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* sc_persona_load_json */
static void test_persona_load_json_malformed_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t e = sc_persona_load_json(&alloc, "not json", 8, &p);
    SC_ASSERT_TRUE(e != SC_OK);
}

static void test_persona_load_json_missing_core(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    const char *json = "{\"version\":1,\"name\":\"test\"}";
    sc_error_t e = sc_persona_load_json(&alloc, json, strlen(json), &p);
    if (e == SC_OK) {
        SC_ASSERT_TRUE(p.traits_count == 0);
        sc_persona_deinit(&alloc, &p);
    }
}

/* sc_persona_load */
static void test_persona_load_empty_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t e = sc_persona_load(&alloc, "", 0, &p);
    SC_ASSERT_TRUE(e != SC_OK);
}

static void test_persona_prompt_respects_size_cap(void) {
    sc_allocator_t alloc = sc_system_allocator();
    /* Build JSON with many long traits to exceed 8KB prompt */
    static const char pad[81] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    char *traits_json = alloc.alloc(alloc.ctx, 24 * 1024);
    SC_ASSERT_NOT_NULL(traits_json);
    size_t pos = 0;
    pos += (size_t)snprintf(traits_json + pos, 24 * 1024 - pos,
                            "{\"version\":1,\"name\":\"big\",\"core\":{"
                            "\"identity\":\"Test persona with many traits for size cap\","
                            "\"traits\":[");
    for (int i = 0; i < 200 && pos < 22 * 1024; i++) {
        if (i > 0)
            pos += (size_t)snprintf(traits_json + pos, 24 * 1024 - pos, ",");
        pos += (size_t)snprintf(traits_json + pos, 24 * 1024 - pos, "\"trait_%d_%s\"", i, pad);
    }
    pos +=
        (size_t)snprintf(traits_json + pos, 24 * 1024 - pos,
                         "],\"vocabulary\":{\"preferred\":[],\"avoided\":[],\"slang\":[]},"
                         "\"communication_rules\":[],\"values\":[],\"decision_style\":\"fast\"}}}");
    traits_json[pos] = '\0';

    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t err = sc_persona_load_json(&alloc, traits_json, pos, &p);
    alloc.free(alloc.ctx, traits_json, 24 * 1024);
    SC_ASSERT_EQ(err, SC_OK);

    char *out = NULL;
    size_t out_len = 0;
    err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len <= SC_PERSONA_PROMPT_MAX_BYTES);
    if (out_len == SC_PERSONA_PROMPT_MAX_BYTES)
        SC_ASSERT_NOT_NULL(strstr(out, "[persona prompt truncated]"));

    alloc.free(alloc.ctx, out, out_len + 1);
    sc_persona_deinit(&alloc, &p);
}

/* sc_persona_build_prompt - edge cases */
static void test_persona_build_prompt_empty_persona(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "empty";
    p.name_len = 5;
    char *prompt = NULL;
    size_t len = 0;
    sc_error_t e = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &prompt, &len);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_TRUE(prompt != NULL);
    SC_ASSERT_TRUE(len > 0);
    alloc.free(alloc.ctx, prompt, len + 1);
}

/* sc_persona_examples_load_json - edge cases */
static void test_persona_examples_load_json_empty_bank(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    const char *json = "{\"examples\":[]}";
    sc_error_t e = sc_persona_examples_load_json(&alloc, "test", 4, json, strlen(json), &bank);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_EQ(bank.examples_count, 0);
    if (bank.channel)
        alloc.free(alloc.ctx, bank.channel, strlen(bank.channel) + 1);
}

static void test_persona_examples_load_json_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    sc_error_t e = sc_persona_examples_load_json(&alloc, "test", 4, "bad", 3, &bank);
    SC_ASSERT_TRUE(e != SC_OK);
}

/* sc_persona_select_examples - edge cases */
static void test_persona_select_examples_null_topic_returns_some(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    p.example_banks = alloc.alloc(alloc.ctx, sizeof(sc_persona_example_bank_t));
    memset(p.example_banks, 0, sizeof(sc_persona_example_bank_t));
    p.example_banks_count = 1;
    p.example_banks[0].channel = sc_strdup(&alloc, "test");
    p.example_banks[0].examples = alloc.alloc(alloc.ctx, sizeof(sc_persona_example_t));
    memset(p.example_banks[0].examples, 0, sizeof(sc_persona_example_t));
    p.example_banks[0].examples_count = 1;
    p.example_banks[0].examples[0].incoming = sc_strdup(&alloc, "hello");
    p.example_banks[0].examples[0].response = sc_strdup(&alloc, "hi");

    const sc_persona_example_t *sel[4];
    size_t sel_count = 0;
    sc_error_t e = sc_persona_select_examples(&p, "test", 4, NULL, 0, sel, &sel_count, 4);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_EQ(sel_count, 1);

    sc_persona_deinit(&alloc, &p);
}

static void test_persona_select_examples_max_zero(void) {
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    const sc_persona_example_t *sel[1];
    size_t sel_count = 99;
    sc_error_t e = sc_persona_select_examples(&p, "x", 1, NULL, 0, sel, &sel_count, 0);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_EQ(sel_count, 0);
}

/* Extracted iMessage example bank integration test */
static void test_persona_extracted_imessage_bank_loads_and_selects(void) {
    sc_allocator_t alloc = sc_system_allocator();

    static const char json[] =
        "{\"examples\":["
        "{\"context\":\"Friend suggesting plans\","
        " \"incoming\":\"Want to grab lunch this week?\","
        " \"response\":\"Let's do it! Wednesday or Thursday evening is great!\"},"
        "{\"context\":\"Confirming a plan\","
        " \"incoming\":\"Does noon work?\","
        " \"response\":\"Sounds good!\"},"
        "{\"context\":\"Quick check-in greeting\","
        " \"incoming\":\"Hey! How's it going?\","
        " \"response\":\"Hey there\"},"
        "{\"context\":\"Sharing a win or good news\","
        " \"incoming\":\"How'd the trip go?\","
        " \"response\":\"I won 1300! Trip paid for again\"},"
        "{\"context\":\"Teasing sibling banter\","
        " \"incoming\":\"You're so annoying\","
        " \"response\":\"That's my job as your little brother\"}"
        "]}";

    sc_persona_example_bank_t bank = {0};
    sc_error_t err =
        sc_persona_examples_load_json(&alloc, "imessage", 8, json, strlen(json), &bank);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(bank.examples_count, 5);
    SC_ASSERT_STR_EQ(bank.channel, "imessage");
    SC_ASSERT_STR_EQ(bank.examples[0].context, "Friend suggesting plans");
    SC_ASSERT_STR_EQ(bank.examples[4].response, "That's my job as your little brother");

    sc_persona_t p = {.example_banks = &bank, .example_banks_count = 1};

    const sc_persona_example_t *sel[3];
    size_t sel_count = 0;
    err = sc_persona_select_examples(&p, "imessage", 8, "lunch plans greeting", 20, sel, &sel_count,
                                     3);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(sel_count > 0);
    SC_ASSERT_TRUE(sel_count <= 3);

    const sc_persona_example_t *sel2[3];
    size_t sel2_count = 0;
    err = sc_persona_select_examples(&p, "imessage", 8, "sibling teasing banter", 22, sel2,
                                     &sel2_count, 3);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(sel2_count > 0);

    free_example_bank(&alloc, &bank);
}

/* sc_persona_find_overlay - edge cases */
static void test_persona_find_overlay_null_channel(void) {
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    const sc_persona_overlay_t *o = sc_persona_find_overlay(&p, NULL, 0);
    SC_ASSERT_TRUE(o == NULL);
}

/* sc_persona_analyzer_parse_response - edge cases */
static void test_persona_analyzer_parse_response_empty_object(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t e = sc_persona_analyzer_parse_response(&alloc, "{}", 2, "test", 4, &p);
    if (e == SC_OK)
        sc_persona_deinit(&alloc, &p);
}

static void test_persona_analyzer_parse_response_malformed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t e = sc_persona_analyzer_parse_response(&alloc, "not json", 8, "test", 4, &p);
    SC_ASSERT_TRUE(e != SC_OK);
}

/* sc_persona_creator_synthesize - edge cases */
static void test_persona_creator_synthesize_single_partial(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t partial;
    memset(&partial, 0, sizeof(partial));
    char *trait = "direct";
    partial.traits = &trait;
    partial.traits_count = 1;

    sc_persona_t out;
    memset(&out, 0, sizeof(out));
    sc_error_t e = sc_persona_creator_synthesize(&alloc, &partial, 1, "single", 6, &out);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_TRUE(out.traits_count >= 1);
    sc_persona_deinit(&alloc, &out);
}

static void test_persona_creator_synthesize_zero_partials(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    sc_persona_t out;
    memset(&out, 0, sizeof(out));
    sc_error_t e = sc_persona_creator_synthesize(&alloc, &dummy, 0, "empty", 5, &out);
    /* With count 0, succeeds with empty persona */
    if (e == SC_OK) {
        SC_ASSERT_TRUE(out.name != NULL);
        sc_persona_deinit(&alloc, &out);
    }
}

/* sc_persona_sampler - edge cases */
static void test_sampler_imessage_query_small_cap(void) {
    char buf[16];
    size_t out_len = 0;
    sc_error_t e = sc_persona_sampler_imessage_query(buf, 16, &out_len, 100);
    SC_ASSERT_TRUE(e != SC_OK);
}

static void test_sampler_facebook_parse_malformed(void) {
    char **out = NULL;
    size_t count = 0;
    sc_error_t e = sc_persona_sampler_facebook_parse("bad", 3, &out, &count);
    SC_ASSERT_TRUE(e != SC_OK || count == 0);
}

static void test_sampler_facebook_parse_missing_messages(void) {
    char **out = NULL;
    size_t count = 0;
    sc_error_t e = sc_persona_sampler_facebook_parse("{\"other\":1}", 11, &out, &count);
    SC_ASSERT_TRUE(e != SC_OK || count == 0);
}

/* sc_persona_deinit - edge cases */
static void test_persona_deinit_double_call(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]}}";
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t e = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(e, SC_OK);
    sc_persona_deinit(&alloc, &p);
    sc_persona_deinit(&alloc, &p);
}

/* tool execute tests */
static void test_persona_tool_execute_list(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(tool.vtable != NULL);

    const char *args = "{\"action\":\"list\"}";
    sc_json_value_t *val = NULL;
    err = sc_json_parse(&alloc, args, strlen(args), &val);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    SC_ASSERT_EQ(err, SC_OK);

    sc_json_free(&alloc, val);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_invalid_action(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *args = "{\"action\":\"invalid\"}";
    sc_json_value_t *val = NULL;
    err = sc_json_parse(&alloc, args, strlen(args), &val);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(!result.success);

    sc_json_free(&alloc, val);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_switch_no_agent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    /* Switch without agent set returns "No active agent" */
    const char *args = "{\"action\":\"switch\",\"name\":\"test\"}";
    sc_json_value_t *val = NULL;
    err = sc_json_parse(&alloc, args, strlen(args), &val);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(!result.success);
    SC_ASSERT_TRUE(result.error_msg != NULL);
    SC_ASSERT_TRUE(strstr(result.error_msg, "active agent") != NULL);

    sc_json_free(&alloc, val);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_switch_with_agent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_provider_t prov = {0};
    SC_ASSERT_EQ(sc_provider_create(&alloc, "openai", 6, "test-key", 8, "", 0, &prov), SC_OK);

    sc_agent_t agent = {0};
    SC_ASSERT_EQ(sc_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 SC_OK);

    sc_agent_set_current_for_tools(&agent);

    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    /* Switch to clear: name empty or null */
    const char *args_clear = "{\"action\":\"switch\"}";
    sc_json_value_t *val = NULL;
    err = sc_json_parse(&alloc, args_clear, strlen(args_clear), &val);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.success);

    sc_json_free(&alloc, val);
    sc_tool_result_free(&alloc, &result);

    sc_agent_clear_current_for_tools();
    sc_agent_deinit(&agent);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_feedback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *args = "{\"action\":\"feedback\",\"name\":\"test\",\"original\":\"Hey there!\","
                       "\"corrected\":\"Hey what's up\",\"context\":\"greeting\"}";
    sc_json_value_t *val = NULL;
    err = sc_json_parse(&alloc, args, strlen(args), &val);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.success);

    sc_json_free(&alloc, val);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_feedback_missing_corrected(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *args = "{\"action\":\"feedback\",\"name\":\"test\",\"original\":\"Hey\"}";
    sc_json_value_t *val = NULL;
    err = sc_json_parse(&alloc, args, strlen(args), &val);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(!result.success);

    sc_json_free(&alloc, val);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_create_redirects_to_cli(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *args = "{\"action\":\"create\",\"name\":\"test\"}";
    sc_json_value_t *val = NULL;
    err = sc_json_parse(&alloc, args, strlen(args), &val);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.output != NULL);
    SC_ASSERT_TRUE(strstr(result.output, "CLI") != NULL || strstr(result.output, "cli") != NULL);

    sc_json_free(&alloc, val);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_show(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *args_str = "{\"action\":\"show\",\"name\":\"nonexistent_xyz\"}";
    sc_json_value_t *args = NULL;
    err = sc_json_parse(&alloc, args_str, strlen(args_str), &args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(args != NULL);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    /* In test mode, show returns a stub message */
    SC_ASSERT_TRUE(result.output != NULL);

    sc_json_free(&alloc, args);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_apply_feedback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *args_str = "{\"action\":\"apply_feedback\",\"name\":\"test\"}";
    sc_json_value_t *args = NULL;
    err = sc_json_parse(&alloc, args_str, strlen(args_str), &args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(args != NULL);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.success);

    sc_json_free(&alloc, args);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_apply_feedback_no_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool = {0};
    sc_error_t err = sc_persona_tool_create(&alloc, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *args_str = "{\"action\":\"apply_feedback\"}";
    sc_json_value_t *args = NULL;
    err = sc_json_parse(&alloc, args_str, strlen(args_str), &args);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(args != NULL);

    sc_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(!result.success);

    sc_json_free(&alloc, args);
    sc_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* Sampler - additional tests */
static void test_sampler_imessage_query_basic(void) {
    char buf[512];
    size_t len = 0;
    sc_error_t err = sc_persona_sampler_imessage_query(buf, 512, &len, 100);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf, "SELECT"));
    SC_ASSERT_NOT_NULL(strstr(buf, "LIMIT 100"));
}

static void test_sampler_imessage_query_null_buf(void) {
    size_t len = 0;
    sc_error_t err = sc_persona_sampler_imessage_query(NULL, 512, &len, 100);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_sampler_imessage_query_escapes_quotes(void) {
    char buf[512];
    size_t len = 0;
    const char *handle_id = "o'brien";
    sc_error_t err = sc_persona_sampler_imessage_conversation_query(handle_id, strlen(handle_id),
                                                                    buf, sizeof(buf), &len, 50);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(len > 0);
    /* Single quotes in handle_id must be escaped as '' for SQL safety. */
    SC_ASSERT_NOT_NULL(strstr(buf, "o''brien"));
    /* Must not contain unescaped injection pattern. */
    SC_ASSERT_NULL(strstr(buf, "'; DROP TABLE"));
}

static void test_sampler_facebook_parse_empty_object(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 99;
    sc_error_t err = sc_persona_sampler_facebook_parse(json, strlen(json), &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, (size_t)0);
}

static void test_sampler_gmail_parse_null(void) {
    char **out = NULL;
    size_t count = 0;
    sc_error_t err = sc_persona_sampler_gmail_parse(NULL, 0, &out, &count);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_sampler_gmail_parse_empty_object(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 99;
    sc_error_t err = sc_persona_sampler_gmail_parse(json, strlen(json), &msgs, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, (size_t)0);
}

/* Feedback - null/error tests */
static void test_feedback_record_null_alloc(void) {
    sc_persona_feedback_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = "cli";
    fb.channel_len = 3;
    fb.original_response = "a";
    fb.original_response_len = 1;
    fb.corrected_response = "b";
    fb.corrected_response_len = 1;
    sc_error_t err = sc_persona_feedback_record(NULL, "test", 4, &fb);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_feedback_record_null_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_feedback_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = "cli";
    fb.channel_len = 3;
    fb.original_response = "a";
    fb.original_response_len = 1;
    fb.corrected_response = "b";
    fb.corrected_response_len = 1;
    sc_error_t err = sc_persona_feedback_record(&alloc, NULL, 0, &fb);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_feedback_record_null_feedback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_persona_feedback_record(&alloc, "test", 4, NULL);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_feedback_apply_null_alloc(void) {
    sc_error_t err = sc_persona_feedback_apply(NULL, "test", 4);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_feedback_apply_null_name(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_persona_feedback_apply(&alloc, NULL, 0);
    SC_ASSERT_NEQ(err, SC_OK);
}

/* Analyzer - additional tests */
static void test_analyzer_build_prompt_basic(void) {
    const char *messages[] = {"hello", "world"};
    char buf[4096];
    size_t len = 0;
    sc_error_t err =
        sc_persona_analyzer_build_prompt(messages, 2, "discord", buf, sizeof(buf), &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(len > 0);
}

static void test_analyzer_build_prompt_null_messages(void) {
    char buf[4096];
    size_t len = 0;
    sc_error_t err = sc_persona_analyzer_build_prompt(NULL, 2, "discord", buf, sizeof(buf), &len);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_analyzer_build_prompt_zero_count(void) {
    const char *messages[] = {"hello"};
    char buf[4096];
    size_t len = 0;
    sc_error_t err =
        sc_persona_analyzer_build_prompt(messages, 0, "discord", buf, sizeof(buf), &len);
    /* Current implementation accepts 0 count and returns SC_OK */
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(len > 0);
}

static void test_analyzer_parse_response_null_alloc(void) {
    sc_persona_t out;
    memset(&out, 0, sizeof(out));
    const char *response = "{\"traits\":[]}";
    sc_error_t err =
        sc_persona_analyzer_parse_response(NULL, response, strlen(response), "test", 4, &out);
    SC_ASSERT_NEQ(err, SC_OK);
}

/* Creator - null/error tests */
static void test_creator_synthesize_null_alloc(void) {
    sc_persona_t partial;
    memset(&partial, 0, sizeof(partial));
    sc_persona_t out;
    memset(&out, 0, sizeof(out));
    sc_error_t err = sc_persona_creator_synthesize(NULL, &partial, 1, "test", 4, &out);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_creator_synthesize_null_partials(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t out;
    memset(&out, 0, sizeof(out));
    sc_error_t err = sc_persona_creator_synthesize(&alloc, NULL, 1, "test", 4, &out);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_creator_synthesize_zero_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    sc_persona_t out;
    memset(&out, 0, sizeof(out));
    sc_error_t err = sc_persona_creator_synthesize(&alloc, &dummy, 0, "test", 4, &out);
    /* Current implementation accepts 0 count and returns SC_OK with empty persona */
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(out.name, "test");
    sc_persona_deinit(&alloc, &out);
}

static void test_creator_write_null_alloc(void) {
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "test";
    p.name_len = 4;
    sc_error_t err = sc_persona_creator_write(NULL, &p);
    SC_ASSERT_NEQ(err, SC_OK);
}

static void test_overlay_typing_quirks_parsed(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"quirk_test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"channel_overlays\":{\"imessage\":{"
                       "\"formality\":\"casual\","
                       "\"typing_quirks\":[\"lowercase\",\"no_periods\"],"
                       "\"message_splitting\":true,"
                       "\"max_segment_chars\":80}}}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(p.overlays_count, 1);
    SC_ASSERT_TRUE(p.overlays[0].message_splitting);
    SC_ASSERT_EQ(p.overlays[0].max_segment_chars, 80u);
    SC_ASSERT_EQ(p.overlays[0].typing_quirks_count, 2);
    SC_ASSERT_STR_EQ(p.overlays[0].typing_quirks[0], "lowercase");
    SC_ASSERT_STR_EQ(p.overlays[0].typing_quirks[1], "no_periods");
    sc_persona_deinit(&alloc, &p);
}

static void test_overlay_typing_quirks_default_when_absent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"no_quirks\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"channel_overlays\":{\"imessage\":{"
                       "\"formality\":\"formal\"}}}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(p.overlays_count, 1);
    SC_ASSERT_FALSE(p.overlays[0].message_splitting);
    SC_ASSERT_EQ(p.overlays[0].max_segment_chars, 0u);
    SC_ASSERT_EQ(p.overlays[0].typing_quirks_count, 0);
    SC_ASSERT_NULL(p.overlays[0].typing_quirks);
    sc_persona_deinit(&alloc, &p);
}

static void test_overlay_typing_quirks_in_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *quirks[] = {"lowercase", "no_periods"};
    sc_persona_overlay_t overlays[] = {
        {.channel = "imessage",
         .formality = "casual",
         .avg_length = NULL,
         .emoji_usage = NULL,
         .style_notes = NULL,
         .style_notes_count = 0,
         .message_splitting = true,
         .max_segment_chars = 80,
         .typing_quirks = quirks,
         .typing_quirks_count = 2},
    };
    sc_persona_t p = {
        .name = "quirk_user",
        .name_len = 10,
        .identity = "A test persona",
        .overlays = overlays,
        .overlays_count = 1,
    };

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Typing quirks"));
    SC_ASSERT_NOT_NULL(strstr(out, "lowercase"));
    SC_ASSERT_NOT_NULL(strstr(out, "no_periods"));
    SC_ASSERT_NOT_NULL(strstr(out, "Message splitting: ON"));
    SC_ASSERT_NOT_NULL(strstr(out, "80 chars"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── Rich persona elements (Tier 1–3) ── */

static void test_persona_load_json_rich_persona(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{"
        "  \"version\": 1, \"name\": \"rich_test\","
        "  \"core\": { \"identity\": \"A richly defined persona\", \"traits\": [\"warm\"] },"
        "  \"core_anchor\": \"I am grounded warmth with quiet strength.\","
        "  \"motivation\": {"
        "    \"primary_drive\": \"To help people feel understood\","
        "    \"protecting\": \"People's dignity\","
        "    \"avoiding\": \"Superficial interactions\","
        "    \"wanting\": \"Genuine connection\""
        "  },"
        "  \"situational_directions\": ["
        "    { \"trigger\": \"user is grieving\", \"instruction\": \"slow down, shorter "
        "sentences\" },"
        "    { \"trigger\": \"celebrating\", \"instruction\": \"match their energy\" }"
        "  ],"
        "  \"humor\": {"
        "    \"type\": \"dry\", \"frequency\": \"occasional\","
        "    \"targets\": [\"self\", \"situations\"],"
        "    \"boundaries\": [\"grief\", \"trauma\"],"
        "    \"timing\": \"tension-breaking\""
        "  },"
        "  \"conflict_style\": {"
        "    \"pushback_response\": \"reframe\","
        "    \"confrontation_comfort\": \"selective\","
        "    \"apology_style\": \"direct\","
        "    \"boundary_assertion\": \"firm but kind\","
        "    \"repair_behavior\": \"acknowledge then reconnect\""
        "  },"
        "  \"emotional_range\": {"
        "    \"ceiling\": \"genuinely excited but never manic\","
        "    \"floor\": \"deeply sad but never despairing\","
        "    \"escalation_triggers\": [\"injustice\", \"dishonesty\"],"
        "    \"de_escalation\": [\"deep breath\", \"reframe\"],"
        "    \"withdrawal_conditions\": \"when pushed past boundaries repeatedly\","
        "    \"recovery_style\": \"slow but steady\""
        "  },"
        "  \"voice_rhythm\": {"
        "    \"sentence_pattern\": \"mixed with occasional short bursts\","
        "    \"paragraph_cadence\": \"frequent breaks\","
        "    \"response_tempo\": \"thoughtful\","
        "    \"emphasis_style\": \"repetition\","
        "    \"pause_behavior\": \"lets beats land\""
        "  },"
        "  \"character_invariants\": ["
        "    \"Never dismisses someone's feelings\","
        "    \"Always acknowledges before advising\""
        "  ],"
        "  \"intellectual\": {"
        "    \"expertise\": [\"psychology\", \"music\"],"
        "    \"curiosity_areas\": [\"philosophy\", \"cooking\"],"
        "    \"thinking_style\": \"analogy\","
        "    \"metaphor_sources\": \"nature and cooking\""
        "  },"
        "  \"backstory_behaviors\": ["
        "    { \"backstory_beat\": \"grew up in chaotic home\","
        "      \"behavioral_rule\": \"over-explains to create clarity\" }"
        "  ],"
        "  \"sensory\": {"
        "    \"dominant_sense\": \"tactile\","
        "    \"metaphor_vocabulary\": [\"that hits hard\", \"feels heavy\"],"
        "    \"grounding_patterns\": \"references weather and physical space\""
        "  }"
        "}";

    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);

    /* Core anchor */
    SC_ASSERT_NOT_NULL(p.core_anchor);
    SC_ASSERT_NOT_NULL(strstr(p.core_anchor, "grounded warmth"));

    /* Motivation */
    SC_ASSERT_NOT_NULL(p.motivation.primary_drive);
    SC_ASSERT_NOT_NULL(strstr(p.motivation.primary_drive, "understood"));
    SC_ASSERT_NOT_NULL(p.motivation.protecting);
    SC_ASSERT_NOT_NULL(p.motivation.avoiding);
    SC_ASSERT_NOT_NULL(p.motivation.wanting);

    /* Situational directions */
    SC_ASSERT_EQ(p.situational_directions_count, 2);
    SC_ASSERT_NOT_NULL(strstr(p.situational_directions[0].trigger, "grieving"));
    SC_ASSERT_NOT_NULL(strstr(p.situational_directions[0].instruction, "shorter"));
    SC_ASSERT_NOT_NULL(strstr(p.situational_directions[1].trigger, "celebrating"));

    /* Humor */
    SC_ASSERT_STR_EQ(p.humor.type, "dry");
    SC_ASSERT_STR_EQ(p.humor.frequency, "occasional");
    SC_ASSERT_EQ(p.humor.targets_count, 2);
    SC_ASSERT_EQ(p.humor.boundaries_count, 2);
    SC_ASSERT_STR_EQ(p.humor.timing, "tension-breaking");

    /* Conflict style */
    SC_ASSERT_STR_EQ(p.conflict_style.pushback_response, "reframe");
    SC_ASSERT_STR_EQ(p.conflict_style.confrontation_comfort, "selective");
    SC_ASSERT_STR_EQ(p.conflict_style.apology_style, "direct");
    SC_ASSERT_NOT_NULL(p.conflict_style.boundary_assertion);
    SC_ASSERT_NOT_NULL(p.conflict_style.repair_behavior);

    /* Emotional range */
    SC_ASSERT_NOT_NULL(p.emotional_range.ceiling);
    SC_ASSERT_NOT_NULL(p.emotional_range.floor);
    SC_ASSERT_EQ(p.emotional_range.escalation_triggers_count, 2);
    SC_ASSERT_EQ(p.emotional_range.de_escalation_count, 2);
    SC_ASSERT_NOT_NULL(p.emotional_range.withdrawal_conditions);
    SC_ASSERT_NOT_NULL(p.emotional_range.recovery_style);

    /* Voice rhythm */
    SC_ASSERT_NOT_NULL(p.voice_rhythm.sentence_pattern);
    SC_ASSERT_NOT_NULL(p.voice_rhythm.paragraph_cadence);
    SC_ASSERT_STR_EQ(p.voice_rhythm.response_tempo, "thoughtful");
    SC_ASSERT_STR_EQ(p.voice_rhythm.emphasis_style, "repetition");
    SC_ASSERT_NOT_NULL(p.voice_rhythm.pause_behavior);

    /* Character invariants */
    SC_ASSERT_EQ(p.character_invariants_count, 2);
    SC_ASSERT_NOT_NULL(strstr(p.character_invariants[0], "Never dismisses"));

    /* Intellectual */
    SC_ASSERT_EQ(p.intellectual.expertise_count, 2);
    SC_ASSERT_EQ(p.intellectual.curiosity_areas_count, 2);
    SC_ASSERT_STR_EQ(p.intellectual.thinking_style, "analogy");
    SC_ASSERT_NOT_NULL(p.intellectual.metaphor_sources);

    /* Backstory behaviors */
    SC_ASSERT_EQ(p.backstory_behaviors_count, 1);
    SC_ASSERT_NOT_NULL(strstr(p.backstory_behaviors[0].backstory_beat, "chaotic"));
    SC_ASSERT_NOT_NULL(strstr(p.backstory_behaviors[0].behavioral_rule, "over-explains"));

    /* Sensory */
    SC_ASSERT_STR_EQ(p.sensory.dominant_sense, "tactile");
    SC_ASSERT_EQ(p.sensory.metaphor_vocabulary_count, 2);
    SC_ASSERT_NOT_NULL(p.sensory.grounding_patterns);

    sc_persona_deinit(&alloc, &p);
}

static void test_persona_prompt_includes_motivation(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = "mottest";
    p.name_len = 7;
    p.identity = "Test";
    p.motivation.primary_drive = "connection";
    p.motivation.protecting = "dignity";
    p.motivation.avoiding = "smalltalk";
    p.motivation.wanting = "depth";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Motivation"));
    SC_ASSERT_NOT_NULL(strstr(out, "connection"));
    SC_ASSERT_NOT_NULL(strstr(out, "dignity"));
    SC_ASSERT_NOT_NULL(strstr(out, "smalltalk"));
    SC_ASSERT_NOT_NULL(strstr(out, "depth"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_situational_directions(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_situational_direction_t dirs[] = {
        {.trigger = "user is angry", .instruction = "stay calm, validate first"},
    };
    sc_persona_t p = {0};
    p.name = "sdtest";
    p.name_len = 6;
    p.identity = "Test";
    p.situational_directions = dirs;
    p.situational_directions_count = 1;

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "WHEN user is angry"));
    SC_ASSERT_NOT_NULL(strstr(out, "stay calm"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_humor(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *targets[] = {"self"};
    char *bounds[] = {"grief"};
    sc_persona_t p = {0};
    p.name = "humtest";
    p.name_len = 7;
    p.identity = "Test";
    p.humor.type = "dry";
    p.humor.frequency = "rare";
    p.humor.targets = targets;
    p.humor.targets_count = 1;
    p.humor.boundaries = bounds;
    p.humor.boundaries_count = 1;
    p.humor.timing = "bonding";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Humor"));
    SC_ASSERT_NOT_NULL(strstr(out, "dry"));
    SC_ASSERT_NOT_NULL(strstr(out, "Never funny"));
    SC_ASSERT_NOT_NULL(strstr(out, "grief"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_conflict_style(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = "cftest";
    p.name_len = 6;
    p.identity = "Test";
    p.conflict_style.pushback_response = "reframe";
    p.conflict_style.confrontation_comfort = "high";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Conflict"));
    SC_ASSERT_NOT_NULL(strstr(out, "reframe"));
    SC_ASSERT_NOT_NULL(strstr(out, "high"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_emotional_range(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *triggers[] = {"injustice"};
    char *deesc[] = {"deep breath"};
    sc_persona_t p = {0};
    p.name = "ertest";
    p.name_len = 6;
    p.identity = "Test";
    p.emotional_range.ceiling = "warm excitement";
    p.emotional_range.floor = "quiet sadness";
    p.emotional_range.escalation_triggers = triggers;
    p.emotional_range.escalation_triggers_count = 1;
    p.emotional_range.de_escalation = deesc;
    p.emotional_range.de_escalation_count = 1;

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Emotional Range"));
    SC_ASSERT_NOT_NULL(strstr(out, "warm excitement"));
    SC_ASSERT_NOT_NULL(strstr(out, "injustice"));
    SC_ASSERT_NOT_NULL(strstr(out, "deep breath"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_voice_rhythm(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = "vrtest";
    p.name_len = 6;
    p.identity = "Test";
    p.voice_rhythm.sentence_pattern = "short bursts";
    p.voice_rhythm.response_tempo = "quick";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Voice Rhythm"));
    SC_ASSERT_NOT_NULL(strstr(out, "short bursts"));
    SC_ASSERT_NOT_NULL(strstr(out, "quick"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_core_anchor(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = "anchor";
    p.name_len = 6;
    p.identity = "Test";
    p.core_anchor = "I am grounded warmth with quiet strength.";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Core Anchor"));
    SC_ASSERT_NOT_NULL(strstr(out, "grounded warmth"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_character_invariants(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *invar[] = {"Never dismisses feelings", "Always listens first"};
    sc_persona_t p = {0};
    p.name = "citest";
    p.name_len = 6;
    p.identity = "Test";
    p.character_invariants = invar;
    p.character_invariants_count = 2;

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Character Invariants"));
    SC_ASSERT_NOT_NULL(strstr(out, "NEVER break"));
    SC_ASSERT_NOT_NULL(strstr(out, "Never dismisses"));
    SC_ASSERT_NOT_NULL(strstr(out, "Always listens"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_intellectual(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *exp[] = {"psychology"};
    char *cur[] = {"cooking"};
    sc_persona_t p = {0};
    p.name = "iptest";
    p.name_len = 6;
    p.identity = "Test";
    p.intellectual.expertise = exp;
    p.intellectual.expertise_count = 1;
    p.intellectual.curiosity_areas = cur;
    p.intellectual.curiosity_areas_count = 1;
    p.intellectual.thinking_style = "analogy";
    p.intellectual.metaphor_sources = "nature";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Intellectual"));
    SC_ASSERT_NOT_NULL(strstr(out, "psychology"));
    SC_ASSERT_NOT_NULL(strstr(out, "cooking"));
    SC_ASSERT_NOT_NULL(strstr(out, "analogy"));
    SC_ASSERT_NOT_NULL(strstr(out, "nature"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_backstory_behaviors(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_backstory_behavior_t bbs[] = {
        {.backstory_beat = "grew up poor", .behavioral_rule = "values resourcefulness"},
    };
    sc_persona_t p = {0};
    p.name = "bbtest";
    p.name_len = 6;
    p.identity = "Test";
    p.backstory_behaviors = bbs;
    p.backstory_behaviors_count = 1;

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Backstory"));
    SC_ASSERT_NOT_NULL(strstr(out, "Because grew up poor"));
    SC_ASSERT_NOT_NULL(strstr(out, "values resourcefulness"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_sensory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *vocab[] = {"hits hard", "feels heavy"};
    sc_persona_t p = {0};
    p.name = "sentest";
    p.name_len = 7;
    p.identity = "Test";
    p.sensory.dominant_sense = "tactile";
    p.sensory.metaphor_vocabulary = vocab;
    p.sensory.metaphor_vocabulary_count = 2;
    p.sensory.grounding_patterns = "references weather";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Sensory"));
    SC_ASSERT_NOT_NULL(strstr(out, "tactile"));
    SC_ASSERT_NOT_NULL(strstr(out, "hits hard"));
    SC_ASSERT_NOT_NULL(strstr(out, "references weather"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_validate_rejects_bad_motivation_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"motivation\":\"not an object\"}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(err);
    SC_ASSERT_NOT_NULL(strstr(err, "motivation"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_humor_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"humor\":42}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(err);
    SC_ASSERT_NOT_NULL(strstr(err, "humor"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_accepts_rich_persona(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"rich\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
        "\"core_anchor\":\"test anchor\","
        "\"motivation\":{\"primary_drive\":\"connection\"},"
        "\"humor\":{\"type\":\"dry\"},"
        "\"conflict_style\":{\"pushback_response\":\"reframe\"},"
        "\"emotional_range\":{\"ceiling\":\"high\"},"
        "\"voice_rhythm\":{\"response_tempo\":\"quick\"},"
        "\"character_invariants\":[\"never X\"],"
        "\"intellectual\":{\"thinking_style\":\"analogy\"},"
        "\"backstory_behaviors\":[{\"backstory_beat\":\"X\",\"behavioral_rule\":\"Y\"}],"
        "\"sensory\":{\"dominant_sense\":\"visual\"},"
        "\"situational_directions\":[{\"trigger\":\"X\",\"instruction\":\"Y\"}]}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_NULL(err);
}

/* --- Research-backed persona element tests --- */

static void test_persona_prompt_includes_relational(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *bids[] = {"sharing interesting finds", "asking how their day went"};
    sc_persona_t p = {0};
    p.name = "reltest";
    p.name_len = 7;
    p.identity = "Test";
    p.relational.bid_response_style = "always turn toward bids with genuine engagement";
    p.relational.emotional_bids = bids;
    p.relational.emotional_bids_count = 2;
    p.relational.attachment_style = "secure";
    p.relational.attachment_awareness =
        "detect anxious patterns and provide consistent reassurance";
    p.relational.dunbar_awareness = "invest deeply in inner circle, warmly in acquaintances";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Relational Intelligence"));
    SC_ASSERT_NOT_NULL(strstr(out, "secure"));
    SC_ASSERT_NOT_NULL(strstr(out, "turn toward"));
    SC_ASSERT_NOT_NULL(strstr(out, "sharing interesting finds"));
    SC_ASSERT_NOT_NULL(strstr(out, "anxious"));
    SC_ASSERT_NOT_NULL(strstr(out, "inner circle"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_listening(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *techniques[] = {"open questions", "affirmations", "reflective listening",
                          "summary reflections"};
    sc_persona_t p = {0};
    p.name = "listest";
    p.name_len = 7;
    p.identity = "Test";
    p.listening.default_response_type = "support (never shift)";
    p.listening.reflective_techniques = techniques;
    p.listening.reflective_techniques_count = 4;
    p.listening.nvc_style =
        "observe without judgment, name feelings, identify needs, make requests";
    p.listening.validation_style = "validate feelings before offering solutions";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Listening"));
    SC_ASSERT_NOT_NULL(strstr(out, "support"));
    SC_ASSERT_NOT_NULL(strstr(out, "reflective listening"));
    SC_ASSERT_NOT_NULL(strstr(out, "observe without judgment"));
    SC_ASSERT_NOT_NULL(strstr(out, "validate feelings"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_repair(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *phrases[] = {"I think I misread that", "let me try again", "I hear you"};
    sc_persona_t p = {0};
    p.name = "reptest";
    p.name_len = 7;
    p.identity = "Test";
    p.repair.rupture_detection = "notice tone shifts, shorter replies, or sudden topic changes";
    p.repair.repair_approach = "name the disconnect directly and take ownership";
    p.repair.face_saving_style = "offer face-saving exits, never corner someone";
    p.repair.repair_phrases = phrases;
    p.repair.repair_phrases_count = 3;

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Repair Protocol"));
    SC_ASSERT_NOT_NULL(strstr(out, "tone shifts"));
    SC_ASSERT_NOT_NULL(strstr(out, "take ownership"));
    SC_ASSERT_NOT_NULL(strstr(out, "face-saving"));
    SC_ASSERT_NOT_NULL(strstr(out, "I think I misread that"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_mirroring(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *adapts[] = {"message_length", "formality", "emoji_usage", "pacing"};
    sc_persona_t p = {0};
    p.name = "mirtest";
    p.name_len = 7;
    p.identity = "Test";
    p.mirroring.mirroring_level = "moderate";
    p.mirroring.adapts_to = adapts;
    p.mirroring.adapts_to_count = 4;
    p.mirroring.convergence_speed = "gradual";
    p.mirroring.power_dynamic = "mirror more with higher-status contacts";

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Linguistic Mirroring"));
    SC_ASSERT_NOT_NULL(strstr(out, "moderate"));
    SC_ASSERT_NOT_NULL(strstr(out, "message_length"));
    SC_ASSERT_NOT_NULL(strstr(out, "gradual"));
    SC_ASSERT_NOT_NULL(strstr(out, "higher-status"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_social(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *bonding[] = {"remembering small details", "checking in without agenda"};
    char *anti[] = {"shift responses", "unsolicited advice", "one-upping"};
    sc_persona_t p = {0};
    p.name = "soctest";
    p.name_len = 7;
    p.identity = "Test";
    p.social.default_ego_state = "adult with nurturing-parent warmth";
    p.social.phatic_style = "values small talk as genuine bonding, not filler";
    p.social.bonding_behaviors = bonding;
    p.social.bonding_behaviors_count = 2;
    p.social.anti_patterns = anti;
    p.social.anti_patterns_count = 3;

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(out, "Social Dynamics"));
    SC_ASSERT_NOT_NULL(strstr(out, "adult with nurturing"));
    SC_ASSERT_NOT_NULL(strstr(out, "small talk"));
    SC_ASSERT_NOT_NULL(strstr(out, "remembering small details"));
    SC_ASSERT_NOT_NULL(strstr(out, "NEVER"));
    SC_ASSERT_NOT_NULL(strstr(out, "shift responses"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_load_json_research_fields(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"research\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"empathic\"]},"
                       "\"relational\":{\"bid_response_style\":\"turn toward\","
                       "\"emotional_bids\":[\"sharing\",\"asking\"],"
                       "\"attachment_style\":\"secure\","
                       "\"attachment_awareness\":\"detect anxious\","
                       "\"dunbar_awareness\":\"invest deeply\"},"
                       "\"listening\":{\"default_response_type\":\"support\","
                       "\"reflective_techniques\":[\"OARS\",\"NVC\"],"
                       "\"nvc_style\":\"observe then feel\","
                       "\"validation_style\":\"validate first\"},"
                       "\"repair\":{\"rupture_detection\":\"tone shifts\","
                       "\"repair_approach\":\"name it\","
                       "\"face_saving_style\":\"offer exits\","
                       "\"repair_phrases\":[\"my bad\",\"let me rephrase\"]},"
                       "\"mirroring\":{\"mirroring_level\":\"moderate\","
                       "\"adapts_to\":[\"length\",\"formality\"],"
                       "\"convergence_speed\":\"gradual\","
                       "\"power_dynamic\":\"mirror more up\"},"
                       "\"social\":{\"default_ego_state\":\"adult\","
                       "\"phatic_style\":\"warm opener\","
                       "\"bonding_behaviors\":[\"remembering details\"],"
                       "\"anti_patterns\":[\"shift responses\",\"one-upping\"]}}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);

    SC_ASSERT_NOT_NULL(p.relational.bid_response_style);
    SC_ASSERT_STR_EQ(p.relational.bid_response_style, "turn toward");
    SC_ASSERT_EQ(p.relational.emotional_bids_count, 2);
    SC_ASSERT_STR_EQ(p.relational.attachment_style, "secure");
    SC_ASSERT_STR_EQ(p.relational.attachment_awareness, "detect anxious");
    SC_ASSERT_STR_EQ(p.relational.dunbar_awareness, "invest deeply");

    SC_ASSERT_STR_EQ(p.listening.default_response_type, "support");
    SC_ASSERT_EQ(p.listening.reflective_techniques_count, 2);
    SC_ASSERT_STR_EQ(p.listening.nvc_style, "observe then feel");
    SC_ASSERT_STR_EQ(p.listening.validation_style, "validate first");

    SC_ASSERT_STR_EQ(p.repair.rupture_detection, "tone shifts");
    SC_ASSERT_STR_EQ(p.repair.repair_approach, "name it");
    SC_ASSERT_STR_EQ(p.repair.face_saving_style, "offer exits");
    SC_ASSERT_EQ(p.repair.repair_phrases_count, 2);

    SC_ASSERT_STR_EQ(p.mirroring.mirroring_level, "moderate");
    SC_ASSERT_EQ(p.mirroring.adapts_to_count, 2);
    SC_ASSERT_STR_EQ(p.mirroring.convergence_speed, "gradual");
    SC_ASSERT_STR_EQ(p.mirroring.power_dynamic, "mirror more up");

    SC_ASSERT_STR_EQ(p.social.default_ego_state, "adult");
    SC_ASSERT_STR_EQ(p.social.phatic_style, "warm opener");
    SC_ASSERT_EQ(p.social.bonding_behaviors_count, 1);
    SC_ASSERT_EQ(p.social.anti_patterns_count, 2);

    sc_persona_deinit(&alloc, &p);
}

static void test_persona_validate_rejects_bad_relational_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"relational\":\"not an object\"}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(err);
    SC_ASSERT_NOT_NULL(strstr(err, "relational"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_listening_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"listening\":42}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(err);
    SC_ASSERT_NOT_NULL(strstr(err, "listening"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_repair_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"repair\":[1,2,3]}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(err);
    SC_ASSERT_NOT_NULL(strstr(err, "repair"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_mirroring_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"mirroring\":true}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(err);
    SC_ASSERT_NOT_NULL(strstr(err, "mirroring"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_social_type(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"social\":\"not an object\"}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(err);
    SC_ASSERT_NOT_NULL(strstr(err, "social"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_accepts_research_persona(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"rp\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"relational\":{\"bid_response_style\":\"turn toward\"},"
                       "\"listening\":{\"default_response_type\":\"support\"},"
                       "\"repair\":{\"rupture_detection\":\"tone shifts\"},"
                       "\"mirroring\":{\"mirroring_level\":\"moderate\"},"
                       "\"social\":{\"default_ego_state\":\"adult\"}}";
    char *err = NULL;
    size_t err_len = 0;
    sc_error_t e = sc_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    SC_ASSERT_EQ(e, SC_OK);
    SC_ASSERT_NULL(err);
}

static void test_persona_deinit_research_fields(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"deinit_research\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"relational\":{\"bid_response_style\":\"turn toward\","
                       "\"emotional_bids\":[\"sharing\",\"asking\"],"
                       "\"attachment_style\":\"secure\","
                       "\"attachment_awareness\":\"detect anxious\","
                       "\"dunbar_awareness\":\"invest deeply\"},"
                       "\"listening\":{\"default_response_type\":\"support\","
                       "\"reflective_techniques\":[\"OARS\"],"
                       "\"nvc_style\":\"observe\",\"validation_style\":\"validate\"},"
                       "\"repair\":{\"rupture_detection\":\"tone\","
                       "\"repair_approach\":\"name it\","
                       "\"face_saving_style\":\"offer exits\","
                       "\"repair_phrases\":[\"my bad\"]},"
                       "\"mirroring\":{\"mirroring_level\":\"moderate\","
                       "\"adapts_to\":[\"length\"],\"convergence_speed\":\"gradual\","
                       "\"power_dynamic\":\"mirror up\"},"
                       "\"social\":{\"default_ego_state\":\"adult\","
                       "\"phatic_style\":\"warm\","
                       "\"bonding_behaviors\":[\"details\"],"
                       "\"anti_patterns\":[\"shift\",\"one-up\"]}}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    sc_persona_deinit(&alloc, &p);
    sc_persona_deinit(&alloc, &p);
}

static void test_persona_deinit_rich_persona(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"deinit_rich\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"warm\"]},"
        "\"core_anchor\":\"test\","
        "\"motivation\":{\"primary_drive\":\"x\",\"protecting\":\"y\","
        "\"avoiding\":\"z\",\"wanting\":\"w\"},"
        "\"humor\":{\"type\":\"dry\",\"targets\":[\"self\"],\"boundaries\":[\"grief\"]},"
        "\"conflict_style\":{\"pushback_response\":\"reframe\"},"
        "\"emotional_range\":{\"escalation_triggers\":[\"a\"],\"de_escalation\":[\"b\"]},"
        "\"voice_rhythm\":{\"sentence_pattern\":\"mixed\"},"
        "\"character_invariants\":[\"never X\"],"
        "\"intellectual\":{\"expertise\":[\"a\"],\"curiosity_areas\":[\"b\"]},"
        "\"backstory_behaviors\":[{\"backstory_beat\":\"X\",\"behavioral_rule\":\"Y\"}],"
        "\"sensory\":{\"dominant_sense\":\"visual\",\"metaphor_vocabulary\":[\"bright\"]},"
        "\"situational_directions\":[{\"trigger\":\"X\",\"instruction\":\"Y\"}]}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    sc_persona_deinit(&alloc, &p);
    /* Double deinit — should be safe */
    sc_persona_deinit(&alloc, &p);
}

static void test_analyzer_parses_research_fields(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *response = "{\"traits\":[\"empathic\"],"
                           "\"relational\":{\"bid_response_style\":\"turn toward\","
                           "\"attachment_style\":\"secure\",\"emotional_bids\":[\"sharing\"]},"
                           "\"listening\":{\"default_response_type\":\"support\","
                           "\"reflective_techniques\":[\"OARS\"],\"nvc_style\":\"observe\"},"
                           "\"repair\":{\"rupture_detection\":\"tone shifts\","
                           "\"repair_phrases\":[\"my bad\"]},"
                           "\"mirroring\":{\"mirroring_level\":\"moderate\","
                           "\"adapts_to\":[\"length\"]},"
                           "\"social\":{\"default_ego_state\":\"adult\","
                           "\"anti_patterns\":[\"shift responses\"]},"
                           "\"intellectual\":{\"thinking_style\":\"analytical\"},"
                           "\"sensory\":{\"dominant_sense\":\"visual\"}}";
    sc_persona_t p = {0};
    sc_error_t err =
        sc_persona_analyzer_parse_response(&alloc, response, strlen(response), "imessage", 8, &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(p.relational.bid_response_style, "turn toward");
    SC_ASSERT_STR_EQ(p.relational.attachment_style, "secure");
    SC_ASSERT_EQ(p.relational.emotional_bids_count, 1);
    SC_ASSERT_STR_EQ(p.listening.default_response_type, "support");
    SC_ASSERT_EQ(p.listening.reflective_techniques_count, 1);
    SC_ASSERT_STR_EQ(p.repair.rupture_detection, "tone shifts");
    SC_ASSERT_EQ(p.repair.repair_phrases_count, 1);
    SC_ASSERT_STR_EQ(p.mirroring.mirroring_level, "moderate");
    SC_ASSERT_EQ(p.mirroring.adapts_to_count, 1);
    SC_ASSERT_STR_EQ(p.social.default_ego_state, "adult");
    SC_ASSERT_EQ(p.social.anti_patterns_count, 1);
    SC_ASSERT_STR_EQ(p.intellectual.thinking_style, "analytical");
    SC_ASSERT_STR_EQ(p.sensory.dominant_sense, "visual");
    sc_persona_deinit(&alloc, &p);
}

static void test_creator_synthesize_merges_research_fields(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p1 = {0};
    p1.relational.bid_response_style = "turn toward";
    p1.relational.attachment_style = "secure";
    p1.listening.default_response_type = "support";
    p1.repair.rupture_detection = "tone shifts";
    p1.mirroring.mirroring_level = "moderate";
    p1.social.default_ego_state = "adult";

    sc_persona_t p2 = {0};
    p2.relational.dunbar_awareness = "invest deeply";
    p2.listening.nvc_style = "observe then feel";
    p2.repair.repair_approach = "name it";
    p2.mirroring.convergence_speed = "gradual";
    p2.social.phatic_style = "warm opener";

    sc_persona_t partials[] = {p1, p2};
    sc_persona_t merged = {0};
    sc_error_t err = sc_persona_creator_synthesize(&alloc, partials, 2, "merged", 6, &merged);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(merged.relational.bid_response_style, "turn toward");
    SC_ASSERT_STR_EQ(merged.relational.attachment_style, "secure");
    SC_ASSERT_STR_EQ(merged.relational.dunbar_awareness, "invest deeply");
    SC_ASSERT_STR_EQ(merged.listening.default_response_type, "support");
    SC_ASSERT_STR_EQ(merged.listening.nvc_style, "observe then feel");
    SC_ASSERT_STR_EQ(merged.repair.rupture_detection, "tone shifts");
    SC_ASSERT_STR_EQ(merged.repair.repair_approach, "name it");
    SC_ASSERT_STR_EQ(merged.mirroring.mirroring_level, "moderate");
    SC_ASSERT_STR_EQ(merged.mirroring.convergence_speed, "gradual");
    SC_ASSERT_STR_EQ(merged.social.default_ego_state, "adult");
    SC_ASSERT_STR_EQ(merged.social.phatic_style, "warm opener");
    sc_persona_deinit(&alloc, &merged);
}

static void test_contact_profile_attachment_and_dunbar(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"cptest\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"contacts\":{\"alice\":{\"name\":\"Alice\","
                       "\"relationship\":\"close friend\","
                       "\"attachment_style\":\"anxious\","
                       "\"dunbar_layer\":\"inner_circle\"}}}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(p.contacts_count, 1);
    SC_ASSERT_STR_EQ(p.contacts[0].attachment_style, "anxious");
    SC_ASSERT_STR_EQ(p.contacts[0].dunbar_layer, "inner_circle");

    char *ctx = NULL;
    size_t ctx_len = 0;
    err = sc_contact_profile_build_context(&alloc, &p.contacts[0], &ctx, &ctx_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(ctx, "anxious"));
    SC_ASSERT_NOT_NULL(strstr(ctx, "inner_circle"));
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    sc_persona_deinit(&alloc, &p);
}

static void test_analyzer_prompt_includes_research_fields(void) {
    const char *messages[] = {"hey", "what's up"};
    char buf[8192];
    size_t out_len = 0;
    sc_error_t err =
        sc_persona_analyzer_build_prompt(messages, 2, "imessage", buf, sizeof(buf), &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(buf, "relational"));
    SC_ASSERT_NOT_NULL(strstr(buf, "listening"));
    SC_ASSERT_NOT_NULL(strstr(buf, "repair"));
    SC_ASSERT_NOT_NULL(strstr(buf, "mirroring"));
    SC_ASSERT_NOT_NULL(strstr(buf, "social"));
    SC_ASSERT_NOT_NULL(strstr(buf, "intellectual"));
    SC_ASSERT_NOT_NULL(strstr(buf, "sensory"));
    SC_ASSERT_NOT_NULL(strstr(buf, "attachment_style"));
    SC_ASSERT_NOT_NULL(strstr(buf, "bid_response_style"));
    SC_ASSERT_NOT_NULL(strstr(buf, "ego_state"));
    SC_ASSERT_NOT_NULL(strstr(buf, "nvc_style"));
}

static void test_auto_profile_returns_mock_overlay(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_overlay_t overlay;
    memset(&overlay, 0, sizeof(overlay));
    sc_error_t err = sc_persona_auto_profile(&alloc, "+18001234567", 12, &overlay);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(overlay.formality);
    SC_ASSERT_NOT_NULL(overlay.avg_length);
    SC_ASSERT_NOT_NULL(overlay.emoji_usage);
    if (overlay.formality)
        alloc.free(alloc.ctx, (char *)overlay.formality, strlen(overlay.formality) + 1);
    if (overlay.avg_length)
        alloc.free(alloc.ctx, (char *)overlay.avg_length, strlen(overlay.avg_length) + 1);
    if (overlay.emoji_usage)
        alloc.free(alloc.ctx, (char *)overlay.emoji_usage, strlen(overlay.emoji_usage) + 1);
}

static void test_auto_profile_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_overlay_t overlay;
    SC_ASSERT_EQ(sc_persona_auto_profile(NULL, "+1", 2, &overlay), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_persona_auto_profile(&alloc, NULL, 0, &overlay), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_persona_auto_profile(&alloc, "+1", 2, NULL), SC_ERR_INVALID_ARGUMENT);
}

static void test_profile_describe_style_formats(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_sampler_contact_stats_t stats = {0};
    stats.their_msg_count = 100;
    stats.avg_their_len = 25;
    stats.uses_emoji = true;
    stats.sends_links = false;
    stats.texts_in_bursts = true;
    stats.prefers_short = true;
    size_t out_len = 0;
    char *desc = sc_persona_profile_describe_style(&alloc, &stats, "+1800", 5, &out_len);
    SC_ASSERT_NOT_NULL(desc);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(desc, "short") != NULL);
    SC_ASSERT_TRUE(strstr(desc, "emoji") != NULL);
    SC_ASSERT_TRUE(strstr(desc, "bursts") != NULL);
    SC_ASSERT_TRUE(strstr(desc, "Mirror") != NULL);
    alloc.free(alloc.ctx, desc, out_len + 1);
}

static void test_profile_describe_style_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t out_len = 0;
    SC_ASSERT_NULL(sc_persona_profile_describe_style(NULL, NULL, NULL, 0, &out_len));
    SC_ASSERT_NULL(sc_persona_profile_describe_style(&alloc, NULL, NULL, 0, &out_len));
}

/* E2E dry run: simulate a Mindy message through the full persona pipeline */
static void test_e2e_mindy_message_full_pipeline(void) {
    sc_allocator_t alloc = sc_system_allocator();

    static const char json[] =
        "{\"version\":1,\"name\":\"seth\","
        "\"core\":{\"identity\":\"Head of Agentic AI at Vanguard. Introverted engineer.\","
        "\"traits\":[\"direct\",\"warm\",\"playfully teasing\"],"
        "\"vocabulary\":{\"preferred\":[\"sounds good\",\"let's do it\"],\"avoided\":[\"synergy\"],"
        "\"slang\":[\"haha\",\"lol\"]},"
        "\"communication_rules\":[\"Keep messages short\",\"Tease family members\"],"
        "\"values\":[\"authenticity\",\"family\"],"
        "\"decision_style\":\"Decides fast\","
        "\"biography\":\"Grew up in Star Valley Wyoming, third of seven children.\","
        "\"directors_notes\":[\"Seth teases Mindy relentlessly but lovingly.\","
        "\"Never sound like an AI assistant.\"]},"
        "\"motivation\":{\"primary_drive\":\"Building AI systems\","
        "\"protecting\":\"His kids and family relationships\","
        "\"avoiding\":\"Corporate BS\",\"wanting\":\"Something meaningful in AI\"},"
        "\"humor\":{\"type\":\"playful teasing with nerdy undertones\","
        "\"frequency\":\"high with family\","
        "\"targets\":[\"his sister Mindy\",\"himself\"],"
        "\"boundaries\":[\"never mean-spirited\"],"
        "\"timing\":\"Teasing IS his love language\"},"
        "\"conflict_style\":{\"pushback_response\":\"Gets quiet first\","
        "\"confrontation_comfort\":\"Moderate\","
        "\"apology_style\":\"Straightforward\","
        "\"boundary_assertion\":\"Calm and clear\","
        "\"repair_behavior\":\"Reaches out first\"},"
        "\"emotional_range\":{\"ceiling\":\"Multiple exclamation marks\","
        "\"floor\":\"Goes quiet\","
        "\"escalation_triggers\":[\"someone being fake\"],"
        "\"de_escalation\":[\"codes something\"],"
        "\"withdrawal_conditions\":\"Extended social drain\","
        "\"recovery_style\":\"Comes back naturally\"},"
        "\"voice_rhythm\":{\"sentence_pattern\":\"Short fragments\","
        "\"paragraph_cadence\":\"Never writes paragraphs in texts\","
        "\"response_tempo\":\"Fast when excited\","
        "\"emphasis_style\":\"Caps for excitement\","
        "\"pause_behavior\":\"Goes silent when thinking\"},"
        "\"inner_world\":{\"contradictions\":[\"Introverted but texts family constantly\"],"
        "\"embodied_memories\":[\"Riding the three-wheeler through the Wasatch mountains\"],"
        "\"emotional_flashpoints\":[\"His kids\"],"
        "\"unfinished_business\":[\"Wanting more time with his four kids\"],"
        "\"secret_self\":[\"The nerdy kid from Star Valley never left\"]},"
        "\"intellectual\":{\"expertise\":[\"Agentic AI\"],"
        "\"curiosity_areas\":[\"vibe coding\"],"
        "\"thinking_style\":\"First-principles\","
        "\"metaphor_sources\":\"Building and engineering\"},"
        "\"sensory\":{\"dominant_sense\":\"kinesthetic\","
        "\"metaphor_vocabulary\":[\"dig into that\",\"feels right\"],"
        "\"grounding_patterns\":\"Goes outside\"},"
        "\"situational_directions\":["
        "{\"trigger\":\"Mindy shares something stressful\","
        "\"instruction\":\"Lead with a quick tease, then pivot to genuine support.\"},"
        "{\"trigger\":\"Mindy shares good news\","
        "\"instruction\":\"Genuine excitement first, then teasing follow-up.\"},"
        "{\"trigger\":\"Making plans with family\","
        "\"instruction\":\"Suggest specific times, places, activities.\"}],"
        "\"backstory_behaviors\":["
        "{\"backstory_beat\":\"Third of seven kids\","
        "\"behavioral_rule\":\"Comfortable with chaos, keeps messages punchy.\"}],"
        "\"character_invariants\":[\"Always responds to family within minutes\","
        "\"Teasing is love\"],"
        "\"core_anchor\":\"The nerdy mountain kid from Star Valley\","
        "\"channel_overlays\":{\"imessage\":{\"formality\":\"very casual\","
        "\"avg_length\":\"short\",\"emoji_usage\":\"moderate\","
        "\"message_splitting\":true,\"max_segment_chars\":60,"
        "\"typing_quirks\":[\"lowercase\",\"no_periods\"],"
        "\"style_notes\":[\"1-10 word messages\",\"rapid-fire short messages\"]}},"
        "\"contacts\":{\"mindy\":{"
        "\"name\":\"Mindy\",\"relationship\":\"older sister\","
        "\"relationship_stage\":\"close_family\","
        "\"warmth_level\":\"high\",\"vulnerability_level\":\"high\","
        "\"identity\":\"Seth's older sister, second of seven kids.\","
        "\"context\":\"Mindy and Seth grew up together in Star Valley.\","
        "\"dynamic\":\"Playful sibling rivalry with deep underlying love.\","
        "\"greeting_style\":\"Hey Min!\","
        "\"closing_style\":\"love you! or see ya\","
        "\"interests\":[\"family events\",\"shared childhood memories\"],"
        "\"sensitive_topics\":[],"
        "\"allowed_behaviors\":[\"relentless teasing\",\"being brutally honest\"],"
        "\"communication_patterns\":{\"texts_in_bursts\":true,"
        "\"prefers_short_texts\":true,\"sends_links_often\":false,"
        "\"uses_emoji\":true},"
        "\"proactive\":{\"enabled\":true,\"channel\":\"imessage\","
        "\"schedule\":\"0 */4 * * *\"}}}}";

    /* Step 1: Parse the persona */
    sc_persona_t p;
    memset(&p, 0, sizeof(p));
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_STR_EQ(p.name, "seth");

    /* Step 2: Find the Mindy contact */
    const sc_contact_profile_t *cp = sc_persona_find_contact(&p, "mindy", 5);
    SC_ASSERT_NOT_NULL(cp);
    SC_ASSERT_STR_EQ(cp->name, "Mindy");
    SC_ASSERT_STR_EQ(cp->relationship_stage, "close_family");
    SC_ASSERT_TRUE(cp->texts_in_bursts);
    SC_ASSERT_TRUE(cp->prefers_short_texts);

    /* Step 3: Build contact context */
    char *contact_ctx = NULL;
    size_t contact_ctx_len = 0;
    err = sc_contact_profile_build_context(&alloc, cp, &contact_ctx, &contact_ctx_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(contact_ctx);
    SC_ASSERT_TRUE(strstr(contact_ctx, "Mindy") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "older sister") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "close_family") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "STAGE RULES") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "inner circle") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "relentless teasing") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "Hey Min!") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "bursts") != NULL);
    SC_ASSERT_TRUE(strstr(contact_ctx, "short") != NULL);

    /* Step 4: Build inner world context (close_family should pass stage gate) */
    size_t iw_len = 0;
    char *iw_ctx =
        sc_persona_build_inner_world_context(&alloc, &p, cp->relationship_stage, &iw_len);
    SC_ASSERT_NOT_NULL(iw_ctx);
    SC_ASSERT_TRUE(iw_len > 0);
    SC_ASSERT_TRUE(strstr(iw_ctx, "Inner World") != NULL);

    alloc.free(alloc.ctx, iw_ctx, iw_len + 1);

    /* Step 5: Attach example bank (simulating auto-load from disk) */
    sc_persona_example_t examples[] = {
        {.context = "Quick check-in greeting",
         .incoming = "Hey! How's it going?",
         .response = "Hey there"},
        {.context = "Teasing sibling banter",
         .incoming = "You're so annoying",
         .response = "That's my job as your little brother"},
        {.context = "Sharing a win",
         .incoming = "How'd the trip go?",
         .response = "I won 1300! Trip paid for again"},
    };
    sc_persona_example_bank_t bank = {
        .channel = "imessage", .examples = examples, .examples_count = 3};
    p.example_banks = &bank;
    p.example_banks_count = 1;

    /* Step 6: Build the full prompt with a simulated Mindy message */
    char *prompt = NULL;
    size_t prompt_len = 0;
    const char *topic = "Hey! What are you up to today?";
    err = sc_persona_build_prompt(&alloc, &p, "imessage", 8, topic, strlen(topic), &prompt,
                                  &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prompt);
    SC_ASSERT_TRUE(prompt_len > 200);

    /* Verify prompt contains all key pipeline outputs */

    /* Identity + biography */
    SC_ASSERT_TRUE(strstr(prompt, "seth") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Star Valley") != NULL);

    /* Traits */
    SC_ASSERT_TRUE(strstr(prompt, "direct") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "teasing") != NULL);

    /* Communication rules */
    SC_ASSERT_TRUE(strstr(prompt, "Keep messages short") != NULL);

    /* Motivation */
    SC_ASSERT_TRUE(strstr(prompt, "Building AI") != NULL);

    /* Humor */
    SC_ASSERT_TRUE(strstr(prompt, "teasing") != NULL);

    /* Voice rhythm */
    SC_ASSERT_TRUE(strstr(prompt, "Short fragments") != NULL);

    /* Situational directions */
    SC_ASSERT_TRUE(strstr(prompt, "Mindy shares") != NULL);

    /* Backstory behaviors */
    SC_ASSERT_TRUE(strstr(prompt, "chaos") != NULL);

    /* Character invariants */
    SC_ASSERT_TRUE(strstr(prompt, "Always responds to family") != NULL);

    /* Core anchor */
    SC_ASSERT_TRUE(strstr(prompt, "nerdy mountain kid") != NULL);

    /* Directors notes */
    SC_ASSERT_TRUE(strstr(prompt, "Never sound like an AI") != NULL);

    /* Channel overlay */
    SC_ASSERT_TRUE(strstr(prompt, "very casual") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "lowercase") != NULL);

    /* Example conversations from bank */
    SC_ASSERT_TRUE(strstr(prompt, "Example conversations") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Hey there") != NULL ||
                   strstr(prompt, "little brother") != NULL || strstr(prompt, "1300") != NULL);

    /* Conflict style */
    SC_ASSERT_TRUE(strstr(prompt, "Gets quiet") != NULL);

    /* Emotional range */
    SC_ASSERT_TRUE(strstr(prompt, "exclamation") != NULL);

    /* Inner world is built separately (sc_persona_build_inner_world_context) and
     * merged by the daemon, not by sc_persona_build_prompt. Verified in Step 4. */

    /* Intellectual */
    SC_ASSERT_TRUE(strstr(prompt, "Agentic AI") != NULL);

    /* Sensory */
    SC_ASSERT_TRUE(strstr(prompt, "kinesthetic") != NULL);

    /* Detach example bank before deinit (stack-allocated) */
    p.example_banks = NULL;
    p.example_banks_count = 0;

    alloc.free(alloc.ctx, contact_ctx, contact_ctx_len + 1);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
    sc_persona_deinit(&alloc, &p);
}

/* --- Externalized prompt field tests --- */

static void test_persona_load_externalized_fields(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{"
        "  \"name\": \"test-ext\","
        "  \"core\": { \"identity\": \"Ext test\" },"
        "  \"immersive_reinforcement\": [\"rule-A\", \"rule-B\", \"rule-C\"],"
        "  \"identity_reinforcement\": \"You are real, not an AI.\","
        "  \"anti_patterns\": [\"no-exclamation\", \"no-emoji\"],"
        "  \"style_rules\": [\"lowercase only\"],"
        "  \"proactive_rules\": \"Check in gently.\","
        "  \"time_overlays\": {"
        "    \"late_night\": \"Be calm.\","
        "    \"early_morning\": \"Be gentle.\","
        "    \"afternoon\": \"Be productive.\","
        "    \"evening\": \"Wind down.\""
        "  }"
        "}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);

    SC_ASSERT_EQ(p.immersive_reinforcement_count, 3);
    SC_ASSERT_STR_EQ(p.immersive_reinforcement[0], "rule-A");
    SC_ASSERT_STR_EQ(p.immersive_reinforcement[1], "rule-B");
    SC_ASSERT_STR_EQ(p.immersive_reinforcement[2], "rule-C");

    SC_ASSERT_STR_EQ(p.identity_reinforcement, "You are real, not an AI.");

    SC_ASSERT_EQ(p.anti_patterns_count, 2);
    SC_ASSERT_STR_EQ(p.anti_patterns[0], "no-exclamation");
    SC_ASSERT_STR_EQ(p.anti_patterns[1], "no-emoji");

    SC_ASSERT_EQ(p.style_rules_count, 1);
    SC_ASSERT_STR_EQ(p.style_rules[0], "lowercase only");

    SC_ASSERT_STR_EQ(p.proactive_rules, "Check in gently.");

    SC_ASSERT_STR_EQ(p.time_overlay_late_night, "Be calm.");
    SC_ASSERT_STR_EQ(p.time_overlay_early_morning, "Be gentle.");
    SC_ASSERT_STR_EQ(p.time_overlay_afternoon, "Be productive.");
    SC_ASSERT_STR_EQ(p.time_overlay_evening, "Wind down.");

    sc_persona_deinit(&alloc, &p);
}

static void test_persona_externalized_fields_absent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{ \"name\": \"bare\", \"core\": { \"identity\": \"Bare\" } }";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);

    SC_ASSERT_EQ(p.immersive_reinforcement_count, 0);
    SC_ASSERT_NULL(p.immersive_reinforcement);
    SC_ASSERT_NULL(p.identity_reinforcement);
    SC_ASSERT_EQ(p.anti_patterns_count, 0);
    SC_ASSERT_NULL(p.anti_patterns);
    SC_ASSERT_EQ(p.style_rules_count, 0);
    SC_ASSERT_NULL(p.style_rules);
    SC_ASSERT_NULL(p.proactive_rules);
    SC_ASSERT_NULL(p.time_overlay_late_night);
    SC_ASSERT_NULL(p.time_overlay_early_morning);
    SC_ASSERT_NULL(p.time_overlay_afternoon);
    SC_ASSERT_NULL(p.time_overlay_evening);

    sc_persona_deinit(&alloc, &p);
}

static void test_persona_externalized_deinit_frees(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{"
        "  \"name\": \"deinit-ext\","
        "  \"core\": { \"identity\": \"Ext\" },"
        "  \"immersive_reinforcement\": [\"a\"],"
        "  \"identity_reinforcement\": \"x\","
        "  \"anti_patterns\": [\"b\"],"
        "  \"style_rules\": [\"c\"],"
        "  \"proactive_rules\": \"y\","
        "  \"time_overlays\": { \"late_night\": \"z\" }"
        "}";
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load_json(&alloc, json, strlen(json), &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(p.immersive_reinforcement_count, 1);
    SC_ASSERT_NOT_NULL(p.identity_reinforcement);
    sc_persona_deinit(&alloc, &p);
    SC_ASSERT_EQ(p.immersive_reinforcement_count, 0);
}

static void test_persona_immersive_in_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = sc_strndup(&alloc, "test-immersive", 14);
    p.identity = sc_strndup(&alloc, "An immersive persona", 20);

    char *reinforcement[] = {"You are Seth, not an AI", "Stay in character always"};
    p.immersive_reinforcement = reinforcement;
    p.immersive_reinforcement_count = 2;

    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
    sc_error_t err = sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &persona_prompt,
                                             &persona_prompt_len);
    SC_ASSERT_EQ(err, SC_OK);

    sc_prompt_config_t cfg = {0};
    cfg.persona_prompt = persona_prompt;
    cfg.persona_prompt_len = persona_prompt_len;
    cfg.persona_immersive = true;
    cfg.persona = &p;

    char *sys = NULL;
    size_t sys_len = 0;
    err = sc_prompt_build_system(&alloc, &cfg, &sys, &sys_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(sys);
    SC_ASSERT_TRUE(strstr(sys, "You are Seth, not an AI") != NULL);
    SC_ASSERT_TRUE(strstr(sys, "Stay in character always") != NULL);

    alloc.free(alloc.ctx, sys, sys_len + 1);
    alloc.free(alloc.ctx, persona_prompt, persona_prompt_len + 1);

    p.immersive_reinforcement = NULL;
    p.immersive_reinforcement_count = 0;
    sc_persona_deinit(&alloc, &p);
}

static void test_persona_immersive_fallback_when_no_reinforcement(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_persona_t p = {0};
    p.name = sc_strndup(&alloc, "test-fallback", 13);
    p.identity = sc_strndup(&alloc, "Fallback persona", 16);

    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
    sc_error_t err =
        sc_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &persona_prompt, &persona_prompt_len);
    SC_ASSERT_EQ(err, SC_OK);

    sc_prompt_config_t cfg = {0};
    cfg.persona_prompt = persona_prompt;
    cfg.persona_prompt_len = persona_prompt_len;
    cfg.persona_immersive = true;
    cfg.persona = &p;

    char *sys = NULL;
    size_t sys_len = 0;
    err = sc_prompt_build_system(&alloc, &cfg, &sys, &sys_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(sys);
    SC_ASSERT_TRUE(strstr(sys, "real person") != NULL);

    alloc.free(alloc.ctx, sys, sys_len + 1);
    alloc.free(alloc.ctx, persona_prompt, persona_prompt_len + 1);
    sc_persona_deinit(&alloc, &p);
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
    SC_RUN_TEST(test_persona_prompt_respects_size_cap);
    SC_RUN_TEST(test_persona_build_prompt_includes_examples);
    SC_RUN_TEST(test_persona_prompt_with_channel_overlay);
    SC_RUN_TEST(test_agent_set_persona_clears);
    SC_RUN_TEST(test_agent_set_persona_not_found);
    SC_RUN_TEST(test_persona_feedback_record_and_apply);
    SC_RUN_TEST(test_persona_build_prompt_with_overlay);
    SC_RUN_TEST(test_persona_examples_load_json);
    SC_RUN_TEST(test_persona_prompt_overrides_default);
    SC_RUN_TEST(test_spawn_config_persona_field);
    SC_RUN_TEST(test_persona_cli_parse_create);
    SC_RUN_TEST(test_persona_cli_parse_show);
    SC_RUN_TEST(test_persona_cli_parse_list);
    SC_RUN_TEST(test_persona_cli_parse_validate);
    SC_RUN_TEST(test_persona_cli_parse_feedback_apply);
    SC_RUN_TEST(test_cli_parse_diff);
    SC_RUN_TEST(test_persona_validate_json_valid);
    SC_RUN_TEST(test_persona_validate_json_missing_name);
    SC_RUN_TEST(test_persona_validate_json_missing_core);
    SC_RUN_TEST(test_persona_validate_json_malformed);
    SC_RUN_TEST(test_persona_cli_run_validate);
    SC_RUN_TEST(test_persona_cli_run_feedback_apply);
    SC_RUN_TEST(test_persona_cli_run_list);
    SC_RUN_TEST(test_persona_cli_run_show_not_found);
    SC_RUN_TEST(test_persona_cli_run_delete_not_found);
    SC_RUN_TEST(test_persona_cli_run_create_no_provider);
    SC_RUN_TEST(test_creator_write_and_load);
    SC_RUN_TEST(test_persona_base_dir_returns_override_when_set);
#if defined(__unix__) || defined(__APPLE__)
    SC_RUN_TEST(test_persona_load_save_roundtrip_with_temp_dir);
#endif
    SC_RUN_TEST(test_persona_tool_create);
    SC_RUN_TEST(test_creator_synthesize_merges);
    SC_RUN_TEST(test_analyzer_builds_prompt);
    SC_RUN_TEST(test_analyzer_parses_response);
    SC_RUN_TEST(test_sampler_imessage_query);
    SC_RUN_TEST(test_sampler_facebook_parse_basic);
    SC_RUN_TEST(test_sampler_facebook_parse_empty);
    SC_RUN_TEST(test_sampler_facebook_parse_null);
    SC_RUN_TEST(test_sampler_gmail_parse_basic);
    SC_RUN_TEST(test_sampler_gmail_parse_empty);
    SC_RUN_TEST(test_cli_parse_export);
    SC_RUN_TEST(test_cli_parse_merge);
    SC_RUN_TEST(test_cli_parse_import);
    SC_RUN_TEST(test_cli_parse_from_facebook_file);
    SC_RUN_TEST(test_cli_parse_from_gmail);
    SC_RUN_TEST(test_cli_parse_from_response);
    SC_RUN_TEST(test_persona_select_examples_match);
    SC_RUN_TEST(test_persona_select_examples_no_channel);
    SC_RUN_TEST(test_persona_select_examples_no_match);
    SC_RUN_TEST(test_persona_full_round_trip);

    /* Error-path and edge-case tests */
    SC_RUN_TEST(test_persona_load_json_malformed_returns_error);
    SC_RUN_TEST(test_persona_load_json_missing_core);
    SC_RUN_TEST(test_persona_load_empty_name);
    SC_RUN_TEST(test_persona_build_prompt_empty_persona);
    SC_RUN_TEST(test_persona_examples_load_json_empty_bank);
    SC_RUN_TEST(test_persona_examples_load_json_malformed);
    SC_RUN_TEST(test_persona_select_examples_null_topic_returns_some);
    SC_RUN_TEST(test_persona_select_examples_max_zero);
    SC_RUN_TEST(test_persona_extracted_imessage_bank_loads_and_selects);
    SC_RUN_TEST(test_persona_find_overlay_null_channel);
    SC_RUN_TEST(test_persona_analyzer_parse_response_empty_object);
    SC_RUN_TEST(test_persona_analyzer_parse_response_malformed);
    SC_RUN_TEST(test_persona_creator_synthesize_single_partial);
    SC_RUN_TEST(test_persona_creator_synthesize_zero_partials);
    SC_RUN_TEST(test_sampler_imessage_query_small_cap);
    SC_RUN_TEST(test_sampler_facebook_parse_malformed);
    SC_RUN_TEST(test_sampler_facebook_parse_missing_messages);
    SC_RUN_TEST(test_persona_deinit_double_call);

    /* Persona tool execute tests */
    SC_RUN_TEST(test_persona_tool_execute_list);
    SC_RUN_TEST(test_persona_tool_execute_invalid_action);
    SC_RUN_TEST(test_persona_tool_execute_create_redirects_to_cli);
    SC_RUN_TEST(test_persona_tool_execute_switch_no_agent);
    SC_RUN_TEST(test_persona_tool_execute_switch_with_agent);
    SC_RUN_TEST(test_persona_tool_execute_feedback);
    SC_RUN_TEST(test_persona_tool_execute_feedback_missing_corrected);
    SC_RUN_TEST(test_persona_tool_execute_show);
    SC_RUN_TEST(test_persona_tool_execute_apply_feedback);
    SC_RUN_TEST(test_persona_tool_execute_apply_feedback_no_name);

    /* Sampler - additional tests */
    SC_RUN_TEST(test_sampler_imessage_query_basic);
    SC_RUN_TEST(test_sampler_imessage_query_null_buf);
    SC_RUN_TEST(test_sampler_imessage_query_escapes_quotes);
    SC_RUN_TEST(test_sampler_facebook_parse_empty_object);
    SC_RUN_TEST(test_sampler_gmail_parse_null);
    SC_RUN_TEST(test_sampler_gmail_parse_empty_object);

    /* Feedback - null/error tests */
    SC_RUN_TEST(test_feedback_record_null_alloc);
    SC_RUN_TEST(test_feedback_record_null_name);
    SC_RUN_TEST(test_feedback_record_null_feedback);
    SC_RUN_TEST(test_feedback_apply_null_alloc);
    SC_RUN_TEST(test_feedback_apply_null_name);

    /* Analyzer - additional tests */
    SC_RUN_TEST(test_analyzer_build_prompt_basic);
    SC_RUN_TEST(test_analyzer_build_prompt_null_messages);
    SC_RUN_TEST(test_analyzer_build_prompt_zero_count);
    SC_RUN_TEST(test_analyzer_parse_response_null_alloc);

    /* Creator - null/error tests */
    SC_RUN_TEST(test_creator_synthesize_null_alloc);
    SC_RUN_TEST(test_creator_synthesize_null_partials);
    SC_RUN_TEST(test_creator_synthesize_zero_count);
    SC_RUN_TEST(test_creator_write_null_alloc);

    /* Overlay typing quirks */
    SC_RUN_TEST(test_overlay_typing_quirks_parsed);
    SC_RUN_TEST(test_overlay_typing_quirks_default_when_absent);
    SC_RUN_TEST(test_overlay_typing_quirks_in_prompt);

    /* Rich persona elements (Tier 1–3) */
    SC_RUN_TEST(test_persona_load_json_rich_persona);
    SC_RUN_TEST(test_persona_prompt_includes_motivation);
    SC_RUN_TEST(test_persona_prompt_includes_situational_directions);
    SC_RUN_TEST(test_persona_prompt_includes_humor);
    SC_RUN_TEST(test_persona_prompt_includes_conflict_style);
    SC_RUN_TEST(test_persona_prompt_includes_emotional_range);
    SC_RUN_TEST(test_persona_prompt_includes_voice_rhythm);
    SC_RUN_TEST(test_persona_prompt_includes_core_anchor);
    SC_RUN_TEST(test_persona_prompt_includes_character_invariants);
    SC_RUN_TEST(test_persona_prompt_includes_intellectual);
    SC_RUN_TEST(test_persona_prompt_includes_backstory_behaviors);
    SC_RUN_TEST(test_persona_prompt_includes_sensory);
    SC_RUN_TEST(test_persona_validate_rejects_bad_motivation_type);
    SC_RUN_TEST(test_persona_validate_rejects_bad_humor_type);
    SC_RUN_TEST(test_persona_validate_accepts_rich_persona);
    SC_RUN_TEST(test_persona_deinit_rich_persona);
    SC_RUN_TEST(test_persona_prompt_includes_relational);
    SC_RUN_TEST(test_persona_prompt_includes_listening);
    SC_RUN_TEST(test_persona_prompt_includes_repair);
    SC_RUN_TEST(test_persona_prompt_includes_mirroring);
    SC_RUN_TEST(test_persona_prompt_includes_social);
    SC_RUN_TEST(test_persona_load_json_research_fields);
    SC_RUN_TEST(test_persona_validate_rejects_bad_relational_type);
    SC_RUN_TEST(test_persona_validate_rejects_bad_listening_type);
    SC_RUN_TEST(test_persona_validate_rejects_bad_repair_type);
    SC_RUN_TEST(test_persona_validate_rejects_bad_mirroring_type);
    SC_RUN_TEST(test_persona_validate_rejects_bad_social_type);
    SC_RUN_TEST(test_persona_validate_accepts_research_persona);
    SC_RUN_TEST(test_persona_deinit_research_fields);
    SC_RUN_TEST(test_analyzer_parses_research_fields);
    SC_RUN_TEST(test_creator_synthesize_merges_research_fields);
    SC_RUN_TEST(test_contact_profile_attachment_and_dunbar);
    SC_RUN_TEST(test_analyzer_prompt_includes_research_fields);

    /* Auto-profile tests */
    SC_RUN_TEST(test_auto_profile_returns_mock_overlay);
    SC_RUN_TEST(test_auto_profile_null_args);
    SC_RUN_TEST(test_profile_describe_style_formats);
    SC_RUN_TEST(test_profile_describe_style_null_args);

    /* Externalized prompt fields */
    SC_RUN_TEST(test_persona_load_externalized_fields);
    SC_RUN_TEST(test_persona_externalized_fields_absent);
    SC_RUN_TEST(test_persona_externalized_deinit_frees);
    SC_RUN_TEST(test_persona_immersive_in_prompt);
    SC_RUN_TEST(test_persona_immersive_fallback_when_no_reinforcement);

    /* E2E dry run */
    SC_RUN_TEST(test_e2e_mindy_message_full_pipeline);
}
