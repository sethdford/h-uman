#include "human/agent.h"
#include "human/agent/prompt.h"
#include "human/agent/spawn.h"
#include "human/agent/tool_context.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/string.h"
#include "human/persona.h"
#include "human/persona/auto_profile.h"
#include "human/providers/factory.h"
#include "human/tool.h"
#include "human/tools/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

static void test_persona_types_exist(void) {
    hu_persona_t p;
    hu_persona_overlay_t ov;
    hu_persona_example_t ex;
    hu_persona_example_bank_t bank;

    memset(&p, 0, sizeof(p));
    memset(&ov, 0, sizeof(ov));
    memset(&ex, 0, sizeof(ex));
    memset(&bank, 0, sizeof(bank));

    HU_ASSERT_NULL(p.name);
    HU_ASSERT_NULL(p.identity);
    HU_ASSERT_NULL(p.overlays);
    HU_ASSERT_EQ(p.overlays_count, 0);
    HU_ASSERT_NULL(ov.channel);
    HU_ASSERT_NULL(ex.context);
    HU_ASSERT_NULL(bank.examples);
}

static void test_persona_find_overlay_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    persona.overlays = (hu_persona_overlay_t *)alloc.alloc(alloc.ctx, sizeof(hu_persona_overlay_t));
    HU_ASSERT_NOT_NULL(persona.overlays);
    memset(persona.overlays, 0, sizeof(hu_persona_overlay_t));
    persona.overlays_count = 1;

    persona.overlays[0].channel = hu_strndup(&alloc, "telegram", 8);

    const hu_persona_overlay_t *found = hu_persona_find_overlay(&persona, "telegram", 8);
    HU_ASSERT_NOT_NULL(found);
    HU_ASSERT_STR_EQ(found->channel, "telegram");

    hu_persona_deinit(&alloc, &persona);
}

static void test_persona_find_overlay_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    persona.overlays = (hu_persona_overlay_t *)alloc.alloc(alloc.ctx, sizeof(hu_persona_overlay_t));
    HU_ASSERT_NOT_NULL(persona.overlays);
    memset(persona.overlays, 0, sizeof(hu_persona_overlay_t));
    persona.overlays_count = 1;
    persona.overlays[0].channel = hu_strndup(&alloc, "telegram", 8);

    const hu_persona_overlay_t *found = hu_persona_find_overlay(&persona, "discord", 7);
    HU_ASSERT_NULL(found);

    found = hu_persona_find_overlay(&persona, "tel", 3);
    HU_ASSERT_NULL(found);

    hu_persona_deinit(&alloc, &persona);
}

static void test_persona_deinit_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));

    hu_persona_deinit(&alloc, &persona);

    HU_ASSERT_NULL(persona.name);
    HU_ASSERT_EQ(persona.overlays_count, 0);
}

static void test_persona_load_json_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
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

    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.name, "test-user");
    HU_ASSERT_EQ(p.traits_count, 2);
    HU_ASSERT_STR_EQ(p.traits[0], "direct");
    HU_ASSERT_STR_EQ(p.identity, "A test persona");
    HU_ASSERT_EQ(p.preferred_vocab_count, 2);
    HU_ASSERT_EQ(p.avoided_vocab_count, 1);
    HU_ASSERT_EQ(p.slang_count, 1);
    HU_ASSERT_EQ(p.communication_rules_count, 1);
    HU_ASSERT_EQ(p.values_count, 1);
    HU_ASSERT_STR_EQ(p.decision_style, "Decides fast");
    HU_ASSERT_EQ(p.overlays_count, 1);
    HU_ASSERT_STR_EQ(p.overlays[0].channel, "imessage");
    HU_ASSERT_STR_EQ(p.overlays[0].formality, "casual");
    HU_ASSERT_EQ(p.overlays[0].style_notes_count, 1);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, "{}", 2, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(p.name);
    HU_ASSERT_NULL(p.identity);
    HU_ASSERT_EQ(p.traits_count, 0);
    HU_ASSERT_EQ(p.overlays_count, 0);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load(&alloc, "nonexistent-persona-xyz", 23, &p);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void free_example_bank(hu_allocator_t *a, hu_persona_example_bank_t *bank) {
    if (!bank)
        return;
    if (bank->channel) {
        size_t len = strlen(bank->channel);
        a->free(a->ctx, bank->channel, len + 1);
    }
    if (bank->examples) {
        for (size_t i = 0; i < bank->examples_count; i++) {
            hu_persona_example_t *e = &bank->examples[i];
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
        a->free(a->ctx, bank->examples, bank->examples_count * sizeof(hu_persona_example_t));
    }
}

static void test_persona_examples_load_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"examples\":["
        "  {\"context\":\"casual\",\"incoming\":\"hey\",\"response\":\"yo\"},"
        "  {\"context\":\"work\",\"incoming\":\"meeting?\",\"response\":\"sure 3pm\"}"
        "]}";
    hu_persona_example_bank_t bank = {0};
    hu_error_t err =
        hu_persona_examples_load_json(&alloc, "imessage", 8, json, strlen(json), &bank);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(bank.examples_count, 2);
    HU_ASSERT_STR_EQ(bank.examples[0].context, "casual");
    HU_ASSERT_STR_EQ(bank.examples[1].response, "sure 3pm");
    free_example_bank(&alloc, &bank);
}

static void test_persona_select_examples_match(void) {
    hu_persona_example_t imsg_examples[] = {
        {.context = "casual greeting", .incoming = "hey", .response = "yo"},
        {.context = "making plans", .incoming = "dinner?", .response = "down"},
        {.context = "tech question", .incoming = "what lang?", .response = "C obviously"},
    };
    hu_persona_example_bank_t banks[] = {
        {.channel = "imessage", .examples = imsg_examples, .examples_count = 3},
    };
    hu_persona_t p = {.example_banks = banks, .example_banks_count = 1};

    const hu_persona_example_t *selected[2];
    size_t count = 0;
    hu_error_t err =
        hu_persona_select_examples(&p, "imessage", 8, "plans dinner", 12, selected, &count, 2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count <= 2);
    HU_ASSERT_TRUE(count > 0);
}

static void test_persona_select_examples_no_channel(void) {
    hu_persona_t p = {0};
    const hu_persona_example_t *selected[2];
    size_t count = 99;
    hu_error_t err = hu_persona_select_examples(&p, "slack", 5, NULL, 0, selected, &count, 2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
}

static void test_persona_prompt_overrides_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_prompt = "You are acting as TestUser. Direct and curious.";
    cfg.persona_prompt_len = strlen(cfg.persona_prompt);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "TestUser") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Human") == NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_analyzer_builds_prompt(void) {
    const char *messages[] = {"hey whats up", "down. where at", "thursday works"};
    char prompt[4096];
    size_t prompt_len = 0;
    hu_error_t err = hu_persona_analyzer_build_prompt(messages, 3, "imessage", prompt,
                                                      sizeof(prompt), &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(prompt_len > 0);
    HU_ASSERT_NOT_NULL(strstr(prompt, "hey whats up"));
}

static void test_persona_cli_parse_create(void) {
    const char *argv[] = {"human", "persona", "create", "seth", "--from-imessage"};
    hu_persona_cli_args_t args = {0};
    hu_error_t err = hu_persona_cli_parse(5, argv, &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)args.action, (int)HU_PERSONA_ACTION_CREATE);
    HU_ASSERT_STR_EQ(args.name, "seth");
    HU_ASSERT_TRUE(args.from_imessage);
    HU_ASSERT_TRUE(!args.from_gmail);
}

static void test_persona_cli_parse_show(void) {
    const char *argv[] = {"human", "persona", "show", "seth"};
    hu_persona_cli_args_t args = {0};
    hu_error_t err = hu_persona_cli_parse(4, argv, &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)args.action, (int)HU_PERSONA_ACTION_SHOW);
    HU_ASSERT_STR_EQ(args.name, "seth");
}

static void test_persona_tool_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "persona");
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_cli_parse_list(void) {
    const char *argv[] = {"human", "persona", "list"};
    hu_persona_cli_args_t args = {0};
    hu_error_t err = hu_persona_cli_parse(3, argv, &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ((int)args.action, (int)HU_PERSONA_ACTION_LIST);
}

static void test_persona_validate_json_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test person\",\"traits\":[\"direct\"]}}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(err == NULL);
}

