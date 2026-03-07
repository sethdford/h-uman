/*
 * skill_write — create a new skill that the agent can use in future turns.
 * Under SC_IS_TEST, returns success without writing to disk.
 */
#include "seaclaw/tools/skill_write.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SC_SKILL_WRITE_NAME_MAX 64

#define TOOL_NAME "skill_write"
#define TOOL_DESC "Create a new skill that the agent can use in future turns"
#define TOOL_PARAMS                                                                       \
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\","                 \
    "\"description\":\"Skill name, alphanumeric + underscore only\"},\"description\":{"   \
    "\"type\":\"string\",\"description\":\"What the skill does\"},\"command\":{\"type\":" \
    "\"string\",\"description\":\"Shell command to execute when the skill is invoked\"}," \
    "\"parameters\":{\"type\":\"string\",\"description\":\"JSON schema string for skill " \
    "parameters\"}},\"required\":[\"name\",\"description\",\"command\"]}"

typedef struct {
    char _unused;
} skill_write_ctx_t;

static bool is_valid_name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool validate_name(const char *name, size_t len) {
    if (!name || len == 0 || len > SC_SKILL_WRITE_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!is_valid_name_char(name[i]))
            return false;
    }
    return true;
}

static sc_error_t skill_write_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                      sc_tool_result_t *out) {
    (void)ctx;
    if (!args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }

    const char *name = sc_json_get_string(args, "name");
    const char *description = sc_json_get_string(args, "description");
    const char *command = sc_json_get_string(args, "command");
    const char *parameters = sc_json_get_string(args, "parameters");

    if (!name || strlen(name) == 0) {
        *out = sc_tool_result_fail("missing name", 12);
        return SC_OK;
    }
    if (!description || strlen(description) == 0) {
        *out = sc_tool_result_fail("missing description", 18);
        return SC_OK;
    }
    if (!command || strlen(command) == 0) {
        *out = sc_tool_result_fail("missing command", 15);
        return SC_OK;
    }

    size_t name_len = strlen(name);
    if (!validate_name(name, name_len)) {
        if (name_len > SC_SKILL_WRITE_NAME_MAX) {
            *out = sc_tool_result_fail("name too long (max 64 chars)", 28);
        } else {
            *out = sc_tool_result_fail("invalid name: alphanumeric and underscore only", 47);
        }
        return SC_OK;
    }

#ifdef SC_IS_TEST
    {
        char *msg = sc_sprintf(alloc, "Skill '%s' created", name);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 13);
            return SC_ERR_OUT_OF_MEMORY;
        }
        size_t len = strlen(msg);
        *out = sc_tool_result_ok_owned(msg, len);
    }
    return SC_OK;
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        *out = sc_tool_result_fail("HOME not set", 13);
        return SC_OK;
    }

#ifndef _WIN32
    char base_dir[512];
    int n = snprintf(base_dir, sizeof(base_dir), "%s/.seaclaw/skills", home);
    if (n <= 0 || (size_t)n >= sizeof(base_dir)) {
        *out = sc_tool_result_fail("path too long", 13);
        return SC_OK;
    }
    mkdir(base_dir, 0755);

    char dir_path[512];
    n = snprintf(dir_path, sizeof(dir_path), "%s/.seaclaw/skills/%.*s", home, (int)name_len, name);
    if (n <= 0 || (size_t)n >= sizeof(dir_path)) {
        *out = sc_tool_result_fail("path too long", 13);
        return SC_OK;
    }
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        *out = sc_tool_result_fail("failed to create skill directory", 32);
        return SC_OK;
    }

    /* Build manifest: {"name":"...","description":"...","command":"...","parameters":...} */
    sc_json_value_t *manifest = sc_json_object_new(alloc);
    if (!manifest) {
        *out = sc_tool_result_fail("out of memory", 13);
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_json_object_set(alloc, manifest, "name", sc_json_string_new(alloc, name, name_len));
    sc_json_object_set(alloc, manifest, "description",
                       sc_json_string_new(alloc, description, strlen(description)));
    sc_json_object_set(alloc, manifest, "command",
                       sc_json_string_new(alloc, command, strlen(command)));

    sc_json_value_t *params_val = NULL;
    if (parameters && parameters[0]) {
        sc_json_value_t *parsed = NULL;
        if (sc_json_parse(alloc, parameters, strlen(parameters), &parsed) == SC_OK && parsed) {
            params_val = parsed;
        }
    }
    if (!params_val) {
        params_val = sc_json_object_new(alloc);
    }
    sc_json_object_set(alloc, manifest, "parameters", params_val);

    char *json_str = NULL;
    size_t json_len = 0;
    sc_error_t err = sc_json_stringify(alloc, manifest, &json_str, &json_len);
    sc_json_free(alloc, manifest);
    if (err != SC_OK || !json_str) {
        *out = sc_tool_result_fail("failed to build manifest", 24);
        return SC_OK;
    }

    char manifest_path[512];
    n = snprintf(manifest_path, sizeof(manifest_path), "%s/.seaclaw/skills/%.*s/manifest.json",
                 home, (int)name_len, name);
    if (n <= 0 || (size_t)n >= sizeof(manifest_path)) {
        alloc->free(alloc->ctx, json_str, json_len + 1);
        *out = sc_tool_result_fail("path too long", 13);
        return SC_OK;
    }

    FILE *f = fopen(manifest_path, "wb");
    if (!f) {
        alloc->free(alloc->ctx, json_str, json_len + 1);
        *out = sc_tool_result_fail("failed to write manifest.json", 29);
        return SC_OK;
    }
    size_t written = fwrite(json_str, 1, json_len, f);
    fclose(f);
    alloc->free(alloc->ctx, json_str, json_len + 1);
    if (written != json_len) {
        remove(manifest_path);
        *out = sc_tool_result_fail("failed to write manifest.json", 29);
        return SC_OK;
    }

    {
        char *msg = sc_sprintf(alloc, "Skill '%s' created", name);
        if (!msg) {
            *out = sc_tool_result_fail("out of memory", 13);
            return SC_ERR_OUT_OF_MEMORY;
        }
        size_t len = strlen(msg);
        *out = sc_tool_result_ok_owned(msg, len);
    }
    return SC_OK;
#else  /* _WIN32 */
    (void)parameters;
    *out = sc_tool_result_fail("skill_write not supported on Windows", 34);
    return SC_OK;
#endif /* _WIN32 */
#endif /* SC_IS_TEST */
}

static const char *skill_write_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *skill_write_description(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *skill_write_parameters_json(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}

static const sc_tool_vtable_t skill_write_vtable = {
    .execute = skill_write_execute,
    .name = skill_write_name,
    .description = skill_write_description,
    .parameters_json = skill_write_parameters_json,
    .deinit = NULL,
};

sc_error_t sc_skill_write_create(sc_allocator_t *alloc, sc_tool_t *out) {
    (void)alloc;
    if (!out)
        return SC_ERR_INVALID_ARGUMENT;
    static skill_write_ctx_t ctx = {0};
    out->ctx = &ctx;
    out->vtable = &skill_write_vtable;
    return SC_OK;
}
