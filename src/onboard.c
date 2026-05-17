#include "human/onboard.h"
#include "human/config.h"
#include "human/core/io_secure.h"
#include "human/core/string.h"
#include "human/interactions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define HU_CONFIG_DIR  ".human"
#define HU_CONFIG_FILE "config.json"
#define HU_MAX_PATH    1024

static char *get_config_path(char *buf, size_t buf_size) {
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    int n = snprintf(buf, buf_size, "%s/%s/%s", home, HU_CONFIG_DIR, HU_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= buf_size)
        return NULL;
    return buf;
}

bool hu_onboard_check_first_run(void) {
    char path[HU_MAX_PATH];
    if (!get_config_path(path, sizeof(path)))
        return true;
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return false;
    }
    return true;
}

/* Single source of truth — accessed via hu_starter_persona_get(),
 * referenced by both `human init` (src/cli_commands.c) and
 * `human onboard` (this file). Defined outside the HU_IS_TEST guard
 * so unit tests (which set HU_IS_TEST) can still link.
 *
 * The starter persona JSON is split into two static const arrays because
 * the combined literal exceeds C99's 4095-char guaranteed minimum and
 * GCC's -Werror=overlength-strings rejects it on strict Linux builds.
 * The split lands at the natural JSON seam between channel_overlays and
 * example_banks. Consumers call hu_starter_persona_get() to receive a
 * pointer to a lazily-joined flat buffer (bytes-on-disk-equivalent to the
 * original single-array form). */
static const char hu_starter_persona_json_part1[] =
    "{\n"
    "  \"version\": 1,\n"
    "  \"name\": \"default\",\n"
    "  \"core\": {\n"
    "    \"identity\": \"A helpful, thoughtful personal assistant that adapts to your "
    "style over time.\",\n"
    "    \"traits\": [\"attentive\", \"concise\", \"warm\"],\n"
    "    \"communication_rules\": [\n"
    "      \"Match the user's energy and formality level\",\n"
    "      \"Be direct but not curt\",\n"
    "      \"Remember context from previous conversations\"\n"
    "    ]\n"
    "  },\n"
    "  \"channel_overlays\": {\n"
    "    \"imessage\": {\n"
    "      \"formality\": \"casual\",\n"
    "      \"avg_length\": \"short\",\n"
    "      \"emoji_usage\": \"moderate\",\n"
    "      \"style_notes\": \"Casual texting style. Short messages. "
    "Use tapbacks when appropriate.\"\n"
    "    },\n"
    "    \"telegram\": {\n"
    "      \"formality\": \"casual\",\n"
    "      \"avg_length\": \"medium\",\n"
    "      \"emoji_usage\": \"low\",\n"
    "      \"style_notes\": \"Conversational but slightly more detailed than texting.\"\n"
    "    },\n"
    "    \"discord\": {\n"
    "      \"formality\": \"casual\",\n"
    "      \"avg_length\": \"medium\",\n"
    "      \"emoji_usage\": \"high\",\n"
    "      \"style_notes\": \"Relaxed community tone. React with emoji when fitting.\"\n"
    "    },\n"
    "    \"slack\": {\n"
    "      \"formality\": \"professional\",\n"
    "      \"avg_length\": \"medium\",\n"
    "      \"emoji_usage\": \"minimal\",\n"
    "      \"style_notes\": \"Professional but approachable. Use threads. "
    "Be concise.\"\n"
    "    },\n"
    "    \"cli\": {\n"
    "      \"formality\": \"neutral\",\n"
    "      \"avg_length\": \"long\",\n"
    "      \"emoji_usage\": \"none\",\n"
    "      \"style_notes\": \"Technical, precise. No emoji. "
    "Format code blocks when showing code.\"\n"
    "    }\n"
    "  },\n";

/* Starter example banks for the four Tier-1 channels. The persona
 * prompt builder samples from these to anchor tone and length when
 * a fresh user has no learned-from-history examples yet. Each bank
 * holds three short examples chosen to match the channel's overlay
 * (formality / avg_length / emoji_usage). Sprint 2b Story A'.
 *
 * Schema (parsed by `hu_persona_load_json` ->
 * `hu_persona_examples_bank_from_array`):
 *
 *   { "channel": "<name>",
 *     "examples": [
 *       {"context": "...", "incoming": "...", "response": "..."},
 *       ... ] }
 *
 * Examples are deliberately neutral (no proper nouns, no PII, no
 * politics, no proper names) so they ship as defaults for every
 * user without baking-in a single voice. They demonstrate length
 * and emoji norms; the persona system overlays the user's learned
 * style on top once `personal_model.bin` has data. */