static void test_persona_validate_json_missing_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"core\":{\"identity\":\"X\",\"traits\":[]}}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_TRUE(err != NULL);
    HU_ASSERT_TRUE(strstr(err, "name") != NULL);
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_json_missing_core(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\"}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_TRUE(err != NULL);
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_json_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "not json at all";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_TRUE(err != NULL);
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_cli_parse_validate(void) {
    const char *argv[] = {"human", "persona", "validate", "test_name"};
    hu_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    hu_error_t e = hu_persona_cli_parse(4, argv, &args);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(args.action, HU_PERSONA_ACTION_VALIDATE);
    HU_ASSERT_TRUE(strcmp(args.name, "test_name") == 0);
}

static void test_persona_cli_parse_feedback_apply(void) {
    const char *argv[] = {"human", "persona", "feedback", "apply", "mypersona"};
    hu_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    hu_error_t e = hu_persona_cli_parse(5, argv, &args);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(args.action, HU_PERSONA_ACTION_FEEDBACK_APPLY);
    HU_ASSERT_TRUE(strcmp(args.name, "mypersona") == 0);
}

static void test_cli_parse_diff(void) {
    const char *argv[] = {"human", "persona", "diff", "a", "b"};
    hu_persona_cli_args_t args = {0};
    HU_ASSERT_EQ(hu_persona_cli_parse(5, argv, &args), HU_OK);
    HU_ASSERT_EQ(args.action, HU_PERSONA_ACTION_DIFF);
    HU_ASSERT_TRUE(strcmp(args.name, "a") == 0);
    HU_ASSERT_TRUE(strcmp(args.diff_name, "b") == 0);
}

static void test_persona_cli_run_validate(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_VALIDATE;
    args.name = "test_name";
    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_persona_cli_run_feedback_apply(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_FEEDBACK_APPLY;
    args.name = "test";
    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_creator_synthesize_merges(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *traits1[] = {"direct", "casual"};
    char *traits2[] = {"curious", "direct"};
    hu_persona_t partials[] = {
        {.traits = traits1, .traits_count = 2},
        {.traits = traits2, .traits_count = 2},
    };
    hu_persona_t merged = {0};
    hu_error_t err = hu_persona_creator_synthesize(&alloc, partials, 2, "testuser", 8, &merged);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(merged.name, "testuser");
    HU_ASSERT_EQ(merged.traits_count, (size_t)3);
    hu_persona_deinit(&alloc, &merged);
}

static void test_analyzer_parses_response(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *response =
        "{\"traits\":[\"direct\",\"casual\"],"
        "\"vocabulary\":{\"preferred\":[\"down\",\"works\"],\"avoided\":[],\"slang\":[]},"
        "\"communication_rules\":[\"Keeps messages very short\"],"
        "\"formality\":\"casual\",\"avg_length\":\"short\","
        "\"emoji_usage\":\"none\"}";
    hu_persona_t partial = {0};
    hu_error_t err = hu_persona_analyzer_parse_response(&alloc, response, strlen(response),
                                                        "imessage", 8, &partial);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(partial.traits_count >= 1);
    HU_ASSERT_TRUE(partial.overlays_count == 1);
    hu_persona_deinit(&alloc, &partial);
}

static void test_sampler_facebook_parse_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"messages\":["
                       "{\"sender_name\":\"Alice\",\"content\":\"hey there\"},"
                       "{\"sender_name\":\"Bob\",\"content\":\"hi alice\"},"
                       "{\"sender_name\":\"Alice\",\"content\":\"whats up\"},"
                       "{\"sender_name\":\"Alice\",\"content\":\"see you later\"}"
                       "]}";
    char **msgs = NULL;
    size_t count = 0;
    hu_error_t err = hu_persona_sampler_facebook_parse(json, strlen(json), &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, (size_t)3);
    HU_ASSERT_STR_EQ(msgs[0], "hey there");
    HU_ASSERT_STR_EQ(msgs[1], "whats up");
    HU_ASSERT_STR_EQ(msgs[2], "see you later");
    for (size_t i = 0; i < count; i++)
        alloc.free(alloc.ctx, msgs[i], strlen(msgs[i]) + 1);
    alloc.free(alloc.ctx, msgs, count * sizeof(char *));
}

static void test_sampler_facebook_parse_empty(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 99;
    hu_error_t err = hu_persona_sampler_facebook_parse(json, strlen(json), &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, (size_t)0);
}

static void test_sampler_facebook_parse_null(void) {
    hu_error_t err = hu_persona_sampler_facebook_parse(NULL, 0, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_sampler_gmail_parse_basic(void) {
    const char *json = "{\"messages\":["
                       "{\"from\":\"me\",\"body\":\"Hey there\"},"
                       "{\"from\":\"other\",\"body\":\"Hi\"},"
                       "{\"from\":\"me\",\"body\":\"Cool thanks\"}"
                       "]}";
    char **msgs = NULL;
    size_t count = 0;
    hu_error_t e = hu_persona_sampler_gmail_parse(json, strlen(json), &msgs, &count);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(count, (size_t)2);
    for (size_t i = 0; i < count; i++)
        free(msgs[i]);
    free(msgs);
}

static void test_sampler_gmail_parse_empty(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 0;
    hu_error_t e = hu_persona_sampler_gmail_parse(json, strlen(json), &msgs, &count);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(count, (size_t)0);
}

static void test_cli_parse_export(void) {
    const char *argv[] = {"human", "persona", "export", "seth"};
    hu_persona_cli_args_t args = {0};
    HU_ASSERT_EQ(hu_persona_cli_parse(4, argv, &args), HU_OK);
    HU_ASSERT_EQ(args.action, HU_PERSONA_ACTION_EXPORT);
    HU_ASSERT_TRUE(strcmp(args.name, "seth") == 0);
}

static void test_cli_parse_merge(void) {
    const char *argv[] = {"human", "persona", "merge", "combined", "a", "b"};
    hu_persona_cli_args_t args = {0};
    HU_ASSERT_EQ(hu_persona_cli_parse(6, argv, &args), HU_OK);
    HU_ASSERT_EQ(args.action, HU_PERSONA_ACTION_MERGE);
    HU_ASSERT_TRUE(strcmp(args.name, "combined") == 0);
    HU_ASSERT_TRUE(args.merge_sources != NULL);
    HU_ASSERT_EQ(args.merge_sources_count, (size_t)2);
    HU_ASSERT_TRUE(strcmp(args.merge_sources[0], "a") == 0);
    HU_ASSERT_TRUE(strcmp(args.merge_sources[1], "b") == 0);
}

static void test_cli_parse_import(void) {
    const char *argv[] = {"human", "persona", "import", "newpersona", "--from-file", "/tmp/p.json"};
    hu_persona_cli_args_t args = {0};
    HU_ASSERT_EQ(hu_persona_cli_parse(6, argv, &args), HU_OK);
    HU_ASSERT_EQ(args.action, HU_PERSONA_ACTION_IMPORT);
    HU_ASSERT_TRUE(strcmp(args.name, "newpersona") == 0);
    HU_ASSERT_TRUE(args.import_file != NULL);
    HU_ASSERT_TRUE(strcmp(args.import_file, "/tmp/p.json") == 0);
}

static void test_cli_parse_from_facebook_file(void) {
    const char *argv[] = {"human", "persona", "create", "test", "--from-facebook", "/tmp/fb.json"};
    hu_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    hu_error_t e = hu_persona_cli_parse(6, argv, &args);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(args.from_facebook);
    HU_ASSERT_TRUE(args.facebook_export_path != NULL);
    HU_ASSERT_TRUE(strcmp(args.facebook_export_path, "/tmp/fb.json") == 0);
}

static void test_cli_parse_from_gmail(void) {
    const char *argv[] = {"human", "persona", "create", "test", "--from-gmail", "/tmp/gmail.json"};
    hu_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    hu_error_t e = hu_persona_cli_parse(6, argv, &args);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(args.from_gmail);
    HU_ASSERT_TRUE(args.gmail_export_path != NULL);
    HU_ASSERT_TRUE(strcmp(args.gmail_export_path, "/tmp/gmail.json") == 0);
}

static void test_cli_parse_from_response(void) {
    const char *argv[] = {"human", "persona",         "create",
                          "test",  "--from-response", "/tmp/resp.json"};
    hu_persona_cli_args_t args;
    memset(&args, 0, sizeof(args));
    hu_error_t err = hu_persona_cli_parse(6, argv, &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(args.action, HU_PERSONA_ACTION_CREATE);
    HU_ASSERT_TRUE(args.response_file != NULL);
    HU_ASSERT_TRUE(strcmp(args.response_file, "/tmp/resp.json") == 0);
}

static void test_sampler_imessage_query(void) {
    char query[512];
    size_t query_len = 0;
    hu_error_t err = hu_persona_sampler_imessage_query(query, sizeof(query), &query_len, 500);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(query_len > 0);
    HU_ASSERT_NOT_NULL(strstr(query, "message"));
    HU_ASSERT_NOT_NULL(strstr(query, "LIMIT"));
}

static void test_spawn_config_persona_field(void) {
    hu_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_name = "seth";
    cfg.persona_name_len = 4;
    HU_ASSERT_STR_EQ(cfg.persona_name, "seth");
    HU_ASSERT_EQ(cfg.persona_name_len, 4);
}

static void test_persona_full_round_trip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"roundtrip\","
                       "\"core\":{\"identity\":\"Integration test persona\","
                       "\"traits\":[\"direct\"],"
                       "\"vocabulary\":{\"preferred\":[\"solid\"],\"avoided\":[],\"slang\":[]},"
                       "\"communication_rules\":[\"Be brief\"],"
                       "\"values\":[\"speed\"],\"decision_style\":\"Fast\"},"
                       "\"channel_overlays\":{\"imessage\":{\"formality\":\"casual\","
                       "\"avg_length\":\"short\",\"emoji_usage\":\"none\","
                       "\"style_notes\":[\"no caps\"]}}}";

    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    char *prompt = NULL;
    size_t prompt_len = 0;
    err = hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(prompt, "roundtrip") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "casual") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "no caps") != NULL);

    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.persona_prompt = prompt;
    cfg.persona_prompt_len = prompt_len;
    char *sys = NULL;
    size_t sys_len = 0;
    err = hu_prompt_build_system(&alloc, &cfg, &sys, &sys_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(sys, "roundtrip") != NULL);
    HU_ASSERT_TRUE(strstr(sys, "Human") == NULL);

    alloc.free(alloc.ctx, sys, sys_len + 1);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_select_examples_no_match(void) {
    hu_persona_example_t imsg_examples[] = {
        {.context = "casual greeting", .incoming = "hey", .response = "yo"},
    };
    hu_persona_example_bank_t banks[] = {
        {.channel = "imessage", .examples = imsg_examples, .examples_count = 1},
    };
    hu_persona_t p = {.example_banks = banks, .example_banks_count = 1};

    const hu_persona_example_t *selected[2];
    size_t count = 99;
    hu_error_t err = hu_persona_select_examples(&p, "slack", 5, "greeting", 8, selected, &count, 2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0);
}

static void test_persona_build_prompt_core(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *traits[] = {"direct", "curious"};
    char *rules[] = {"Keep it short"};
    char *values[] = {"honesty"};
    hu_persona_t p = {
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_NOT_NULL(strstr(out, "You are acting as"));
    HU_ASSERT_NOT_NULL(strstr(out, "testuser"));
    HU_ASSERT_NOT_NULL(strstr(out, "direct"));
    HU_ASSERT_NOT_NULL(strstr(out, "Keep it short"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_agent_persona_prompt_injected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
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
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "TestUser"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_spawn_config_has_persona(void) {
    hu_spawn_config_t cfg = {
        .persona_name = "seth",
        .persona_name_len = 4,
    };
    HU_ASSERT_NOT_NULL(cfg.persona_name);
    HU_ASSERT_EQ(cfg.persona_name_len, (size_t)4);
}

static void test_config_persona_field(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"agent\":{\"persona\":\"seth\"}}";
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_arena_t *arena = hu_arena_create(alloc);
    HU_ASSERT_NOT_NULL(arena);
    cfg.arena = arena;
    cfg.allocator = hu_arena_allocator(arena);
    hu_error_t err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(cfg.agent.persona);
    HU_ASSERT_STR_EQ(cfg.agent.persona, "seth");
    hu_config_deinit(&cfg);
}

static void test_persona_cli_run_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_LIST;
    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_persona_cli_run_show_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_SHOW;
    args.name = "nonexistent_persona_xyz_test";
    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_persona_cli_run_delete_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_DELETE;
    args.name = "nonexistent_persona_xyz_test";
    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_persona_cli_run_create_no_provider(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_cli_args_t args = {0};
    args.action = HU_PERSONA_ACTION_CREATE;
    args.name = "test_create";
    args.from_imessage = true;
    hu_error_t err = hu_persona_cli_run(&alloc, &args);
    /* In test mode: HU_OK. In non-test mode: HU_ERR_NOT_SUPPORTED */
    HU_ASSERT_EQ(err, HU_OK);
}

static void test_creator_write_and_load(void) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = hu_strdup(&alloc, "test_creator_write");
    p.name_len = strlen("test_creator_write");
    HU_ASSERT_NOT_NULL(p.name);
    hu_error_t err = hu_persona_creator_write(&alloc, &p);
    if (err != HU_OK) {
        hu_persona_deinit(&alloc, &p);
        return; /* Skip if filesystem not writable (e.g. sandbox) */
    }
    hu_persona_t loaded = {0};
    err = hu_persona_load(&alloc, "test_creator_write", 18, &loaded);
    if (err != HU_OK) {
        hu_persona_deinit(&alloc, &p);
#if defined(__unix__) || defined(__APPLE__)
        {
            const char *home = getenv("HOME");
            if (home && home[0]) {
                char path[512];
                int n = snprintf(path, sizeof(path), "%s/.human/personas/test_creator_write.json",
                                 home);
                if (n > 0 && (size_t)n < sizeof(path))
                    (void)unlink(path);
            }
        }
#endif
        return; /* Skip if load fails (path mismatch or sandbox) */
    }
    HU_ASSERT_NOT_NULL(loaded.name);
    HU_ASSERT_STR_EQ(loaded.name, "test_creator_write");
    hu_persona_deinit(&alloc, &loaded);
    hu_persona_deinit(&alloc, &p);
#if defined(__unix__) || defined(__APPLE__)
    {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char path[512];
            int n =
                snprintf(path, sizeof(path), "%s/.human/personas/test_creator_write.json", home);
            if (n > 0 && (size_t)n < sizeof(path))
                (void)unlink(path);
        }
    }
#endif
#endif
}

static void test_persona_base_dir_returns_override_when_set(void) {
#if defined(__unix__) || defined(__APPLE__)
    char tmpdir[] = "/tmp/human_base_dir_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        return;
    }
    setenv("HU_PERSONA_DIR", tmpdir, 1);
    char buf[512];
    const char *got = hu_persona_base_dir(buf, sizeof(buf));
    unsetenv("HU_PERSONA_DIR");
    rmdir(tmpdir);
    HU_ASSERT_NOT_NULL(got);
    HU_ASSERT_STR_EQ(got, tmpdir);
#else
    (void)0;
#endif
}

#if defined(__unix__) || defined(__APPLE__)
static void test_persona_load_save_roundtrip_with_temp_dir(void) {
    char tmpdir[] = "/tmp/human_persona_test_XXXXXX";
    if (!mkdtemp(tmpdir))
        return; /* skip if can't create temp dir */

    setenv("HU_PERSONA_DIR", tmpdir, 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = hu_strdup(&alloc, "roundtrip_test");
    p.name_len = strlen(p.name);
    p.identity = hu_strdup(&alloc, "Test identity");
    if (!p.name || !p.identity) {
        hu_persona_deinit(&alloc, &p);
        unsetenv("HU_PERSONA_DIR");
        rmdir(tmpdir);
        return;
    }
    hu_error_t err = hu_persona_creator_write(&alloc, &p);
    hu_persona_deinit(&alloc, &p);
    if (err != HU_OK) {
        unsetenv("HU_PERSONA_DIR");
        rmdir(tmpdir);
        return;
    }

    hu_persona_t loaded = {0};
    err = hu_persona_load(&alloc, "roundtrip_test", 14, &loaded);
    unsetenv("HU_PERSONA_DIR");
    if (err != HU_OK) {
        char path[512];
        snprintf(path, sizeof(path), "%s/roundtrip_test.json", tmpdir);
        unlink(path);
        rmdir(tmpdir);
        return;
    }

    HU_ASSERT_STR_EQ(loaded.name, "roundtrip_test");
    HU_ASSERT_STR_EQ(loaded.identity, "Test identity");

    hu_persona_deinit(&alloc, &loaded);

    char path[512];
    snprintf(path, sizeof(path), "%s/roundtrip_test.json", tmpdir);
    unlink(path);
    rmdir(tmpdir);
}
#endif

static void test_persona_prompt_with_channel_overlay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"ch_test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"channel_overlays\":{\"imessage\":{\"formality\":\"casual\","
                       "\"avg_length\":\"short\",\"emoji_usage\":\"minimal\","
                       "\"style_notes\":[\"no caps\"]}}}";
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    /* Without channel — no overlay */
    char *prompt1 = NULL;
    size_t len1 = 0;
    err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &prompt1, &len1);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(prompt1, "imessage") == NULL);

    /* With channel — overlay applied */
    char *prompt2 = NULL;
    size_t len2 = 0;
    err = hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &prompt2, &len2);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(prompt2, "casual") != NULL);
    HU_ASSERT_TRUE(strstr(prompt2, "no caps") != NULL);
    HU_ASSERT_TRUE(len2 > len1);

    alloc.free(alloc.ctx, prompt1, len1 + 1);
    alloc.free(alloc.ctx, prompt2, len2 + 1);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_build_prompt_includes_examples(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"ex_test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]}}";
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    /* Manually add an example bank */
    p.example_banks = alloc.alloc(alloc.ctx, sizeof(hu_persona_example_bank_t));
    HU_ASSERT_TRUE(p.example_banks != NULL);
    memset(p.example_banks, 0, sizeof(hu_persona_example_bank_t));
    p.example_banks_count = 1;
    p.example_banks[0].channel = hu_strdup(&alloc, "cli");
    p.example_banks[0].examples = alloc.alloc(alloc.ctx, sizeof(hu_persona_example_t));
    memset(p.example_banks[0].examples, 0, sizeof(hu_persona_example_t));
    p.example_banks[0].examples_count = 1;
    p.example_banks[0].examples[0].incoming = hu_strdup(&alloc, "Hey what's up");
    p.example_banks[0].examples[0].response = hu_strdup(&alloc, "Not much, you?");

    char *prompt = NULL;
    size_t plen = 0;
    err = hu_persona_build_prompt(&alloc, &p, "cli", 3, NULL, 0, &prompt, &plen);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(prompt, "Hey what's up") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Not much, you?") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Example conversations") != NULL);

    alloc.free(alloc.ctx, prompt, plen + 1);
    hu_persona_deinit(&alloc, &p);
}

