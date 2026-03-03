#include "seaclaw/onboard.h"
#include "seaclaw/config.h"
#include "seaclaw/core/string.h"
#include "seaclaw/interactions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SC_CONFIG_DIR  ".seaclaw"
#define SC_CONFIG_FILE "config.json"
#define SC_MAX_PATH    1024

static char *get_config_path(char *buf, size_t buf_size) {
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    int n = snprintf(buf, buf_size, "%s/%s/%s", home, SC_CONFIG_DIR, SC_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= buf_size)
        return NULL;
    return buf;
}

bool sc_onboard_check_first_run(void) {
    char path[SC_MAX_PATH];
    if (!get_config_path(path, sizeof(path)))
        return true;
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return false;
    }
    return true;
}

#ifdef SC_IS_TEST
sc_error_t sc_onboard_run(sc_allocator_t *alloc) {
    (void)alloc;
    return SC_OK;
}
#else

static const char *const SC_AGENTS_TEMPLATE = "# AGENTS.md — Project Agent Protocol\n"
                                              "## Build & Test\n"
                                              "- Build: `make` or `cmake .. && make`\n"
                                              "- Test: `make test`\n"
                                              "## Conventions\n"
                                              "- Follow existing code style\n"
                                              "- Write tests for new features\n"
                                              "- Keep commits focused\n";

static const char *const SC_USER_TEMPLATE = "# User Preferences\n"
                                            "## Communication\n"
                                            "- Be concise and direct\n"
                                            "- Show code examples when helpful\n"
                                            "## Expertise\n"
                                            "- Assume intermediate programming knowledge\n";

static const char *const SC_IDENTITY_TEMPLATE =
    "# Agent Identity\n"
    "name: SeaClaw\n"
    "description: Autonomous AI assistant running locally\n"
    "personality: Helpful, concise, security-conscious\n";

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

sc_error_t sc_onboard_run(sc_allocator_t *alloc) {
    if (!alloc)
        return SC_ERR_INVALID_ARGUMENT;

    if (!sc_onboard_check_first_run()) {
        printf("Config already exists. Run 'seaclaw doctor' to check status.\n");
        return SC_OK;
    }

    printf("SeaClaw Setup Wizard\n");
    printf("===================\n\n");

    char buf[512];
    char *line;

    static const sc_choice_t provider_choices[] = {
        {"OpenAI (GPT-4, etc.)", "openai", true},
        {"Anthropic (Claude)", "anthropic", false},
        {"Ollama (local)", "ollama", false},
        {"OpenRouter", "openrouter", false},
    };
    sc_choice_result_t provider_result;
    sc_error_t err =
        sc_choices_prompt("Choose your default provider:", provider_choices,
                          sizeof(provider_choices) / sizeof(provider_choices[0]), &provider_result);
    const char *provider = (err == SC_OK && provider_result.selected_value)
                               ? provider_result.selected_value
                               : "openai";

    printf("API key (or set %s env var): ", "OPENAI_API_KEY");
    fflush(stdout);
    line = read_line(buf, sizeof(buf));
    const char *api_key = line && line[0] ? line : "";

    printf("Model (e.g. gpt-4o): ");
    fflush(stdout);
    line = read_line(buf, sizeof(buf));
    const char *model = line && line[0] ? line : "gpt-4o";

    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    char config_dir[SC_MAX_PATH];
    int n = snprintf(config_dir, sizeof(config_dir), "%s/%s", home, SC_CONFIG_DIR);
    if (n <= 0 || (size_t)n >= sizeof(config_dir))
        return SC_ERR_IO;

#ifdef _WIN32
    (void)_mkdir(config_dir);
#else
    (void)mkdir(config_dir, 0700);
#endif

    char config_path[SC_MAX_PATH];
    n = snprintf(config_path, sizeof(config_path), "%s/%s", config_dir, SC_CONFIG_FILE);
    if (n <= 0 || (size_t)n >= sizeof(config_path))
        return SC_ERR_IO;

    /* Minimal config JSON */
    char *ws_dir = sc_sprintf(alloc, "%s/%s/workspace", home, SC_CONFIG_DIR);
    if (!ws_dir)
        return SC_ERR_OUT_OF_MEMORY;

#ifdef _WIN32
    (void)_mkdir(ws_dir);
#else
    (void)mkdir(ws_dir, 0700);
#endif

    /* Scaffold workspace templates (write only if missing) */
    {
        char tmpl_path[SC_MAX_PATH];
        int n = snprintf(tmpl_path, sizeof(tmpl_path), "%s/AGENTS.md", ws_dir);
        if (n > 0 && (size_t)n < sizeof(tmpl_path) &&
            write_template_if_missing(tmpl_path, SC_AGENTS_TEMPLATE))
            printf("  Created %s\n", tmpl_path);
        n = snprintf(tmpl_path, sizeof(tmpl_path), "%s/USER.md", ws_dir);
        if (n > 0 && (size_t)n < sizeof(tmpl_path) &&
            write_template_if_missing(tmpl_path, SC_USER_TEMPLATE))
            printf("  Created %s\n", tmpl_path);
        n = snprintf(tmpl_path, sizeof(tmpl_path), "%s/IDENTITY.md", ws_dir);
        if (n > 0 && (size_t)n < sizeof(tmpl_path) &&
            write_template_if_missing(tmpl_path, SC_IDENTITY_TEMPLATE))
            printf("  Created %s\n", tmpl_path);
    }

    FILE *f = fopen(config_path, "w");
    if (!f) {
        alloc->free(alloc->ctx, ws_dir, strlen(ws_dir) + 1);
        return SC_ERR_IO;
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
    fprintf(f, "  \"memory\": {\"backend\": \"sqlite\", \"auto_save\": true},\n");
    fprintf(f, "  \"gateway\": {\"port\": 3000, \"host\": \"127.0.0.1\"}\n");
    fprintf(f, "}\n");
    fclose(f);
    alloc->free(alloc->ctx, ws_dir, strlen(ws_dir) + 1);

    printf("\nConfig written to %s\n", config_path);
    return SC_OK;
}
#endif