static const char hu_starter_persona_json_part2[] =
    "  \"example_banks\": [\n"
    "    {\n"
    "      \"channel\": \"imessage\",\n"
    "      \"examples\": [\n"
    "        { \"context\": \"casual check-in\",\n"
    "          \"incoming\": \"hey you up?\",\n"
    "          \"response\": \"yeah, what's up?\" },\n"
    "        { \"context\": \"quick thanks\",\n"
    "          \"incoming\": \"thx for the help earlier\",\n"
    "          \"response\": \"anytime 🙏\" },\n"
    "        { \"context\": \"running late\",\n"
    "          \"incoming\": \"i'm gonna be like 10 min late\",\n"
    "          \"response\": \"no worries, see you soon\" }\n"
    "      ]\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"telegram\",\n"
    "      \"examples\": [\n"
    "        { \"context\": \"news chat\",\n"
    "          \"incoming\": \"did you see the latest update?\",\n"
    "          \"response\": \"just read it. interesting take, "
    "though i'd want to see the data.\" },\n"
    "        { \"context\": \"making plans\",\n"
    "          \"incoming\": \"want to grab dinner tonight?\",\n"
    "          \"response\": \"i can do 7pm. any spot in mind?\" },\n"
    "        { \"context\": \"meeting recap\",\n"
    "          \"incoming\": \"how did the meeting go?\",\n"
    "          \"response\": \"longer than i wanted but they're "
    "on board with the proposal.\" }\n"
    "      ]\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"discord\",\n"
    "      \"examples\": [\n"
    "        { \"context\": \"game invite\",\n"
    "          \"incoming\": \"anyone wanna play tonight?\",\n"
    "          \"response\": \"i'm in 🎮 what are we playing?\" },\n"
    "        { \"context\": \"thanks for help\",\n"
    "          \"incoming\": \"thanks for helping me debug that\",\n"
    "          \"response\": \"np 🙌 hit me up anytime\" },\n"
    "        { \"context\": \"reacting to news\",\n"
    "          \"incoming\": \"they finally fixed the lag bug!\",\n"
    "          \"response\": \"about time 🔥 release notes drop today?\" }\n"
    "      ]\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"slack\",\n"
    "      \"examples\": [\n"
    "        { \"context\": \"PR review request\",\n"
    "          \"incoming\": \"Can you review my PR when you have a sec?\",\n"
    "          \"response\": \"Reviewing now \\u2014 I'll leave "
    "comments by EOD.\" },\n"
    "        { \"context\": \"calendar coordination\",\n"
    "          \"incoming\": \"Quick sync at 3?\",\n"
    "          \"response\": \"Works for me. I'll send the invite.\" },\n"
    "        { \"context\": \"deploy heads-up\",\n"
    "          \"incoming\": \"Heads up: the deploy is delayed an hour.\",\n"
    "          \"response\": \"Thanks for the flag. I'll let the "
    "team know in the channel.\" }\n"
    "      ]\n"
    "    },\n"
    "    {\n"
    "      \"channel\": \"cli\",\n"
    "      \"examples\": [\n"
    "        { \"context\": \"schedule lookup\",\n"
    "          \"incoming\": \"What do I have going on today?\",\n"
    "          \"response\": \"You have a team standup at 10am and a dentist "
    "appointment at 3pm. Want a reminder for the dentist?\" },\n"
    "        { \"context\": \"good news\",\n"
    "          \"incoming\": \"I got the promotion!\",\n"
    "          \"response\": \"Congratulations — well deserved. How are you "
    "planning to celebrate?\" },\n"
    "        { \"context\": \"drafting help\",\n"
    "          \"incoming\": \"Can you help me draft an email about the new "
    "project timeline?\",\n"
    "          \"response\": \"Sure — are timelines moving up or slipping, and "
    "do you want a casual update or a formal announcement?\" }\n"
    "      ]\n"
    "    }\n"
    "  ]\n"
    "}\n";