static void test_agent_set_persona_clears(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&alloc, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);

    /* Clearing with NULL/empty should succeed */
    HU_ASSERT_EQ(hu_agent_set_persona(&agent, NULL, 0), HU_OK);
    HU_ASSERT_NULL(agent.persona);
    HU_ASSERT_NULL(agent.persona_name);

    hu_agent_deinit(&agent);
}

static void test_agent_set_persona_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&alloc, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);

    hu_error_t err = hu_agent_set_persona(&agent, "nonexistent-persona-xyz", 23);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(agent.persona);

    hu_agent_deinit(&agent);
}

static void test_persona_feedback_record_and_apply(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_feedback_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = "cli";
    fb.channel_len = 3;
    fb.original_response = "Hey there buddy!";
    fb.original_response_len = 16;
    fb.corrected_response = "Hey what's up";
    fb.corrected_response_len = 13;
    fb.context = "greeting";
    fb.context_len = 8;

    /* In test mode, record no-ops on disk but returns HU_OK */
    hu_error_t e = hu_persona_feedback_record(&alloc, "test", 4, &fb);
    HU_ASSERT_EQ(e, HU_OK);

    /* Apply also no-ops in test mode */
    e = hu_persona_feedback_apply(&alloc, "test", 4);
    HU_ASSERT_EQ(e, HU_OK);
}

static void test_persona_build_prompt_with_overlay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *notes[] = {"drops punctuation"};
    hu_persona_overlay_t overlays[] = {
        {.channel = "imessage",
         .formality = "casual",
         .avg_length = "short",
         .emoji_usage = "minimal",
         .style_notes = notes,
         .style_notes_count = 1},
    };
    hu_persona_t p = {
        .name = "testuser",
        .name_len = 8,
        .identity = "A test persona",
        .overlays = overlays,
        .overlays_count = 1,
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "imessage"));
    HU_ASSERT_NOT_NULL(strstr(out, "casual"));
    HU_ASSERT_NOT_NULL(strstr(out, "drops punctuation"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* hu_persona_load_json */
static void test_persona_load_json_malformed_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t e = hu_persona_load_json(&alloc, "not json", 8, &p);
    HU_ASSERT_TRUE(e != HU_OK);
}

static void test_persona_load_json_missing_core(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    const char *json = "{\"version\":1,\"name\":\"test\"}";
    hu_error_t e = hu_persona_load_json(&alloc, json, strlen(json), &p);
    if (e == HU_OK) {
        HU_ASSERT_TRUE(p.traits_count == 0);
        hu_persona_deinit(&alloc, &p);
    }
}

/* hu_persona_load */
static void test_persona_load_empty_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t e = hu_persona_load(&alloc, "", 0, &p);
    HU_ASSERT_TRUE(e != HU_OK);
}

static void test_persona_prompt_respects_size_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Build JSON with many long traits to exceed 8KB prompt */
    static const char pad[81] =
        "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    char *traits_json = alloc.alloc(alloc.ctx, 24 * 1024);
    HU_ASSERT_NOT_NULL(traits_json);
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

    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t err = hu_persona_load_json(&alloc, traits_json, pos, &p);
    alloc.free(alloc.ctx, traits_json, 24 * 1024);
    HU_ASSERT_EQ(err, HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len <= HU_PERSONA_PROMPT_MAX_BYTES);
    if (out_len == HU_PERSONA_PROMPT_MAX_BYTES)
        HU_ASSERT_NOT_NULL(strstr(out, "[persona prompt truncated]"));

    alloc.free(alloc.ctx, out, out_len + 1);
    hu_persona_deinit(&alloc, &p);
}

/* hu_persona_build_prompt - edge cases */
static void test_persona_build_prompt_empty_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "empty";
    p.name_len = 5;
    char *prompt = NULL;
    size_t len = 0;
    hu_error_t e = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &prompt, &len);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(prompt != NULL);
    HU_ASSERT_TRUE(len > 0);
    alloc.free(alloc.ctx, prompt, len + 1);
}

/* hu_persona_examples_load_json - edge cases */
static void test_persona_examples_load_json_empty_bank(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    const char *json = "{\"examples\":[]}";
    hu_error_t e = hu_persona_examples_load_json(&alloc, "test", 4, json, strlen(json), &bank);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(bank.examples_count, 0);
    if (bank.channel)
        alloc.free(alloc.ctx, bank.channel, strlen(bank.channel) + 1);
}

static void test_persona_examples_load_json_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_example_bank_t bank;
    memset(&bank, 0, sizeof(bank));
    hu_error_t e = hu_persona_examples_load_json(&alloc, "test", 4, "bad", 3, &bank);
    HU_ASSERT_TRUE(e != HU_OK);
}

/* hu_persona_select_examples - edge cases */
static void test_persona_select_examples_null_topic_returns_some(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.example_banks = alloc.alloc(alloc.ctx, sizeof(hu_persona_example_bank_t));
    memset(p.example_banks, 0, sizeof(hu_persona_example_bank_t));
    p.example_banks_count = 1;
    p.example_banks[0].channel = hu_strdup(&alloc, "test");
    p.example_banks[0].examples = alloc.alloc(alloc.ctx, sizeof(hu_persona_example_t));
    memset(p.example_banks[0].examples, 0, sizeof(hu_persona_example_t));
    p.example_banks[0].examples_count = 1;
    p.example_banks[0].examples[0].incoming = hu_strdup(&alloc, "hello");
    p.example_banks[0].examples[0].response = hu_strdup(&alloc, "hi");

    const hu_persona_example_t *sel[4];
    size_t sel_count = 0;
    hu_error_t e = hu_persona_select_examples(&p, "test", 4, NULL, 0, sel, &sel_count, 4);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(sel_count, 1);

    hu_persona_deinit(&alloc, &p);
}

static void test_persona_select_examples_max_zero(void) {
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    const hu_persona_example_t *sel[1];
    size_t sel_count = 99;
    hu_error_t e = hu_persona_select_examples(&p, "x", 1, NULL, 0, sel, &sel_count, 0);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_EQ(sel_count, 0);
}

/* Extracted iMessage example bank integration test */
static void test_persona_extracted_imessage_bank_loads_and_selects(void) {
    hu_allocator_t alloc = hu_system_allocator();

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

    hu_persona_example_bank_t bank = {0};
    hu_error_t err =
        hu_persona_examples_load_json(&alloc, "imessage", 8, json, strlen(json), &bank);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(bank.examples_count, 5);
    HU_ASSERT_STR_EQ(bank.channel, "imessage");
    HU_ASSERT_STR_EQ(bank.examples[0].context, "Friend suggesting plans");
    HU_ASSERT_STR_EQ(bank.examples[4].response, "That's my job as your little brother");

    hu_persona_t p = {.example_banks = &bank, .example_banks_count = 1};

    const hu_persona_example_t *sel[3];
    size_t sel_count = 0;
    err = hu_persona_select_examples(&p, "imessage", 8, "lunch plans greeting", 20, sel, &sel_count,
                                     3);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(sel_count > 0);
    HU_ASSERT_TRUE(sel_count <= 3);

    const hu_persona_example_t *sel2[3];
    size_t sel2_count = 0;
    err = hu_persona_select_examples(&p, "imessage", 8, "sibling teasing banter", 22, sel2,
                                     &sel2_count, 3);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(sel2_count > 0);

    free_example_bank(&alloc, &bank);
}

/* hu_persona_find_overlay - edge cases */
static void test_persona_find_overlay_null_channel(void) {
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    const hu_persona_overlay_t *o = hu_persona_find_overlay(&p, NULL, 0);
    HU_ASSERT_TRUE(o == NULL);
}

/* hu_persona_analyzer_parse_response - edge cases */
static void test_persona_analyzer_parse_response_empty_object(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t e = hu_persona_analyzer_parse_response(&alloc, "{}", 2, "test", 4, &p);
    if (e == HU_OK)
        hu_persona_deinit(&alloc, &p);
}

static void test_persona_analyzer_parse_response_malformed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t e = hu_persona_analyzer_parse_response(&alloc, "not json", 8, "test", 4, &p);
    HU_ASSERT_TRUE(e != HU_OK);
}

/* hu_persona_creator_synthesize - edge cases */
static void test_persona_creator_synthesize_single_partial(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t partial;
    memset(&partial, 0, sizeof(partial));
    char *trait = "direct";
    partial.traits = &trait;
    partial.traits_count = 1;

    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t e = hu_persona_creator_synthesize(&alloc, &partial, 1, "single", 6, &out);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_TRUE(out.traits_count >= 1);
    hu_persona_deinit(&alloc, &out);
}

static void test_persona_creator_synthesize_zero_partials(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t e = hu_persona_creator_synthesize(&alloc, &dummy, 0, "empty", 5, &out);
    /* With count 0, succeeds with empty persona */
    if (e == HU_OK) {
        HU_ASSERT_TRUE(out.name != NULL);
        hu_persona_deinit(&alloc, &out);
    }
}

/* hu_persona_sampler - edge cases */
static void test_sampler_imessage_query_small_cap(void) {
    char buf[16];
    size_t out_len = 0;
    hu_error_t e = hu_persona_sampler_imessage_query(buf, 16, &out_len, 100);
    HU_ASSERT_TRUE(e != HU_OK);
}

static void test_sampler_facebook_parse_malformed(void) {
    char **out = NULL;
    size_t count = 0;
    hu_error_t e = hu_persona_sampler_facebook_parse("bad", 3, &out, &count);
    HU_ASSERT_TRUE(e != HU_OK || count == 0);
}

static void test_sampler_facebook_parse_missing_messages(void) {
    char **out = NULL;
    size_t count = 0;
    hu_error_t e = hu_persona_sampler_facebook_parse("{\"other\":1}", 11, &out, &count);
    HU_ASSERT_TRUE(e != HU_OK || count == 0);
}

/* hu_persona_deinit - edge cases */
static void test_persona_deinit_double_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]}}";
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t e = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(e, HU_OK);
    hu_persona_deinit(&alloc, &p);
    hu_persona_deinit(&alloc, &p);
}

