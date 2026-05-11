#include "human/onboard.h"
#include "human/config.h"
#include "human/core/string.h"
#include "human/interactions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
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

/* Single source of truth — declared in include/human/onboard.h and
 * referenced by both `human init` (src/cli_commands.c) and
 * `human onboard` (this file). Defined outside the HU_IS_TEST guard
 * so unit tests (which set HU_IS_TEST) can still link against this
 * symbol — the literal is bytes-on-disk-equivalent in both builds. */
const char hu_starter_persona_json[] =
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
    "  }\n"
    "}\n";

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
    FILE *check = fopen(path, "rb");
    if (check) {
        fclose(check);
        return false;
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
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
        printf("Config already exists. Run 'human doctor' to check status.\n");
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
            {"Ollama (local)", "ollama", false},
            {"OpenRouter", "openrouter", false},
        };
#else
        static const hu_choice_t provider_choices[] = {
            {"Gemini (cloud)", "gemini", true},         {"OpenAI (GPT-4, etc.)", "openai", false},
            {"Anthropic (Claude)", "anthropic", false}, {"Ollama (local)", "ollama", false},
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

    FILE *f = fopen(config_path, "w");
    if (!f) {
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
                    FILE *pf = fopen(persona_path, "w");
                    if (pf) {
                        size_t plen = strlen(hu_starter_persona_json);
                        if (fwrite(hu_starter_persona_json, 1, plen, pf) == plen)
                            printf("Starter persona created at %s\n", persona_path);
                        fclose(pf);
                    }
                }
            }
        }
    }
#endif /* !HU_IS_TEST */

    alloc->free(alloc->ctx, ws_dir, strlen(ws_dir) + 1);

    printf("\nConfig written to %s\n", config_path);
    if (is_apple_provider(provider))
        printf("Run 'human agent' to start chatting with Apple Intelligence.\n");
    else
        printf("Run 'human agent' to start chatting.\n");
    return HU_OK;
}
#endif