const char *hu_starter_persona_get(size_t *out_len) {
    /* Lazy one-time join of the two literal halves into a flat buffer.
     * Buffer size is generous to absorb future content growth without
     * another split. Not thread-safe on first call, but onboarding runs
     * single-threaded at startup. */
    static char buf[sizeof(hu_starter_persona_json_part1) + sizeof(hu_starter_persona_json_part2)];
    static size_t total_len;
    static bool initialized;
    if (!initialized) {
        size_t l1 = sizeof(hu_starter_persona_json_part1) - 1;
        size_t l2 = sizeof(hu_starter_persona_json_part2) - 1;
        memcpy(buf, hu_starter_persona_json_part1, l1);
        memcpy(buf + l1, hu_starter_persona_json_part2, l2);
        buf[l1 + l2] = '\0';
        total_len = l1 + l2;
        initialized = true;
    }
    if (out_len)
        *out_len = total_len;
    return buf;
}

/* US-9.2: Pure formatter for the post-wizard "What's next" block.
 *
 * Lives outside the HU_IS_TEST guard so unit tests
 * (tests/test_onboard_nextstep.c) can link against it without crossing
 * the wizard's interactive boundary. See sprints/sprint-9/designs/US-9.2.md
 * for the full truth table and rationale.
 *
 * Pattern: .claude/rules/security-predicate-extraction.md applied to UX
 * text — the "what message do we print?" decision is extracted into a
 * pure function so we don't have to spawn a subprocess and parse stdout
 * to test it. */
hu_error_t hu_onboard_nextstep_format(const hu_onboard_nextstep_ctx_t *ctx, char *out,
                                      size_t out_sz) {
    if (!ctx || !out || out_sz == 0 || !ctx->config_path)
        return HU_ERR_INVALID_ARGUMENT;

    int n = -1;
    if (ctx->already_exists) {
        if (ctx->platform_is_apple) {
            n = snprintf(out, out_sz,
                         "Config already exists at %s.\n"
                         "Run 'human doctor' to check status, or 'human doctor imessage' to pair "
                         "iMessage.\n",
                         ctx->config_path);
        } else {
            n = snprintf(out, out_sz,
                         "Config already exists at %s.\n"
                         "Run 'human doctor' to check status.\n",
                         ctx->config_path);
        }
    } else if (!ctx->parsed_ok) {
        n = snprintf(out, out_sz,
                     "Config written to %s.\n"
                     "Warning: config written but failed to parse — run 'human doctor --fix' to "
                     "repair\n",
                     ctx->config_path);
    } else if (ctx->platform_is_apple) {
        n = snprintf(out, out_sz,
                     "Config verified OK\n"
                     "Config written to %s.\n"
                     "What's next:\n"
                     "  1. Pair iMessage:  human doctor imessage\n"
                     "  2. Start the agent: human agent\n",
                     ctx->config_path);
    } else {
        n = snprintf(out, out_sz,
                     "Config verified OK\n"
                     "Config written to %s.\n"
                     "What's next:\n"
                     "  1. Start the agent: human agent\n"
                     "  (Tier-1 channels other than iMessage require manual config — see "
                     "docs/guides/channels.md)\n",
                     ctx->config_path);
    }

    if (n < 0 || (size_t)n >= out_sz)
        return HU_ERR_IO;
    return HU_OK;
}

#ifdef HU_IS_TEST
hu_error_t hu_onboard_run(hu_allocator_t *alloc) {
    (void)alloc;
    return HU_OK;
}
hu_error_t hu_onboard_run_with_args(hu_allocator_t *alloc, const char *cli_provider,
                                    const char *cli_api_key, bool apple_shortcut) {
    (void)alloc;
    (void)cli_provider;
    (void)cli_api_key;
    (void)apple_shortcut;
    return HU_OK;
}
#else

static const char *const HU_AGENTS_TEMPLATE = "# AGENTS.md — Project Agent Protocol\n"
                                              "## Build & Test\n"
                                              "- Build: `make` or `cmake .. && make`\n"
                                              "- Test: `make test`\n"
                                              "## Conventions\n"
                                              "- Follow existing code style\n"
                                              "- Write tests for new features\n"
                                              "- Keep commits focused\n";

static const char *const HU_USER_TEMPLATE = "# User Preferences\n"
                                            "## Communication\n"
                                            "- Be concise and direct\n"
                                            "- Show code examples when helpful\n"
                                            "## Expertise\n"
                                            "- Assume intermediate programming knowledge\n";