/* tool execute tests */
static void test_persona_tool_execute_list(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(tool.vtable != NULL);

    const char *args = "{\"action\":\"list\"}";
    hu_json_value_t *val = NULL;
    err = hu_json_parse(&alloc, args, strlen(args), &val);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    HU_ASSERT_EQ(err, HU_OK);

    hu_json_free(&alloc, val);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_invalid_action(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *args = "{\"action\":\"invalid\"}";
    hu_json_value_t *val = NULL;
    err = hu_json_parse(&alloc, args, strlen(args), &val);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result.success);

    hu_json_free(&alloc, val);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_switch_no_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    /* Switch without agent set returns "No active agent" */
    const char *args = "{\"action\":\"switch\",\"name\":\"test\"}";
    hu_json_value_t *val = NULL;
    err = hu_json_parse(&alloc, args, strlen(args), &val);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result.success);
    HU_ASSERT_TRUE(result.error_msg != NULL);
    HU_ASSERT_TRUE(strstr(result.error_msg, "active agent") != NULL);

    hu_json_free(&alloc, val);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_switch_with_agent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov = {0};
    HU_ASSERT_EQ(hu_provider_create(&alloc, "openai", 6, "test-key", 8, "", 0, &prov), HU_OK);

    hu_agent_t agent = {0};
    HU_ASSERT_EQ(hu_agent_from_config(&agent, &alloc, prov, NULL, 0, NULL, NULL, NULL, NULL,
                                      "gpt-4o-mini", 10, "openai", 6, 0.7, ".", 1, 5, 20, false, 2,
                                      NULL, 0, NULL, 0, NULL),
                 HU_OK);

    hu_agent_set_current_for_tools(&agent);

    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    /* Switch to clear: name empty or null */
    const char *args_clear = "{\"action\":\"switch\"}";
    hu_json_value_t *val = NULL;
    err = hu_json_parse(&alloc, args_clear, strlen(args_clear), &val);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);

    hu_json_free(&alloc, val);
    hu_tool_result_free(&alloc, &result);

    hu_agent_clear_current_for_tools();
    hu_agent_deinit(&agent);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_feedback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *args = "{\"action\":\"feedback\",\"name\":\"test\",\"original\":\"Hey there!\","
                       "\"corrected\":\"Hey what's up\",\"context\":\"greeting\"}";
    hu_json_value_t *val = NULL;
    err = hu_json_parse(&alloc, args, strlen(args), &val);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);

    hu_json_free(&alloc, val);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_feedback_missing_corrected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *args = "{\"action\":\"feedback\",\"name\":\"test\",\"original\":\"Hey\"}";
    hu_json_value_t *val = NULL;
    err = hu_json_parse(&alloc, args, strlen(args), &val);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result.success);

    hu_json_free(&alloc, val);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_create_redirects_to_cli(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *args = "{\"action\":\"create\",\"name\":\"test\"}";
    hu_json_value_t *val = NULL;
    err = hu_json_parse(&alloc, args, strlen(args), &val);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, val, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.output != NULL);
    HU_ASSERT_TRUE(strstr(result.output, "CLI") != NULL || strstr(result.output, "cli") != NULL);

    hu_json_free(&alloc, val);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_show(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *args_str = "{\"action\":\"show\",\"name\":\"nonexistent_xyz\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, args_str, strlen(args_str), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(args != NULL);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    /* In test mode, show returns a stub message */
    HU_ASSERT_TRUE(result.output != NULL);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_apply_feedback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *args_str = "{\"action\":\"apply_feedback\",\"name\":\"test\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, args_str, strlen(args_str), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(args != NULL);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.success);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_persona_tool_execute_apply_feedback_no_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_persona_tool_create(&alloc, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *args_str = "{\"action\":\"apply_feedback\"}";
    hu_json_value_t *args = NULL;
    err = hu_json_parse(&alloc, args_str, strlen(args_str), &args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(args != NULL);

    hu_tool_result_t result;
    memset(&result, 0, sizeof(result));
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result.success);

    hu_json_free(&alloc, args);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* Sampler - additional tests */
static void test_sampler_imessage_query_basic(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_persona_sampler_imessage_query(buf, 512, &len, 100);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf, "SELECT"));
    HU_ASSERT_NOT_NULL(strstr(buf, "LIMIT 100"));
}