static const char *const HU_IDENTITY_TEMPLATE =
    "# Agent Identity\n"
    "name: Human\n"
    "description: Autonomous AI assistant running locally\n"
    "personality: Helpful, concise, security-conscious\n";

/* Duplicate definition removed — `hu_starter_persona_json` is the
 * single source of truth, defined above the HU_IS_TEST guard so
 * unit tests can link against it. */

static bool write_template_if_missing(const char *path, const char *content) {
    /* Previously: fopen("rb") existence check, then fopen("w") to
     * create — a classic TOCTOU race (CodeQL cpp/toctou-race-condition
     * at onboard.c:383). A malicious local process can win the race
     * and replace `path` with a symlink between the check and the
     * open, redirecting the write to an attacker-chosen file.
     *
     * Fix: do the "create only if missing" atomically with
     * O_CREAT|O_EXCL via plain open(). hu_io_secure_open uses O_TRUNC
     * (always-create semantics) so we can't reuse it here without
     * extending its API — do the open inline and keep this function
     * tight. */
#ifndef _WIN32
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
        return false;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return false;
    }
#else
    /* Windows: best-effort fall-back to the original two-step pattern;
     * the TOCTOU window is narrow and Windows ACLs cover the worst
     * abuse. */
    FILE *check = fopen(path, "rb");
    if (check) {
        fclose(check);
        return false;
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
#endif
    size_t len = strlen(content);
    if (fwrite(content, 1, len, f) != len) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static char *read_line(char *buf, size_t buf_size) {
    if (!fgets(buf, (int)buf_size, stdin))
        return NULL;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return buf;
}

static bool is_apple_provider(const char *provider) {
    return strcmp(provider, "apple") == 0;
}

hu_error_t hu_onboard_run(hu_allocator_t *alloc) {
    return hu_onboard_run_with_args(alloc, NULL, NULL, false);
}

hu_error_t hu_onboard_run_with_args(hu_allocator_t *alloc, const char *cli_provider,
                                    const char *cli_api_key, bool apple_shortcut) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    if (!hu_onboard_check_first_run()) {
        char existing_path[HU_MAX_PATH];
        const char *path_str =
            get_config_path(existing_path, sizeof(existing_path)) ? existing_path : "?";
        hu_onboard_nextstep_ctx_t nctx = {
            .config_path = path_str,
            .provider = NULL,
#if defined(__APPLE__)
            .platform_is_apple = true,
#else
            .platform_is_apple = false,
#endif
            .already_exists = true,
            .parsed_ok = false,
        };
        char nbuf[1024];
        if (hu_onboard_nextstep_format(&nctx, nbuf, sizeof(nbuf)) == HU_OK)
            fputs(nbuf, stdout);
        return HU_OK;
    }

    printf("Human Setup Wizard\n");
    printf("===================\n\n");

    char buf[512];
    char *line;
    const char *provider = NULL;
    const char *api_key = "";
    const char *model = NULL;

    if (apple_shortcut) {
        provider = "apple";
        model = "apple-foundationmodel";
    } else if (cli_provider) {
        provider = cli_provider;
    }

    if (!provider) {
#if defined(__APPLE__)
        static const hu_choice_t provider_choices[] = {
            {"MLX Local (fine-tuned Gemma, on-device)", "mlx_local", true},
#ifdef HU_ENABLE_APPLE_INTELLIGENCE
            {"Apple Intelligence (on-device, no API key)", "apple", false},
#endif
            {"Gemini (cloud)", "gemini", false},
            {"OpenAI (GPT-4, etc.)", "openai", false},
            {"Anthropic (Claude)", "anthropic", false},
            {"Ollama (local, no API key)", "ollama", false},
            {"OpenRouter", "openrouter", false},
        };
#else
        static const hu_choice_t provider_choices[] = {
            {"Ollama (local, no API key)", "ollama", true},
            {"Gemini (cloud)", "gemini", false},
            {"OpenAI (GPT-4, etc.)", "openai", false},
            {"Anthropic (Claude)", "anthropic", false},
            {"OpenRouter", "openrouter", false},
        };
#endif
        hu_choice_result_t provider_result;
        hu_error_t err = hu_choices_prompt("Choose your default provider:", provider_choices,
                                           sizeof(provider_choices) / sizeof(provider_choices[0]),
                                           &provider_result);
        provider = (err == HU_OK && provider_result.selected_value) ? provider_result.selected_value
                                                                    : provider_choices[0].value;
    }

    if (is_apple_provider(provider)) {
        model = "apple-foundationmodel";
        printf("\nApple Intelligence selected — no API key needed.\n");
        printf("Requires: macOS 26+, Apple Silicon, Apple Intelligence enabled.\n");
        printf("The human-ondevice server handles on-device inference automatically.\n\n");
    } else if (strcmp(provider, "mlx_local") == 0 || strcmp(provider, "mlx-local") == 0) {
        if (!model)
            model = "mlx-community/gemma-4-26b-a4b-it-4bit";
        printf("\nMLX Local selected — no API key needed.\n");
        printf("Fine-tuned Gemma model runs entirely on your Mac (Apple Silicon).\n");
        printf("The model server starts automatically when you run 'human agent'.\n\n");
    } else if (strcmp(provider, "ollama") == 0) {
        if (!model)
            model = "llama3.2";
        api_key = "";
        printf("\nOllama selected — no API key needed.\n");
        printf("Start the server: ollama serve\n");
        printf("Pull a model:     ollama pull llama3.2\n\n");
    } else {
        if (cli_api_key) {
            api_key = cli_api_key;
        } else {
            const char *env_hint = "OPENAI_API_KEY";
            if (strcmp(provider, "anthropic") == 0)
                env_hint = "ANTHROPIC_API_KEY";
            else if (strcmp(provider, "gemini") == 0)
                env_hint = "GOOGLE_APPLICATION_CREDENTIALS";

            printf("API key (or set %s env var): ", env_hint);
            fflush(stdout);
            line = read_line(buf, sizeof(buf));
            api_key = line && line[0] ? line : "";
        }

        if (!model) {
            const char *default_model = "gpt-4o";
            if (strcmp(provider, "anthropic") == 0)
                default_model = "claude-sonnet-4-20250514";
            else if (strcmp(provider, "gemini") == 0)
                default_model = "gemini-3.1-flash-lite-preview";
            else if (strcmp(provider, "ollama") == 0)
                default_model = "llama3";
            else if (strcmp(provider, "mlx_local") == 0 || strcmp(provider, "mlx-local") == 0)
                default_model = "mlx-community/gemma-4-26b-a4b-it-4bit";

            printf("Model (default: %s): ", default_model);
            fflush(stdout);
            line = read_line(buf, sizeof(buf));
            model = line && line[0] ? line : default_model;
        }
    }

    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    char config_dir[HU_MAX_PATH];
    int n = snprintf(config_dir, sizeof(config_dir), "%s/%s", home, HU_CONFIG_DIR);
    if (n <= 0 || (size_t)n >= sizeof(config_dir))
        return HU_ERR_IO;

#ifdef _WIN32
    (void)_mkdir(config_dir);
#else
    (void)mkdir(config_dir, 0700);
#endif

    char config_path[HU_MAX_PATH];
    n = snprintf(config_path, sizeof(config_path), "%s/%s", config_dir, HU_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= sizeof(config_path))
        return HU_ERR_IO;

    char *ws_dir = hu_sprintf(alloc, "%s/%s/workspace", home, HU_CONFIG_DIR);
    if (!ws_dir)
        return HU_ERR_OUT_OF_MEMORY;

#ifdef _WIN32
    (void)_mkdir(ws_dir);
#else
    (void)mkdir(ws_dir, 0700);
#endif

    /* Scaffold workspace templates (write only if missing) */
    {
        char tmpl_path[HU_MAX_PATH];
        int nr = snprintf(tmpl_path, sizeof(tmpl_path), "%s/AGENTS.md", ws_dir);
        if (nr > 0 && (size_t)nr < sizeof(tmpl_path) &&
            write_template_if_missing(tmpl_path, HU_AGENTS_TEMPLATE))
            printf("  Created %s\n", tmpl_path);
        nr = snprintf(tmpl_path, sizeof(tmpl_path), "%s/USER.md", ws_dir);
        if (nr > 0 && (size_t)nr < sizeof(tmpl_path) &&
            write_template_if_missing(tmpl_path, HU_USER_TEMPLATE))
            printf("  Created %s\n", tmpl_path);
        nr = snprintf(tmpl_path, sizeof(tmpl_path), "%s/IDENTITY.md", ws_dir);
        if (nr > 0 && (size_t)nr < sizeof(tmpl_path) &&
            write_template_if_missing(tmpl_path, HU_IDENTITY_TEMPLATE))
            printf("  Created %s\n", tmpl_path);
    }

    /* config.json contains an API key when the user types one in
     * interactively (line 410-412 below). Treat it as a secret so
     * the file is created mode 0600 even when the user hasn't pasted
     * a key — they might paste later via `human config edit` and the
     * mode persists. */
    FILE *f = NULL;
    if (hu_io_secure_open(config_path, HU_IO_PERM_SECRET, "w", &f) != HU_OK || !f) {
        alloc->free(alloc->ctx, ws_dir, strlen(ws_dir) + 1);
        return HU_ERR_IO;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"workspace\": \"%s\",\n", ws_dir);
    fprintf(f, "  \"default_provider\": \"%s\",\n", provider);
    fprintf(f, "  \"default_model\": \"%s\",\n", model);
    if (api_key[0])
        fprintf(f, "  \"providers\": [{\"name\": \"%s\", \"api_key\": \"%s\"}],\n", provider,
                api_key);
    else
        fprintf(f, "  \"providers\": [],\n");
    fprintf(f, "  \"agent\": {\"persona\": \"default\"},\n");
    fprintf(f, "  \"memory\": {\"backend\": \"sqlite\", \"auto_save\": true},\n");
    fprintf(f, "  \"gateway\": {\"port\": 3000, \"host\": \"127.0.0.1\"}\n");
    fprintf(f, "}\n");
    fclose(f);

#ifndef HU_IS_TEST
    {
        char persona_dir[HU_MAX_PATH];
        int pn = snprintf(persona_dir, sizeof(persona_dir), "%s/%s/personas", home, HU_CONFIG_DIR);
        if (pn > 0 && (size_t)pn < sizeof(persona_dir)) {
#ifdef _WIN32
            (void)_mkdir(persona_dir);
#else
            (void)mkdir(persona_dir, 0700);
#endif
            char persona_path[HU_MAX_PATH];
            pn = snprintf(persona_path, sizeof(persona_path), "%s/default.json", persona_dir);
            if (pn > 0 && (size_t)pn < sizeof(persona_path)) {
#ifdef _WIN32
                if (_access(persona_path, 0) != 0)
#else
                if (access(persona_path, F_OK) != 0)
#endif
                {
                    /* Persona file at ~/.human/personas/default.json.
                     * The path is env-derived; route through the
                     * secure helper. User-readable (0644) is fine —
                     * persona JSON contains no secrets. */
                    FILE *pf = NULL;
                    (void)hu_io_secure_open(persona_path, HU_IO_PERM_USER, "w", &pf);
                    if (pf) {
                        size_t plen = 0;
                        const char *json_str = hu_starter_persona_get(&plen);
                        if (fwrite(json_str, 1, plen, pf) == plen)
                            printf("Starter persona created at %s\n", persona_path);
                        fclose(pf);
                    }
                }
            }
        }
    }
#endif /* !HU_IS_TEST */

    alloc->free(alloc->ctx, ws_dir, strlen(ws_dir) + 1);

    /* US-9.2 AC-9.2.3: verify the freshly-written config parses cleanly.
     * If it doesn't, we still warn the user but return HU_ERR_IO so a
     * scripted caller can detect the bad-config state. */
    bool parsed_ok = false;
    {
        hu_config_t check_cfg;
        hu_error_t lerr = hu_config_load_from(alloc, config_path, &check_cfg);
        if (lerr == HU_OK) {
            parsed_ok = true;
            hu_config_deinit(&check_cfg);
        }
    }

    hu_onboard_nextstep_ctx_t nctx = {
        .config_path = config_path,
        .provider = provider,
#if defined(__APPLE__)
        .platform_is_apple = true,
#else
        .platform_is_apple = false,
#endif
        .already_exists = false,
        .parsed_ok = parsed_ok,
    };
    printf("\n");
    char nbuf[2048];
    if (hu_onboard_nextstep_format(&nctx, nbuf, sizeof(nbuf)) == HU_OK)
        fputs(nbuf, stdout);

    return parsed_ok ? HU_OK : HU_ERR_IO;
}
#endif