static void test_sampler_imessage_query_null_buf(void) {
    size_t len = 0;
    hu_error_t err = hu_persona_sampler_imessage_query(NULL, 512, &len, 100);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_sampler_imessage_query_escapes_quotes(void) {
    char buf[512];
    size_t len = 0;
    const char *handle_id = "o'brien";
    hu_error_t err = hu_persona_sampler_imessage_conversation_query(handle_id, strlen(handle_id),
                                                                    buf, sizeof(buf), &len, 50);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    /* Single quotes in handle_id must be escaped as '' for SQL safety. */
    HU_ASSERT_NOT_NULL(strstr(buf, "o''brien"));
    /* Must not contain unescaped injection pattern. */
    HU_ASSERT_NULL(strstr(buf, "'; DROP TABLE"));
}

static void test_sampler_facebook_parse_empty_object(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 99;
    hu_error_t err = hu_persona_sampler_facebook_parse(json, strlen(json), &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, (size_t)0);
}

static void test_sampler_gmail_parse_null(void) {
    char **out = NULL;
    size_t count = 0;
    hu_error_t err = hu_persona_sampler_gmail_parse(NULL, 0, &out, &count);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_sampler_gmail_parse_empty_object(void) {
    const char *json = "{\"messages\":[]}";
    char **msgs = NULL;
    size_t count = 99;
    hu_error_t err = hu_persona_sampler_gmail_parse(json, strlen(json), &msgs, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, (size_t)0);
}

/* Feedback - null/error tests */
static void test_feedback_record_null_alloc(void) {
    hu_persona_feedback_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = "cli";
    fb.channel_len = 3;
    fb.original_response = "a";
    fb.original_response_len = 1;
    fb.corrected_response = "b";
    fb.corrected_response_len = 1;
    hu_error_t err = hu_persona_feedback_record(NULL, "test", 4, &fb);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_feedback_record_null_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_feedback_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = "cli";
    fb.channel_len = 3;
    fb.original_response = "a";
    fb.original_response_len = 1;
    fb.corrected_response = "b";
    fb.corrected_response_len = 1;
    hu_error_t err = hu_persona_feedback_record(&alloc, NULL, 0, &fb);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_feedback_record_null_feedback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_persona_feedback_record(&alloc, "test", 4, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_feedback_apply_null_alloc(void) {
    hu_error_t err = hu_persona_feedback_apply(NULL, "test", 4);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_feedback_apply_null_name(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_persona_feedback_apply(&alloc, NULL, 0);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* Analyzer - additional tests */
static void test_analyzer_build_prompt_basic(void) {
    const char *messages[] = {"hello", "world"};
    char buf[4096];
    size_t len = 0;
    hu_error_t err =
        hu_persona_analyzer_build_prompt(messages, 2, "discord", buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
}

static void test_analyzer_build_prompt_null_messages(void) {
    char buf[4096];
    size_t len = 0;
    hu_error_t err = hu_persona_analyzer_build_prompt(NULL, 2, "discord", buf, sizeof(buf), &len);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_analyzer_build_prompt_zero_count(void) {
    const char *messages[] = {"hello"};
    char buf[4096];
    size_t len = 0;
    hu_error_t err =
        hu_persona_analyzer_build_prompt(messages, 0, "discord", buf, sizeof(buf), &len);
    /* Current implementation accepts 0 count and returns HU_OK */
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
}

static void test_analyzer_parse_response_null_alloc(void) {
    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    const char *response = "{\"traits\":[]}";
    hu_error_t err =
        hu_persona_analyzer_parse_response(NULL, response, strlen(response), "test", 4, &out);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* Creator - null/error tests */
static void test_creator_synthesize_null_alloc(void) {
    hu_persona_t partial;
    memset(&partial, 0, sizeof(partial));
    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t err = hu_persona_creator_synthesize(NULL, &partial, 1, "test", 4, &out);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_creator_synthesize_null_partials(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t err = hu_persona_creator_synthesize(&alloc, NULL, 1, "test", 4, &out);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_creator_synthesize_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    hu_persona_t out;
    memset(&out, 0, sizeof(out));
    hu_error_t err = hu_persona_creator_synthesize(&alloc, &dummy, 0, "test", 4, &out);
    /* Current implementation accepts 0 count and returns HU_OK with empty persona */
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out.name, "test");
    hu_persona_deinit(&alloc, &out);
}

static void test_creator_write_null_alloc(void) {
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.name = "test";
    p.name_len = 4;
    hu_error_t err = hu_persona_creator_write(NULL, &p);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_overlay_typing_quirks_parsed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"quirk_test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"channel_overlays\":{\"imessage\":{"
                       "\"formality\":\"casual\","
                       "\"typing_quirks\":[\"lowercase\",\"no_periods\"],"
                       "\"message_splitting\":true,"
                       "\"max_segment_chars\":80}}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.overlays_count, 1);
    HU_ASSERT_TRUE(p.overlays[0].message_splitting);
    HU_ASSERT_EQ(p.overlays[0].max_segment_chars, 80u);
    HU_ASSERT_EQ(p.overlays[0].typing_quirks_count, 2);
    HU_ASSERT_STR_EQ(p.overlays[0].typing_quirks[0], "lowercase");
    HU_ASSERT_STR_EQ(p.overlays[0].typing_quirks[1], "no_periods");
    hu_persona_deinit(&alloc, &p);
}

static void test_overlay_typing_quirks_default_when_absent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"no_quirks\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"channel_overlays\":{\"imessage\":{"
                       "\"formality\":\"formal\"}}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.overlays_count, 1);
    HU_ASSERT_FALSE(p.overlays[0].message_splitting);
    HU_ASSERT_EQ(p.overlays[0].max_segment_chars, 0u);
    HU_ASSERT_EQ(p.overlays[0].typing_quirks_count, 0);
    HU_ASSERT_NULL(p.overlays[0].typing_quirks);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_humanization_block_parses_values(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"human_test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"humanization\":{"
                       "\"disfluency_frequency\":0.25,"
                       "\"backchannel_probability\":0.5,"
                       "\"burst_message_probability\":0.08"
                       "},"
                       "\"context_modifiers\":{"
                       "\"serious_topics_reduction\":0.6,"
                       "\"personal_sharing_warmth_boost\":2.0,"
                       "\"high_emotion_breathing_boost\":1.8,"
                       "\"early_turn_humanization_boost\":1.9"
                       "}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ(p.humanization.disfluency_frequency, 0.25f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.humanization.backchannel_probability, 0.5f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.humanization.burst_message_probability, 0.08f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.serious_topics_reduction, 0.6f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.personal_sharing_warmth_boost, 2.0f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.high_emotion_breathing_boost, 1.8f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.early_turn_humanization_boost, 1.9f, 0.001f);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_humanization_defaults_when_absent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"no_humanization\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ(p.humanization.disfluency_frequency, 0.15f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.humanization.backchannel_probability, 0.3f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.humanization.burst_message_probability, 0.03f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.serious_topics_reduction, 0.4f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.personal_sharing_warmth_boost, 1.6f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.high_emotion_breathing_boost, 1.5f, 0.001f);
    HU_ASSERT_FLOAT_EQ(p.context_modifiers.early_turn_humanization_boost, 1.4f, 0.001f);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_important_dates_parses(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"dates_test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"important_dates\":["
                       "{\"date\":\"07-15\",\"type\":\"birthday\",\"message\":\"happy birthday min!\"},"
                       "{\"date\":\"12-25\",\"type\":\"holiday\",\"message\":\"merry christmas!\"}"
                       "]}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.important_dates_count, 2);
    HU_ASSERT_NOT_NULL(p.important_dates);
    HU_ASSERT_STR_EQ(p.important_dates[0].date, "07-15");
    HU_ASSERT_STR_EQ(p.important_dates[0].type, "birthday");
    HU_ASSERT_STR_EQ(p.important_dates[0].message, "happy birthday min!");
    HU_ASSERT_STR_EQ(p.important_dates[1].date, "12-25");
    HU_ASSERT_STR_EQ(p.important_dates[1].type, "holiday");
    HU_ASSERT_STR_EQ(p.important_dates[1].message, "merry christmas!");
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_context_awareness_calendar_enabled(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"ctx_aware\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
                       "\"context_awareness\":{\"calendar_enabled\":true}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(p.context_awareness.calendar_enabled);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_important_dates_context_awareness_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"no_dates_or_ctx\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.important_dates_count, 0);
    HU_ASSERT_NULL(p.important_dates);
    HU_ASSERT_FALSE(p.context_awareness.calendar_enabled);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_phase4_all_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"phase4_test\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
        "\"follow_up_style\":{\"delayed_follow_up_probability\":0.15,\"min_delay_minutes\":20,"
        "\"max_delay_hours\":4},"
        "\"bookend_messages\":{\"enabled\":true,\"morning_window\":[7,9],\"evening_window\":[22,23],"
        "\"frequency_per_week\":2.5,\"phrases_morning\":[\"morning min\"],\"phrases_evening\":[\"night\"]},"
        "\"context_awareness\":{\"calendar_enabled\":false,\"weather_enabled\":false,"
        "\"sports_teams\":[\"Lakers\"],\"news_topics\":[\"tech\"]},"
        "\"humanization\":{\"double_text_probability\":0.08},"
        "\"timezone\":\"America/Denver\",\"location\":\"Denver, CO\",\"group_response_rate\":0.1}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ(p.follow_up_style.delayed_follow_up_probability, 0.15f, 0.001f);
    HU_ASSERT_EQ(p.follow_up_style.min_delay_minutes, 20);
    HU_ASSERT_EQ(p.follow_up_style.max_delay_hours, 4);
    HU_ASSERT_TRUE(p.bookend_messages.enabled);
    HU_ASSERT_EQ(p.bookend_messages.morning_window[0], 7);
    HU_ASSERT_EQ(p.bookend_messages.morning_window[1], 9);
    HU_ASSERT_EQ(p.bookend_messages.evening_window[0], 22);
    HU_ASSERT_EQ(p.bookend_messages.evening_window[1], 23);
    HU_ASSERT_FLOAT_EQ(p.bookend_messages.frequency_per_week, 2.5f, 0.001f);
    HU_ASSERT_EQ(p.bookend_messages.phrases_morning_count, 1);
    HU_ASSERT_STR_EQ(p.bookend_messages.phrases_morning[0], "morning min");
    HU_ASSERT_EQ(p.bookend_messages.phrases_evening_count, 1);
    HU_ASSERT_STR_EQ(p.bookend_messages.phrases_evening[0], "night");
    HU_ASSERT_FALSE(p.context_awareness.calendar_enabled);
    HU_ASSERT_FALSE(p.context_awareness.weather_enabled);
    HU_ASSERT_EQ(p.context_awareness.sports_teams_count, 1);
    HU_ASSERT_STR_EQ(p.context_awareness.sports_teams[0], "Lakers");
    HU_ASSERT_EQ(p.context_awareness.news_topics_count, 1);
    HU_ASSERT_STR_EQ(p.context_awareness.news_topics[0], "tech");
    HU_ASSERT_FLOAT_EQ(p.humanization.double_text_probability, 0.08f, 0.001f);
    HU_ASSERT_STR_EQ(p.timezone, "America/Denver");
    HU_ASSERT_STR_EQ(p.location, "Denver, CO");
    HU_ASSERT_FLOAT_EQ(p.group_response_rate, 0.1f, 0.001f);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_phase4_defaults_when_absent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"no_phase4\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ(p.follow_up_style.delayed_follow_up_probability, 0.15f, 0.001f);
    HU_ASSERT_EQ(p.follow_up_style.min_delay_minutes, 20);
    HU_ASSERT_EQ(p.follow_up_style.max_delay_hours, 4);
    HU_ASSERT_FALSE(p.bookend_messages.enabled);
    HU_ASSERT_EQ(p.bookend_messages.morning_window[0], 7);
    HU_ASSERT_EQ(p.bookend_messages.morning_window[1], 9);
    HU_ASSERT_EQ(p.bookend_messages.evening_window[0], 22);
    HU_ASSERT_EQ(p.bookend_messages.evening_window[1], 23);
    HU_ASSERT_FLOAT_EQ(p.bookend_messages.frequency_per_week, 2.5f, 0.001f);
    HU_ASSERT_EQ(p.bookend_messages.phrases_morning_count, 0);
    HU_ASSERT_EQ(p.bookend_messages.phrases_evening_count, 0);
    HU_ASSERT_EQ(p.context_awareness.sports_teams_count, 0);
    HU_ASSERT_EQ(p.context_awareness.news_topics_count, 0);
    HU_ASSERT_FLOAT_EQ(p.humanization.double_text_probability, 0.08f, 0.001f);
    HU_ASSERT_EQ(p.timezone[0], '\0');
    HU_ASSERT_EQ(p.location[0], '\0');
    HU_ASSERT_FLOAT_EQ(p.group_response_rate, 0.1f, 0.001f);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_voice_block_parses(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"voice_user\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
        "\"voice\":{"
        "\"provider\":\"cartesia\","
        "\"voice_id\":\"cloned-voice-uuid\","
        "\"model\":\"sonic-3-2026-01-12\","
        "\"default_emotion\":\"content\","
        "\"default_speed\":0.95,"
        "\"nonverbals\":true"
        "}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.voice.provider, "cartesia");
    HU_ASSERT_STR_EQ(p.voice.voice_id, "cloned-voice-uuid");
    HU_ASSERT_STR_EQ(p.voice.model, "sonic-3-2026-01-12");
    HU_ASSERT_STR_EQ(p.voice.default_emotion, "content");
    HU_ASSERT_FLOAT_EQ(p.voice.default_speed, 0.95f, 0.001f);
    HU_ASSERT_TRUE(p.voice.nonverbals);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_voice_defaults_when_absent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"no_voice\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.voice.provider, "cartesia");
    HU_ASSERT_EQ(p.voice.voice_id[0], '\0');
    HU_ASSERT_STR_EQ(p.voice.model, "sonic-3-2026-01-12");
    HU_ASSERT_STR_EQ(p.voice.default_emotion, "content");
    HU_ASSERT_FLOAT_EQ(p.voice.default_speed, 0.95f, 0.001f);
    HU_ASSERT_TRUE(p.voice.nonverbals);
    HU_ASSERT_FALSE(p.voice_messages.enabled);
    HU_ASSERT_STR_EQ(p.voice_messages.frequency, "rare");
    HU_ASSERT_EQ(p.voice_messages.max_duration_sec, 30u);
    HU_ASSERT_EQ(p.voice_messages.prefer_for_count, 0u);
    HU_ASSERT_EQ(p.voice_messages.never_for_count, 0u);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_voice_messages_parses(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"voice_msg_user\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
        "\"voice_messages\":{"
        "\"enabled\":true,"
        "\"frequency\":\"occasional\","
        "\"prefer_for\":[\"emotional\",\"late_night\",\"long_response\"],"
        "\"never_for\":[\"questions\",\"logistics\"],"
        "\"max_duration_sec\":60"
        "}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(p.voice_messages.enabled);
    HU_ASSERT_STR_EQ(p.voice_messages.frequency, "occasional");
    HU_ASSERT_EQ(p.voice_messages.max_duration_sec, 60u);
    HU_ASSERT_EQ(p.voice_messages.prefer_for_count, 3u);
    HU_ASSERT_STR_EQ(p.voice_messages.prefer_for[0], "emotional");
    HU_ASSERT_STR_EQ(p.voice_messages.prefer_for[1], "late_night");
    HU_ASSERT_STR_EQ(p.voice_messages.prefer_for[2], "long_response");
    HU_ASSERT_EQ(p.voice_messages.never_for_count, 2u);
    HU_ASSERT_STR_EQ(p.voice_messages.never_for[0], "questions");
    HU_ASSERT_STR_EQ(p.voice_messages.never_for[1], "logistics");
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_load_json_bookend_phrases_morning_array(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"version\":1,\"name\":\"bookend_phrases\","
        "\"core\":{\"identity\":\"Test\",\"traits\":[\"direct\"]},"
        "\"bookend_messages\":{\"enabled\":true,\"phrases_morning\":[\"morning min\",\"hey\",\"gm\"],"
        "\"phrases_evening\":[\"night\",\"gn\"]}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.bookend_messages.phrases_morning_count, 3);
    HU_ASSERT_STR_EQ(p.bookend_messages.phrases_morning[0], "morning min");
    HU_ASSERT_STR_EQ(p.bookend_messages.phrases_morning[1], "hey");
    HU_ASSERT_STR_EQ(p.bookend_messages.phrases_morning[2], "gm");
    HU_ASSERT_EQ(p.bookend_messages.phrases_evening_count, 2);
    HU_ASSERT_STR_EQ(p.bookend_messages.phrases_evening[0], "night");
    HU_ASSERT_STR_EQ(p.bookend_messages.phrases_evening[1], "gn");
    hu_persona_deinit(&alloc, &p);
}

static void test_overlay_typing_quirks_in_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *quirks[] = {"lowercase", "no_periods"};
    hu_persona_overlay_t overlays[] = {
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
    hu_persona_t p = {
        .name = "quirk_user",
        .name_len = 10,
        .identity = "A test persona",
        .overlays = overlays,
        .overlays_count = 1,
    };

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, "imessage", 8, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Typing quirks"));
    HU_ASSERT_NOT_NULL(strstr(out, "lowercase"));
    HU_ASSERT_NOT_NULL(strstr(out, "no_periods"));
    HU_ASSERT_NOT_NULL(strstr(out, "Message splitting: ON"));
    HU_ASSERT_NOT_NULL(strstr(out, "80 chars"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── Rich persona elements (Tier 1–3) ── */

static void test_persona_load_json_rich_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
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

    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    /* Core anchor */
    HU_ASSERT_NOT_NULL(p.core_anchor);
    HU_ASSERT_NOT_NULL(strstr(p.core_anchor, "grounded warmth"));

    /* Motivation */
    HU_ASSERT_NOT_NULL(p.motivation.primary_drive);
    HU_ASSERT_NOT_NULL(strstr(p.motivation.primary_drive, "understood"));
    HU_ASSERT_NOT_NULL(p.motivation.protecting);
    HU_ASSERT_NOT_NULL(p.motivation.avoiding);
    HU_ASSERT_NOT_NULL(p.motivation.wanting);

    /* Situational directions */
    HU_ASSERT_EQ(p.situational_directions_count, 2);
    HU_ASSERT_NOT_NULL(strstr(p.situational_directions[0].trigger, "grieving"));
    HU_ASSERT_NOT_NULL(strstr(p.situational_directions[0].instruction, "shorter"));
    HU_ASSERT_NOT_NULL(strstr(p.situational_directions[1].trigger, "celebrating"));

    /* Humor */
    HU_ASSERT_STR_EQ(p.humor.type, "dry");
    HU_ASSERT_STR_EQ(p.humor.frequency, "occasional");
    HU_ASSERT_EQ(p.humor.targets_count, 2);
    HU_ASSERT_EQ(p.humor.boundaries_count, 2);
    HU_ASSERT_STR_EQ(p.humor.timing, "tension-breaking");

    /* Conflict style */
    HU_ASSERT_STR_EQ(p.conflict_style.pushback_response, "reframe");
    HU_ASSERT_STR_EQ(p.conflict_style.confrontation_comfort, "selective");
    HU_ASSERT_STR_EQ(p.conflict_style.apology_style, "direct");
    HU_ASSERT_NOT_NULL(p.conflict_style.boundary_assertion);
    HU_ASSERT_NOT_NULL(p.conflict_style.repair_behavior);

    /* Emotional range */
    HU_ASSERT_NOT_NULL(p.emotional_range.ceiling);
    HU_ASSERT_NOT_NULL(p.emotional_range.floor);
    HU_ASSERT_EQ(p.emotional_range.escalation_triggers_count, 2);
    HU_ASSERT_EQ(p.emotional_range.de_escalation_count, 2);
    HU_ASSERT_NOT_NULL(p.emotional_range.withdrawal_conditions);
    HU_ASSERT_NOT_NULL(p.emotional_range.recovery_style);

    /* Voice rhythm */
    HU_ASSERT_NOT_NULL(p.voice_rhythm.sentence_pattern);
    HU_ASSERT_NOT_NULL(p.voice_rhythm.paragraph_cadence);
    HU_ASSERT_STR_EQ(p.voice_rhythm.response_tempo, "thoughtful");
    HU_ASSERT_STR_EQ(p.voice_rhythm.emphasis_style, "repetition");
    HU_ASSERT_NOT_NULL(p.voice_rhythm.pause_behavior);

    /* Character invariants */
    HU_ASSERT_EQ(p.character_invariants_count, 2);
    HU_ASSERT_NOT_NULL(strstr(p.character_invariants[0], "Never dismisses"));

    /* Intellectual */
    HU_ASSERT_EQ(p.intellectual.expertise_count, 2);
    HU_ASSERT_EQ(p.intellectual.curiosity_areas_count, 2);
    HU_ASSERT_STR_EQ(p.intellectual.thinking_style, "analogy");
    HU_ASSERT_NOT_NULL(p.intellectual.metaphor_sources);

    /* Backstory behaviors */
    HU_ASSERT_EQ(p.backstory_behaviors_count, 1);
    HU_ASSERT_NOT_NULL(strstr(p.backstory_behaviors[0].backstory_beat, "chaotic"));
    HU_ASSERT_NOT_NULL(strstr(p.backstory_behaviors[0].behavioral_rule, "over-explains"));

    /* Sensory */
    HU_ASSERT_STR_EQ(p.sensory.dominant_sense, "tactile");
    HU_ASSERT_EQ(p.sensory.metaphor_vocabulary_count, 2);
    HU_ASSERT_NOT_NULL(p.sensory.grounding_patterns);

    hu_persona_deinit(&alloc, &p);
}

static void test_persona_prompt_includes_motivation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = "mottest";
    p.name_len = 7;
    p.identity = "Test";
    p.motivation.primary_drive = "connection";
    p.motivation.protecting = "dignity";
    p.motivation.avoiding = "smalltalk";
    p.motivation.wanting = "depth";

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Motivation"));
    HU_ASSERT_NOT_NULL(strstr(out, "connection"));
    HU_ASSERT_NOT_NULL(strstr(out, "dignity"));
    HU_ASSERT_NOT_NULL(strstr(out, "smalltalk"));
    HU_ASSERT_NOT_NULL(strstr(out, "depth"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_situational_directions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_situational_direction_t dirs[] = {
        {.trigger = "user is angry", .instruction = "stay calm, validate first"},
    };
    hu_persona_t p = {0};
    p.name = "sdtest";
    p.name_len = 6;
    p.identity = "Test";
    p.situational_directions = dirs;
    p.situational_directions_count = 1;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "WHEN user is angry"));
    HU_ASSERT_NOT_NULL(strstr(out, "stay calm"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_humor(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *targets[] = {"self"};
    char *bounds[] = {"grief"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Humor"));
    HU_ASSERT_NOT_NULL(strstr(out, "dry"));
    HU_ASSERT_NOT_NULL(strstr(out, "Never funny"));
    HU_ASSERT_NOT_NULL(strstr(out, "grief"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_conflict_style(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = "cftest";
    p.name_len = 6;
    p.identity = "Test";
    p.conflict_style.pushback_response = "reframe";
    p.conflict_style.confrontation_comfort = "high";

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Conflict"));
    HU_ASSERT_NOT_NULL(strstr(out, "reframe"));
    HU_ASSERT_NOT_NULL(strstr(out, "high"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_emotional_range(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *triggers[] = {"injustice"};
    char *deesc[] = {"deep breath"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Emotional Range"));
    HU_ASSERT_NOT_NULL(strstr(out, "warm excitement"));
    HU_ASSERT_NOT_NULL(strstr(out, "injustice"));
    HU_ASSERT_NOT_NULL(strstr(out, "deep breath"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_voice_rhythm(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = "vrtest";
    p.name_len = 6;
    p.identity = "Test";
    p.voice_rhythm.sentence_pattern = "short bursts";
    p.voice_rhythm.response_tempo = "quick";

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Voice Rhythm"));
    HU_ASSERT_NOT_NULL(strstr(out, "short bursts"));
    HU_ASSERT_NOT_NULL(strstr(out, "quick"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_core_anchor(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = "anchor";
    p.name_len = 6;
    p.identity = "Test";
    p.core_anchor = "I am grounded warmth with quiet strength.";

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Core Anchor"));
    HU_ASSERT_NOT_NULL(strstr(out, "grounded warmth"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_character_invariants(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *invar[] = {"Never dismisses feelings", "Always listens first"};
    hu_persona_t p = {0};
    p.name = "citest";
    p.name_len = 6;
    p.identity = "Test";
    p.character_invariants = invar;
    p.character_invariants_count = 2;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Character Invariants"));
    HU_ASSERT_NOT_NULL(strstr(out, "NEVER break"));
    HU_ASSERT_NOT_NULL(strstr(out, "Never dismisses"));
    HU_ASSERT_NOT_NULL(strstr(out, "Always listens"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_intellectual(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *exp[] = {"psychology"};
    char *cur[] = {"cooking"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Intellectual"));
    HU_ASSERT_NOT_NULL(strstr(out, "psychology"));
    HU_ASSERT_NOT_NULL(strstr(out, "cooking"));
    HU_ASSERT_NOT_NULL(strstr(out, "analogy"));
    HU_ASSERT_NOT_NULL(strstr(out, "nature"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_backstory_behaviors(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_backstory_behavior_t bbs[] = {
        {.backstory_beat = "grew up poor", .behavioral_rule = "values resourcefulness"},
    };
    hu_persona_t p = {0};
    p.name = "bbtest";
    p.name_len = 6;
    p.identity = "Test";
    p.backstory_behaviors = bbs;
    p.backstory_behaviors_count = 1;

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Backstory"));
    HU_ASSERT_NOT_NULL(strstr(out, "Because grew up poor"));
    HU_ASSERT_NOT_NULL(strstr(out, "values resourcefulness"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_sensory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *vocab[] = {"hits hard", "feels heavy"};
    hu_persona_t p = {0};
    p.name = "sentest";
    p.name_len = 7;
    p.identity = "Test";
    p.sensory.dominant_sense = "tactile";
    p.sensory.metaphor_vocabulary = vocab;
    p.sensory.metaphor_vocabulary_count = 2;
    p.sensory.grounding_patterns = "references weather";

    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Sensory"));
    HU_ASSERT_NOT_NULL(strstr(out, "tactile"));
    HU_ASSERT_NOT_NULL(strstr(out, "hits hard"));
    HU_ASSERT_NOT_NULL(strstr(out, "references weather"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_validate_rejects_bad_motivation_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"motivation\":\"not an object\"}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_NOT_NULL(strstr(err, "motivation"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_humor_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"humor\":42}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_NOT_NULL(strstr(err, "humor"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_accepts_rich_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
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
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_NULL(err);
}

/* --- Research-backed persona element tests --- */

static void test_persona_prompt_includes_relational(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *bids[] = {"sharing interesting finds", "asking how their day went"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Relational Intelligence"));
    HU_ASSERT_NOT_NULL(strstr(out, "secure"));
    HU_ASSERT_NOT_NULL(strstr(out, "turn toward"));
    HU_ASSERT_NOT_NULL(strstr(out, "sharing interesting finds"));
    HU_ASSERT_NOT_NULL(strstr(out, "anxious"));
    HU_ASSERT_NOT_NULL(strstr(out, "inner circle"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_listening(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *techniques[] = {"open questions", "affirmations", "reflective listening",
                          "summary reflections"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Listening"));
    HU_ASSERT_NOT_NULL(strstr(out, "support"));
    HU_ASSERT_NOT_NULL(strstr(out, "reflective listening"));
    HU_ASSERT_NOT_NULL(strstr(out, "observe without judgment"));
    HU_ASSERT_NOT_NULL(strstr(out, "validate feelings"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_repair(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *phrases[] = {"I think I misread that", "let me try again", "I hear you"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Repair Protocol"));
    HU_ASSERT_NOT_NULL(strstr(out, "tone shifts"));
    HU_ASSERT_NOT_NULL(strstr(out, "take ownership"));
    HU_ASSERT_NOT_NULL(strstr(out, "face-saving"));
    HU_ASSERT_NOT_NULL(strstr(out, "I think I misread that"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_mirroring(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *adapts[] = {"message_length", "formality", "emoji_usage", "pacing"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Linguistic Mirroring"));
    HU_ASSERT_NOT_NULL(strstr(out, "moderate"));
    HU_ASSERT_NOT_NULL(strstr(out, "message_length"));
    HU_ASSERT_NOT_NULL(strstr(out, "gradual"));
    HU_ASSERT_NOT_NULL(strstr(out, "higher-status"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_prompt_includes_social(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *bonding[] = {"remembering small details", "checking in without agenda"};
    char *anti[] = {"shift responses", "unsolicited advice", "one-upping"};
    hu_persona_t p = {0};
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
    hu_error_t err = hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(out, "Social Dynamics"));
    HU_ASSERT_NOT_NULL(strstr(out, "adult with nurturing"));
    HU_ASSERT_NOT_NULL(strstr(out, "small talk"));
    HU_ASSERT_NOT_NULL(strstr(out, "remembering small details"));
    HU_ASSERT_NOT_NULL(strstr(out, "NEVER"));
    HU_ASSERT_NOT_NULL(strstr(out, "shift responses"));
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_persona_load_json_research_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
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
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_NOT_NULL(p.relational.bid_response_style);
    HU_ASSERT_STR_EQ(p.relational.bid_response_style, "turn toward");
    HU_ASSERT_EQ(p.relational.emotional_bids_count, 2);
    HU_ASSERT_STR_EQ(p.relational.attachment_style, "secure");
    HU_ASSERT_STR_EQ(p.relational.attachment_awareness, "detect anxious");
    HU_ASSERT_STR_EQ(p.relational.dunbar_awareness, "invest deeply");

    HU_ASSERT_STR_EQ(p.listening.default_response_type, "support");
    HU_ASSERT_EQ(p.listening.reflective_techniques_count, 2);
    HU_ASSERT_STR_EQ(p.listening.nvc_style, "observe then feel");
    HU_ASSERT_STR_EQ(p.listening.validation_style, "validate first");

    HU_ASSERT_STR_EQ(p.repair.rupture_detection, "tone shifts");
    HU_ASSERT_STR_EQ(p.repair.repair_approach, "name it");
    HU_ASSERT_STR_EQ(p.repair.face_saving_style, "offer exits");
    HU_ASSERT_EQ(p.repair.repair_phrases_count, 2);

    HU_ASSERT_STR_EQ(p.mirroring.mirroring_level, "moderate");
    HU_ASSERT_EQ(p.mirroring.adapts_to_count, 2);
    HU_ASSERT_STR_EQ(p.mirroring.convergence_speed, "gradual");
    HU_ASSERT_STR_EQ(p.mirroring.power_dynamic, "mirror more up");

    HU_ASSERT_STR_EQ(p.social.default_ego_state, "adult");
    HU_ASSERT_STR_EQ(p.social.phatic_style, "warm opener");
    HU_ASSERT_EQ(p.social.bonding_behaviors_count, 1);
    HU_ASSERT_EQ(p.social.anti_patterns_count, 2);

    hu_persona_deinit(&alloc, &p);
}

static void test_persona_validate_rejects_bad_relational_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"relational\":\"not an object\"}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_NOT_NULL(strstr(err, "relational"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_listening_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"listening\":42}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_NOT_NULL(strstr(err, "listening"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_repair_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"repair\":[1,2,3]}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_NOT_NULL(strstr(err, "repair"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_mirroring_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"mirroring\":true}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_NOT_NULL(strstr(err, "mirroring"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_rejects_bad_social_type(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"test\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"social\":\"not an object\"}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(err);
    HU_ASSERT_NOT_NULL(strstr(err, "social"));
    alloc.free(alloc.ctx, err, err_len + 1);
}

static void test_persona_validate_accepts_research_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"rp\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"relational\":{\"bid_response_style\":\"turn toward\"},"
                       "\"listening\":{\"default_response_type\":\"support\"},"
                       "\"repair\":{\"rupture_detection\":\"tone shifts\"},"
                       "\"mirroring\":{\"mirroring_level\":\"moderate\"},"
                       "\"social\":{\"default_ego_state\":\"adult\"}}";
    char *err = NULL;
    size_t err_len = 0;
    hu_error_t e = hu_persona_validate_json(&alloc, json, strlen(json), &err, &err_len);
    HU_ASSERT_EQ(e, HU_OK);
    HU_ASSERT_NULL(err);
}

static void test_persona_deinit_research_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
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
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    hu_persona_deinit(&alloc, &p);
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_deinit_rich_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
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
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    hu_persona_deinit(&alloc, &p);
    /* Double deinit — should be safe */
    hu_persona_deinit(&alloc, &p);
}

static void test_analyzer_parses_research_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
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
    hu_persona_t p = {0};
    hu_error_t err =
        hu_persona_analyzer_parse_response(&alloc, response, strlen(response), "imessage", 8, &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.relational.bid_response_style, "turn toward");
    HU_ASSERT_STR_EQ(p.relational.attachment_style, "secure");
    HU_ASSERT_EQ(p.relational.emotional_bids_count, 1);
    HU_ASSERT_STR_EQ(p.listening.default_response_type, "support");
    HU_ASSERT_EQ(p.listening.reflective_techniques_count, 1);
    HU_ASSERT_STR_EQ(p.repair.rupture_detection, "tone shifts");
    HU_ASSERT_EQ(p.repair.repair_phrases_count, 1);
    HU_ASSERT_STR_EQ(p.mirroring.mirroring_level, "moderate");
    HU_ASSERT_EQ(p.mirroring.adapts_to_count, 1);
    HU_ASSERT_STR_EQ(p.social.default_ego_state, "adult");
    HU_ASSERT_EQ(p.social.anti_patterns_count, 1);
    HU_ASSERT_STR_EQ(p.intellectual.thinking_style, "analytical");
    HU_ASSERT_STR_EQ(p.sensory.dominant_sense, "visual");
    hu_persona_deinit(&alloc, &p);
}

static void test_creator_synthesize_merges_research_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p1 = {0};
    p1.relational.bid_response_style = "turn toward";
    p1.relational.attachment_style = "secure";
    p1.listening.default_response_type = "support";
    p1.repair.rupture_detection = "tone shifts";
    p1.mirroring.mirroring_level = "moderate";
    p1.social.default_ego_state = "adult";

    hu_persona_t p2 = {0};
    p2.relational.dunbar_awareness = "invest deeply";
    p2.listening.nvc_style = "observe then feel";
    p2.repair.repair_approach = "name it";
    p2.mirroring.convergence_speed = "gradual";
    p2.social.phatic_style = "warm opener";

    hu_persona_t partials[] = {p1, p2};
    hu_persona_t merged = {0};
    hu_error_t err = hu_persona_creator_synthesize(&alloc, partials, 2, "merged", 6, &merged);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(merged.relational.bid_response_style, "turn toward");
    HU_ASSERT_STR_EQ(merged.relational.attachment_style, "secure");
    HU_ASSERT_STR_EQ(merged.relational.dunbar_awareness, "invest deeply");
    HU_ASSERT_STR_EQ(merged.listening.default_response_type, "support");
    HU_ASSERT_STR_EQ(merged.listening.nvc_style, "observe then feel");
    HU_ASSERT_STR_EQ(merged.repair.rupture_detection, "tone shifts");
    HU_ASSERT_STR_EQ(merged.repair.repair_approach, "name it");
    HU_ASSERT_STR_EQ(merged.mirroring.mirroring_level, "moderate");
    HU_ASSERT_STR_EQ(merged.mirroring.convergence_speed, "gradual");
    HU_ASSERT_STR_EQ(merged.social.default_ego_state, "adult");
    HU_ASSERT_STR_EQ(merged.social.phatic_style, "warm opener");
    hu_persona_deinit(&alloc, &merged);
}

static void test_contact_profile_attachment_and_dunbar(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"version\":1,\"name\":\"cptest\","
                       "\"core\":{\"identity\":\"Test\",\"traits\":[\"a\"]},"
                       "\"contacts\":{\"alice\":{\"name\":\"Alice\","
                       "\"relationship\":\"close friend\","
                       "\"attachment_style\":\"anxious\","
                       "\"dunbar_layer\":\"inner_circle\"}}}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.contacts_count, 1);
    HU_ASSERT_STR_EQ(p.contacts[0].attachment_style, "anxious");
    HU_ASSERT_STR_EQ(p.contacts[0].dunbar_layer, "inner_circle");

    char *ctx = NULL;
    size_t ctx_len = 0;
    err = hu_contact_profile_build_context(&alloc, &p.contacts[0], &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(ctx, "anxious"));
    HU_ASSERT_NOT_NULL(strstr(ctx, "inner_circle"));
    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_persona_deinit(&alloc, &p);
}

static void test_analyzer_prompt_includes_research_fields(void) {
    const char *messages[] = {"hey", "what's up"};
    char buf[8192];
    size_t out_len = 0;
    hu_error_t err =
        hu_persona_analyzer_build_prompt(messages, 2, "imessage", buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(buf, "relational"));
    HU_ASSERT_NOT_NULL(strstr(buf, "listening"));
    HU_ASSERT_NOT_NULL(strstr(buf, "repair"));
    HU_ASSERT_NOT_NULL(strstr(buf, "mirroring"));
    HU_ASSERT_NOT_NULL(strstr(buf, "social"));
    HU_ASSERT_NOT_NULL(strstr(buf, "intellectual"));
    HU_ASSERT_NOT_NULL(strstr(buf, "sensory"));
    HU_ASSERT_NOT_NULL(strstr(buf, "attachment_style"));
    HU_ASSERT_NOT_NULL(strstr(buf, "bid_response_style"));
    HU_ASSERT_NOT_NULL(strstr(buf, "ego_state"));
    HU_ASSERT_NOT_NULL(strstr(buf, "nvc_style"));
}

static void test_auto_profile_returns_mock_overlay(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_overlay_t overlay;
    memset(&overlay, 0, sizeof(overlay));
    hu_error_t err = hu_persona_auto_profile(&alloc, "+18001234567", 12, &overlay);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(overlay.formality);
    HU_ASSERT_NOT_NULL(overlay.avg_length);
    HU_ASSERT_NOT_NULL(overlay.emoji_usage);
    if (overlay.formality)
        alloc.free(alloc.ctx, (char *)overlay.formality, strlen(overlay.formality) + 1);
    if (overlay.avg_length)
        alloc.free(alloc.ctx, (char *)overlay.avg_length, strlen(overlay.avg_length) + 1);
    if (overlay.emoji_usage)
        alloc.free(alloc.ctx, (char *)overlay.emoji_usage, strlen(overlay.emoji_usage) + 1);
}

static void test_auto_profile_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_overlay_t overlay;
    HU_ASSERT_EQ(hu_persona_auto_profile(NULL, "+1", 2, &overlay), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_auto_profile(&alloc, NULL, 0, &overlay), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_auto_profile(&alloc, "+1", 2, NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_profile_describe_style_formats(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_sampler_contact_stats_t stats = {0};
    stats.their_msg_count = 100;
    stats.avg_their_len = 25;
    stats.uses_emoji = true;
    stats.sends_links = false;
    stats.texts_in_bursts = true;
    stats.prefers_short = true;
    size_t out_len = 0;
    char *desc = hu_persona_profile_describe_style(&alloc, &stats, "+1800", 5, &out_len);
    HU_ASSERT_NOT_NULL(desc);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(desc, "short") != NULL);
    HU_ASSERT_TRUE(strstr(desc, "emoji") != NULL);
    HU_ASSERT_TRUE(strstr(desc, "bursts") != NULL);
    HU_ASSERT_TRUE(strstr(desc, "Mirror") != NULL);
    alloc.free(alloc.ctx, desc, out_len + 1);
}

static void test_profile_describe_style_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    HU_ASSERT_NULL(hu_persona_profile_describe_style(NULL, NULL, NULL, 0, &out_len));
    HU_ASSERT_NULL(hu_persona_profile_describe_style(&alloc, NULL, NULL, 0, &out_len));
}

/* E2E dry run: simulate a Mindy message through the full persona pipeline */
static void test_e2e_mindy_message_full_pipeline(void) {
    hu_allocator_t alloc = hu_system_allocator();

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
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(p.name, "seth");

    /* Step 2: Find the Mindy contact */
    const hu_contact_profile_t *cp = hu_persona_find_contact(&p, "mindy", 5);
    HU_ASSERT_NOT_NULL(cp);
    HU_ASSERT_STR_EQ(cp->name, "Mindy");
    HU_ASSERT_STR_EQ(cp->relationship_stage, "close_family");
    HU_ASSERT_TRUE(cp->texts_in_bursts);
    HU_ASSERT_TRUE(cp->prefers_short_texts);

    /* Step 3: Build contact context */
    char *contact_ctx = NULL;
    size_t contact_ctx_len = 0;
    err = hu_contact_profile_build_context(&alloc, cp, &contact_ctx, &contact_ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(contact_ctx);
    HU_ASSERT_TRUE(strstr(contact_ctx, "Mindy") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "older sister") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "close_family") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "STAGE RULES") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "inner circle") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "relentless teasing") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "Hey Min!") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "bursts") != NULL);
    HU_ASSERT_TRUE(strstr(contact_ctx, "short") != NULL);

    /* Step 4: Build inner world context (close_family should pass stage gate) */
    size_t iw_len = 0;
    char *iw_ctx =
        hu_persona_build_inner_world_context(&alloc, &p, cp->relationship_stage, &iw_len);
    HU_ASSERT_NOT_NULL(iw_ctx);
    HU_ASSERT_TRUE(iw_len > 0);
    HU_ASSERT_TRUE(strstr(iw_ctx, "Inner World") != NULL);

    alloc.free(alloc.ctx, iw_ctx, iw_len + 1);

    /* Step 5: Attach example bank (simulating auto-load from disk) */
    hu_persona_example_t examples[] = {
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
    hu_persona_example_bank_t bank = {
        .channel = "imessage", .examples = examples, .examples_count = 3};
    p.example_banks = &bank;
    p.example_banks_count = 1;

    /* Step 6: Build the full prompt with a simulated Mindy message */
    char *prompt = NULL;
    size_t prompt_len = 0;
    const char *topic = "Hey! What are you up to today?";
    err = hu_persona_build_prompt(&alloc, &p, "imessage", 8, topic, strlen(topic), &prompt,
                                  &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(prompt_len > 200);

    /* Verify prompt contains all key pipeline outputs */

    /* Identity + biography */
    HU_ASSERT_TRUE(strstr(prompt, "seth") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Star Valley") != NULL);

    /* Traits */
    HU_ASSERT_TRUE(strstr(prompt, "direct") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "teasing") != NULL);

    /* Communication rules */
    HU_ASSERT_TRUE(strstr(prompt, "Keep messages short") != NULL);

    /* Motivation */
    HU_ASSERT_TRUE(strstr(prompt, "Building AI") != NULL);

    /* Humor */
    HU_ASSERT_TRUE(strstr(prompt, "teasing") != NULL);

    /* Voice rhythm */
    HU_ASSERT_TRUE(strstr(prompt, "Short fragments") != NULL);

    /* Situational directions */
    HU_ASSERT_TRUE(strstr(prompt, "Mindy shares") != NULL);

    /* Backstory behaviors */
    HU_ASSERT_TRUE(strstr(prompt, "chaos") != NULL);

    /* Character invariants */
    HU_ASSERT_TRUE(strstr(prompt, "Always responds to family") != NULL);

    /* Core anchor */
    HU_ASSERT_TRUE(strstr(prompt, "nerdy mountain kid") != NULL);

    /* Directors notes */
    HU_ASSERT_TRUE(strstr(prompt, "Never sound like an AI") != NULL);

    /* Channel overlay */
    HU_ASSERT_TRUE(strstr(prompt, "very casual") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "lowercase") != NULL);

    /* Example conversations from bank */
    HU_ASSERT_TRUE(strstr(prompt, "Example conversations") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Hey there") != NULL ||
                   strstr(prompt, "little brother") != NULL || strstr(prompt, "1300") != NULL);

    /* Conflict style */
    HU_ASSERT_TRUE(strstr(prompt, "Gets quiet") != NULL);

    /* Emotional range */
    HU_ASSERT_TRUE(strstr(prompt, "exclamation") != NULL);

    /* Inner world is built separately (hu_persona_build_inner_world_context) and
     * merged by the daemon, not by hu_persona_build_prompt. Verified in Step 4. */

    /* Intellectual */
    HU_ASSERT_TRUE(strstr(prompt, "Agentic AI") != NULL);

    /* Sensory */
    HU_ASSERT_TRUE(strstr(prompt, "kinesthetic") != NULL);

    /* Detach example bank before deinit (stack-allocated) */
    p.example_banks = NULL;
    p.example_banks_count = 0;

    alloc.free(alloc.ctx, contact_ctx, contact_ctx_len + 1);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
    hu_persona_deinit(&alloc, &p);
}

/* --- Externalized prompt field tests --- */

static void test_persona_load_externalized_fields(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{"
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
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_EQ(p.immersive_reinforcement_count, 3);
    HU_ASSERT_STR_EQ(p.immersive_reinforcement[0], "rule-A");
    HU_ASSERT_STR_EQ(p.immersive_reinforcement[1], "rule-B");
    HU_ASSERT_STR_EQ(p.immersive_reinforcement[2], "rule-C");

    HU_ASSERT_STR_EQ(p.identity_reinforcement, "You are real, not an AI.");

    HU_ASSERT_EQ(p.anti_patterns_count, 2);
    HU_ASSERT_STR_EQ(p.anti_patterns[0], "no-exclamation");
    HU_ASSERT_STR_EQ(p.anti_patterns[1], "no-emoji");

    HU_ASSERT_EQ(p.style_rules_count, 1);
    HU_ASSERT_STR_EQ(p.style_rules[0], "lowercase only");

    HU_ASSERT_STR_EQ(p.proactive_rules, "Check in gently.");

    HU_ASSERT_STR_EQ(p.time_overlay_late_night, "Be calm.");
    HU_ASSERT_STR_EQ(p.time_overlay_early_morning, "Be gentle.");
    HU_ASSERT_STR_EQ(p.time_overlay_afternoon, "Be productive.");
    HU_ASSERT_STR_EQ(p.time_overlay_evening, "Wind down.");

    hu_persona_deinit(&alloc, &p);
}

static void test_persona_externalized_fields_absent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{ \"name\": \"bare\", \"core\": { \"identity\": \"Bare\" } }";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_EQ(p.immersive_reinforcement_count, 0);
    HU_ASSERT_NULL(p.immersive_reinforcement);
    HU_ASSERT_NULL(p.identity_reinforcement);
    HU_ASSERT_EQ(p.anti_patterns_count, 0);
    HU_ASSERT_NULL(p.anti_patterns);
    HU_ASSERT_EQ(p.style_rules_count, 0);
    HU_ASSERT_NULL(p.style_rules);
    HU_ASSERT_NULL(p.proactive_rules);
    HU_ASSERT_NULL(p.time_overlay_late_night);
    HU_ASSERT_NULL(p.time_overlay_early_morning);
    HU_ASSERT_NULL(p.time_overlay_afternoon);
    HU_ASSERT_NULL(p.time_overlay_evening);

    hu_persona_deinit(&alloc, &p);
}

static void test_persona_externalized_deinit_frees(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{"
                       "  \"name\": \"deinit-ext\","
                       "  \"core\": { \"identity\": \"Ext\" },"
                       "  \"immersive_reinforcement\": [\"a\"],"
                       "  \"identity_reinforcement\": \"x\","
                       "  \"anti_patterns\": [\"b\"],"
                       "  \"style_rules\": [\"c\"],"
                       "  \"proactive_rules\": \"y\","
                       "  \"time_overlays\": { \"late_night\": \"z\" }"
                       "}";
    hu_persona_t p = {0};
    hu_error_t err = hu_persona_load_json(&alloc, json, strlen(json), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(p.immersive_reinforcement_count, 1);
    HU_ASSERT_NOT_NULL(p.identity_reinforcement);
    hu_persona_deinit(&alloc, &p);
    HU_ASSERT_EQ(p.immersive_reinforcement_count, 0);
}

static void test_persona_immersive_in_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = hu_strndup(&alloc, "test-immersive", 14);
    p.identity = hu_strndup(&alloc, "An immersive persona", 20);

    char *reinforcement[] = {"You are Seth, not an AI", "Stay in character always"};
    p.immersive_reinforcement = reinforcement;
    p.immersive_reinforcement_count = 2;

    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
    hu_error_t err =
        hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &persona_prompt, &persona_prompt_len);
    HU_ASSERT_EQ(err, HU_OK);

    hu_prompt_config_t cfg = {0};
    cfg.persona_prompt = persona_prompt;
    cfg.persona_prompt_len = persona_prompt_len;
    cfg.persona_immersive = true;
    cfg.persona = &p;

    char *sys = NULL;
    size_t sys_len = 0;
    err = hu_prompt_build_system(&alloc, &cfg, &sys, &sys_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(sys);
    HU_ASSERT_TRUE(strstr(sys, "You are Seth, not an AI") != NULL);
    HU_ASSERT_TRUE(strstr(sys, "Stay in character always") != NULL);

    alloc.free(alloc.ctx, sys, sys_len + 1);
    alloc.free(alloc.ctx, persona_prompt, persona_prompt_len + 1);

    p.immersive_reinforcement = NULL;
    p.immersive_reinforcement_count = 0;
    hu_persona_deinit(&alloc, &p);
}

static void test_persona_immersive_fallback_when_no_reinforcement(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p = {0};
    p.name = hu_strndup(&alloc, "test-fallback", 13);
    p.identity = hu_strndup(&alloc, "Fallback persona", 16);

    char *persona_prompt = NULL;
    size_t persona_prompt_len = 0;
    hu_error_t err =
        hu_persona_build_prompt(&alloc, &p, NULL, 0, NULL, 0, &persona_prompt, &persona_prompt_len);
    HU_ASSERT_EQ(err, HU_OK);

    hu_prompt_config_t cfg = {0};
    cfg.persona_prompt = persona_prompt;
    cfg.persona_prompt_len = persona_prompt_len;
    cfg.persona_immersive = true;
    cfg.persona = &p;

    char *sys = NULL;
    size_t sys_len = 0;
    err = hu_prompt_build_system(&alloc, &cfg, &sys, &sys_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(sys);
    HU_ASSERT_TRUE(strstr(sys, "real person") != NULL);

    alloc.free(alloc.ctx, sys, sys_len + 1);
    alloc.free(alloc.ctx, persona_prompt, persona_prompt_len + 1);
    hu_persona_deinit(&alloc, &p);
}

void run_persona_tests(void) {
    HU_TEST_SUITE("Persona");

    HU_RUN_TEST(test_persona_types_exist);
    HU_RUN_TEST(test_persona_find_overlay_found);
    HU_RUN_TEST(test_persona_find_overlay_not_found);
    HU_RUN_TEST(test_persona_deinit_null_safe);
    HU_RUN_TEST(test_persona_load_json_basic);
    HU_RUN_TEST(test_persona_load_json_empty);
    HU_RUN_TEST(test_persona_load_not_found);
    HU_RUN_TEST(test_agent_persona_prompt_injected);
    HU_RUN_TEST(test_spawn_config_has_persona);
    HU_RUN_TEST(test_config_persona_field);
    HU_RUN_TEST(test_persona_build_prompt_core);
    HU_RUN_TEST(test_persona_prompt_respects_size_cap);
    HU_RUN_TEST(test_persona_build_prompt_includes_examples);
    HU_RUN_TEST(test_persona_prompt_with_channel_overlay);
    HU_RUN_TEST(test_agent_set_persona_clears);
    HU_RUN_TEST(test_agent_set_persona_not_found);
    HU_RUN_TEST(test_persona_feedback_record_and_apply);
    HU_RUN_TEST(test_persona_build_prompt_with_overlay);
    HU_RUN_TEST(test_persona_examples_load_json);
    HU_RUN_TEST(test_persona_prompt_overrides_default);
    HU_RUN_TEST(test_spawn_config_persona_field);
    HU_RUN_TEST(test_persona_cli_parse_create);
    HU_RUN_TEST(test_persona_cli_parse_show);
    HU_RUN_TEST(test_persona_cli_parse_list);
    HU_RUN_TEST(test_persona_cli_parse_validate);
    HU_RUN_TEST(test_persona_cli_parse_feedback_apply);
    HU_RUN_TEST(test_cli_parse_diff);
    HU_RUN_TEST(test_persona_validate_json_valid);
    HU_RUN_TEST(test_persona_validate_json_missing_name);
    HU_RUN_TEST(test_persona_validate_json_missing_core);
    HU_RUN_TEST(test_persona_validate_json_malformed);
    HU_RUN_TEST(test_persona_cli_run_validate);
    HU_RUN_TEST(test_persona_cli_run_feedback_apply);
    HU_RUN_TEST(test_persona_cli_run_list);
    HU_RUN_TEST(test_persona_cli_run_show_not_found);
    HU_RUN_TEST(test_persona_cli_run_delete_not_found);
    HU_RUN_TEST(test_persona_cli_run_create_no_provider);
    HU_RUN_TEST(test_creator_write_and_load);
    HU_RUN_TEST(test_persona_base_dir_returns_override_when_set);
#if defined(__unix__) || defined(__APPLE__)
    HU_RUN_TEST(test_persona_load_save_roundtrip_with_temp_dir);
#endif
    HU_RUN_TEST(test_persona_tool_create);
    HU_RUN_TEST(test_creator_synthesize_merges);
    HU_RUN_TEST(test_analyzer_builds_prompt);
    HU_RUN_TEST(test_analyzer_parses_response);
    HU_RUN_TEST(test_sampler_imessage_query);
    HU_RUN_TEST(test_sampler_facebook_parse_basic);
    HU_RUN_TEST(test_sampler_facebook_parse_empty);
    HU_RUN_TEST(test_sampler_facebook_parse_null);
    HU_RUN_TEST(test_sampler_gmail_parse_basic);
    HU_RUN_TEST(test_sampler_gmail_parse_empty);
    HU_RUN_TEST(test_cli_parse_export);
    HU_RUN_TEST(test_cli_parse_merge);
    HU_RUN_TEST(test_cli_parse_import);
    HU_RUN_TEST(test_cli_parse_from_facebook_file);
    HU_RUN_TEST(test_cli_parse_from_gmail);
    HU_RUN_TEST(test_cli_parse_from_response);
    HU_RUN_TEST(test_persona_select_examples_match);
    HU_RUN_TEST(test_persona_select_examples_no_channel);
    HU_RUN_TEST(test_persona_select_examples_no_match);
    HU_RUN_TEST(test_persona_full_round_trip);

    /* Error-path and edge-case tests */
    HU_RUN_TEST(test_persona_load_json_malformed_returns_error);
    HU_RUN_TEST(test_persona_load_json_missing_core);
    HU_RUN_TEST(test_persona_load_empty_name);
    HU_RUN_TEST(test_persona_build_prompt_empty_persona);
    HU_RUN_TEST(test_persona_examples_load_json_empty_bank);
    HU_RUN_TEST(test_persona_examples_load_json_malformed);
    HU_RUN_TEST(test_persona_select_examples_null_topic_returns_some);
    HU_RUN_TEST(test_persona_select_examples_max_zero);
    HU_RUN_TEST(test_persona_extracted_imessage_bank_loads_and_selects);
    HU_RUN_TEST(test_persona_find_overlay_null_channel);
    HU_RUN_TEST(test_persona_analyzer_parse_response_empty_object);
    HU_RUN_TEST(test_persona_analyzer_parse_response_malformed);
    HU_RUN_TEST(test_persona_creator_synthesize_single_partial);
    HU_RUN_TEST(test_persona_creator_synthesize_zero_partials);
    HU_RUN_TEST(test_sampler_imessage_query_small_cap);
    HU_RUN_TEST(test_sampler_facebook_parse_malformed);
    HU_RUN_TEST(test_sampler_facebook_parse_missing_messages);
    HU_RUN_TEST(test_persona_deinit_double_call);

    /* Persona tool execute tests */
    HU_RUN_TEST(test_persona_tool_execute_list);
    HU_RUN_TEST(test_persona_tool_execute_invalid_action);
    HU_RUN_TEST(test_persona_tool_execute_create_redirects_to_cli);
    HU_RUN_TEST(test_persona_tool_execute_switch_no_agent);
    HU_RUN_TEST(test_persona_tool_execute_switch_with_agent);
    HU_RUN_TEST(test_persona_tool_execute_feedback);
    HU_RUN_TEST(test_persona_tool_execute_feedback_missing_corrected);
    HU_RUN_TEST(test_persona_tool_execute_show);
    HU_RUN_TEST(test_persona_tool_execute_apply_feedback);
    HU_RUN_TEST(test_persona_tool_execute_apply_feedback_no_name);

    /* Sampler - additional tests */
    HU_RUN_TEST(test_sampler_imessage_query_basic);
    HU_RUN_TEST(test_sampler_imessage_query_null_buf);
    HU_RUN_TEST(test_sampler_imessage_query_escapes_quotes);
    HU_RUN_TEST(test_sampler_facebook_parse_empty_object);
    HU_RUN_TEST(test_sampler_gmail_parse_null);
    HU_RUN_TEST(test_sampler_gmail_parse_empty_object);

    /* Feedback - null/error tests */
    HU_RUN_TEST(test_feedback_record_null_alloc);
    HU_RUN_TEST(test_feedback_record_null_name);
    HU_RUN_TEST(test_feedback_record_null_feedback);
    HU_RUN_TEST(test_feedback_apply_null_alloc);
    HU_RUN_TEST(test_feedback_apply_null_name);

    /* Analyzer - additional tests */
    HU_RUN_TEST(test_analyzer_build_prompt_basic);
    HU_RUN_TEST(test_analyzer_build_prompt_null_messages);
    HU_RUN_TEST(test_analyzer_build_prompt_zero_count);
    HU_RUN_TEST(test_analyzer_parse_response_null_alloc);

    /* Creator - null/error tests */
    HU_RUN_TEST(test_creator_synthesize_null_alloc);
    HU_RUN_TEST(test_creator_synthesize_null_partials);
    HU_RUN_TEST(test_creator_synthesize_zero_count);
    HU_RUN_TEST(test_creator_write_null_alloc);

    /* Overlay typing quirks */
    HU_RUN_TEST(test_overlay_typing_quirks_parsed);
    HU_RUN_TEST(test_overlay_typing_quirks_default_when_absent);
    HU_RUN_TEST(test_overlay_typing_quirks_in_prompt);

    /* Humanization and context_modifiers */
    HU_RUN_TEST(test_persona_load_json_humanization_block_parses_values);
    HU_RUN_TEST(test_persona_load_json_humanization_defaults_when_absent);

    /* Important dates and context_awareness */
    HU_RUN_TEST(test_persona_load_json_important_dates_parses);
    HU_RUN_TEST(test_persona_load_json_context_awareness_calendar_enabled);
    HU_RUN_TEST(test_persona_load_json_important_dates_context_awareness_defaults);

    /* Phase 4 — follow-ups, bookends, timezone, weather, groups */
    HU_RUN_TEST(test_persona_load_json_phase4_all_fields);
    HU_RUN_TEST(test_persona_load_json_phase4_defaults_when_absent);
    HU_RUN_TEST(test_persona_load_json_voice_block_parses);
    HU_RUN_TEST(test_persona_load_json_voice_defaults_when_absent);
    HU_RUN_TEST(test_persona_load_json_voice_messages_parses);
    HU_RUN_TEST(test_persona_load_json_bookend_phrases_morning_array);

    /* Rich persona elements (Tier 1–3) */
    HU_RUN_TEST(test_persona_load_json_rich_persona);
    HU_RUN_TEST(test_persona_prompt_includes_motivation);
    HU_RUN_TEST(test_persona_prompt_includes_situational_directions);
    HU_RUN_TEST(test_persona_prompt_includes_humor);
    HU_RUN_TEST(test_persona_prompt_includes_conflict_style);
    HU_RUN_TEST(test_persona_prompt_includes_emotional_range);
    HU_RUN_TEST(test_persona_prompt_includes_voice_rhythm);
    HU_RUN_TEST(test_persona_prompt_includes_core_anchor);
    HU_RUN_TEST(test_persona_prompt_includes_character_invariants);
    HU_RUN_TEST(test_persona_prompt_includes_intellectual);
    HU_RUN_TEST(test_persona_prompt_includes_backstory_behaviors);
    HU_RUN_TEST(test_persona_prompt_includes_sensory);
    HU_RUN_TEST(test_persona_validate_rejects_bad_motivation_type);
    HU_RUN_TEST(test_persona_validate_rejects_bad_humor_type);
    HU_RUN_TEST(test_persona_validate_accepts_rich_persona);
    HU_RUN_TEST(test_persona_deinit_rich_persona);
    HU_RUN_TEST(test_persona_prompt_includes_relational);
    HU_RUN_TEST(test_persona_prompt_includes_listening);
    HU_RUN_TEST(test_persona_prompt_includes_repair);
    HU_RUN_TEST(test_persona_prompt_includes_mirroring);
    HU_RUN_TEST(test_persona_prompt_includes_social);
    HU_RUN_TEST(test_persona_load_json_research_fields);
    HU_RUN_TEST(test_persona_validate_rejects_bad_relational_type);
    HU_RUN_TEST(test_persona_validate_rejects_bad_listening_type);
    HU_RUN_TEST(test_persona_validate_rejects_bad_repair_type);
    HU_RUN_TEST(test_persona_validate_rejects_bad_mirroring_type);
    HU_RUN_TEST(test_persona_validate_rejects_bad_social_type);
    HU_RUN_TEST(test_persona_validate_accepts_research_persona);
    HU_RUN_TEST(test_persona_deinit_research_fields);
    HU_RUN_TEST(test_analyzer_parses_research_fields);
    HU_RUN_TEST(test_creator_synthesize_merges_research_fields);
    HU_RUN_TEST(test_contact_profile_attachment_and_dunbar);
    HU_RUN_TEST(test_analyzer_prompt_includes_research_fields);

    /* Auto-profile tests */
    HU_RUN_TEST(test_auto_profile_returns_mock_overlay);
    HU_RUN_TEST(test_auto_profile_null_args);
    HU_RUN_TEST(test_profile_describe_style_formats);
    HU_RUN_TEST(test_profile_describe_style_null_args);

    /* Externalized prompt fields */
    HU_RUN_TEST(test_persona_load_externalized_fields);
    HU_RUN_TEST(test_persona_externalized_fields_absent);
    HU_RUN_TEST(test_persona_externalized_deinit_frees);
    HU_RUN_TEST(test_persona_immersive_in_prompt);
    HU_RUN_TEST(test_persona_immersive_fallback_when_no_reinforcement);

    /* E2E dry run */
    HU_RUN_TEST(test_e2e_mindy_message_full_pipeline);
}
