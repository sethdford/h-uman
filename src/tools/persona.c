#include "seaclaw/tools/persona.h"
#include "seaclaw/agent.h"
#include "seaclaw/agent/tool_context.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/persona.h"
#include "seaclaw/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <unistd.h>
#endif

#define PERSONA_PARAMS                                                                   \
    "{\"type\":\"object\",\"properties\":{"                                              \
    "\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"update\",\"show\",\"list\"," \
    "\"delete\",\"switch\",\"feedback\"]},"                                              \
    "\"name\":{\"type\":\"string\"},"                                                    \
    "\"source\":{\"type\":\"string\",\"enum\":[\"imessage\",\"gmail\",\"facebook\"]},"   \
    "\"channel\":{\"type\":\"string\"},"                                                 \
    "\"patch\":{\"type\":\"object\"},"                                                   \
    "\"original\":{\"type\":\"string\"},"                                                \
    "\"corrected\":{\"type\":\"string\"},"                                               \
    "\"context\":{\"type\":\"string\"}"                                                  \
    "},\"required\":[\"action\"]}"

#define SC_PERSONA_PATH_MAX 512

typedef struct {
    sc_allocator_t *alloc;
} persona_tool_ctx_t;

#if !SC_IS_TEST
static const char *persona_dir_path(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !home[0])
        home = ".";
    int n = snprintf(buf, cap, "%s/.seaclaw/personas", home);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}
#endif

static sc_error_t do_show(sc_allocator_t *alloc, const char *name, sc_tool_result_t *out) {
#if SC_IS_TEST
    (void)alloc;
    (void)name;
    *out = sc_tool_result_ok("Persona show requires non-test mode (file access)", 47);
    return SC_OK;
#else
    if (!name || !name[0]) {
        *out = sc_tool_result_fail("name required for show", 22);
        return SC_OK;
    }
    sc_persona_t p = {0};
    sc_error_t err = sc_persona_load(alloc, name, strlen(name), &p);
    if (err != SC_OK) {
        char *msg = sc_sprintf(alloc, "Persona not found: %s", name);
        if (msg) {
            *out = sc_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = sc_tool_result_fail("Persona not found", 17);
        }
        return SC_OK;
    }
    char *prompt = NULL;
    size_t prompt_len = 0;
    err = sc_persona_build_prompt(alloc, &p, NULL, 0, &prompt, &prompt_len);
    sc_persona_deinit(alloc, &p);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("failed to build prompt", 22);
        return SC_OK;
    }
    *out = sc_tool_result_ok_owned(prompt, prompt_len);
    return SC_OK;
#endif
}

static sc_error_t do_list(sc_allocator_t *alloc, sc_tool_result_t *out) {
#if SC_IS_TEST
    (void)alloc;
    *out = sc_tool_result_ok("[]", 2);
    return SC_OK;
#else
#if defined(__unix__) || defined(__APPLE__)
    char dir_buf[SC_PERSONA_PATH_MAX];
    const char *dir = persona_dir_path(dir_buf, sizeof(dir_buf));
    if (!dir) {
        *out = sc_tool_result_fail("could not resolve persona dir", 30);
        return SC_OK;
    }
    DIR *d = opendir(dir);
    if (!d) {
        *out = sc_tool_result_ok("[]", 2);
        return SC_OK;
    }
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, alloc);
    sc_json_buf_append_raw(&buf, "[", 1);
    bool first = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '\0' || e->d_name[0] == '.')
            continue;
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0)
            continue;
        char name[256];
        size_t name_len = len - 5;
        if (name_len >= sizeof(name))
            continue;
        memcpy(name, e->d_name, name_len);
        name[name_len] = '\0';
        if (!first)
            sc_json_buf_append_raw(&buf, ",", 1);
        sc_json_buf_append_raw(&buf, "\"", 1);
        sc_json_append_string(&buf, name, name_len);
        sc_json_buf_append_raw(&buf, "\"", 1);
        first = false;
    }
    closedir(d);
    sc_json_buf_append_raw(&buf, "]", 1);
    char *result =
        (buf.ptr && buf.len > 0) ? sc_strndup(alloc, buf.ptr, buf.len) : sc_strdup(alloc, "[]");
    sc_json_buf_free(&buf);
    if (!result) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_OK;
    }
    *out = sc_tool_result_ok_owned(result, strlen(result));
    return SC_OK;
#else
    (void)alloc;
    *out = sc_tool_result_ok("[]", 2);
    return SC_OK;
#endif
#endif
}

static sc_error_t do_delete(sc_allocator_t *alloc, const char *name, sc_tool_result_t *out) {
#if SC_IS_TEST
    (void)alloc;
    (void)name;
    *out = sc_tool_result_ok("Persona delete skipped in test mode", 35);
    return SC_OK;
#else
    if (!name || !name[0]) {
        *out = sc_tool_result_fail("name required for delete", 24);
        return SC_OK;
    }
    char path[SC_PERSONA_PATH_MAX];
    const char *dir = persona_dir_path(path, sizeof(path));
    if (!dir) {
        *out = sc_tool_result_fail("could not resolve persona dir", 30);
        return SC_OK;
    }
    int n = snprintf(path, sizeof(path), "%s/%s.json", dir, name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        *out = sc_tool_result_fail("invalid name", 12);
        return SC_OK;
    }
    if (unlink(path) != 0) {
        char *msg = sc_sprintf(alloc, "Failed to delete: %s", name);
        if (msg) {
            *out = sc_tool_result_fail_owned(msg, strlen(msg));
        } else {
            *out = sc_tool_result_fail("delete failed", 13);
        }
        return SC_OK;
    }
    char *result = sc_sprintf(alloc, "{\"deleted\":\"%s\"}", name);
    *out = sc_tool_result_ok_owned(result, result ? strlen(result) : 0);
    return SC_OK;
#endif
}

static sc_error_t do_switch(sc_allocator_t *alloc, const char *name, sc_tool_result_t *out) {
    sc_agent_t *agent = sc_agent_get_current_for_tools();
    if (!agent) {
        *out = sc_tool_result_fail("No active agent", 16);
        return SC_OK;
    }
    size_t name_len = name ? strlen(name) : 0;
    sc_error_t err = sc_agent_set_persona(agent, name, name_len);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Failed to switch persona", 23);
        return SC_OK;
    }
    if (name && name_len > 0) {
        char *msg = sc_sprintf(alloc, "Switched to persona: %s", name);
        if (msg) {
            *out = sc_tool_result_ok_owned(msg, strlen(msg));
        } else {
            *out = sc_tool_result_ok("Persona switched", 16);
        }
    } else {
        *out = sc_tool_result_ok("Persona cleared", 15);
    }
    return SC_OK;
}

static sc_error_t do_feedback(sc_allocator_t *alloc, const char *persona_name, const char *original,
                              const char *corrected, const char *context, const char *channel,
                              sc_tool_result_t *out) {
    const char *name = persona_name;
    if (!name || !name[0]) {
        sc_agent_t *agent = sc_agent_get_current_for_tools();
        if (agent && agent->persona_name && agent->persona_name_len > 0) {
            name = agent->persona_name;
        }
    }
    if (!name || !name[0]) {
        *out = sc_tool_result_fail("name required for feedback (or no active persona)", 45);
        return SC_OK;
    }
    if (!corrected || !corrected[0]) {
        *out = sc_tool_result_fail("corrected required for feedback", 31);
        return SC_OK;
    }
    sc_persona_feedback_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.channel = channel && channel[0] ? channel : "cli";
    fb.channel_len = strlen(fb.channel);
    fb.original_response = original ? original : "";
    fb.original_response_len = original ? strlen(original) : 0;
    fb.corrected_response = corrected;
    fb.corrected_response_len = strlen(corrected);
    fb.context = context ? context : "";
    fb.context_len = context ? strlen(context) : 0;

    sc_error_t err = sc_persona_feedback_record(alloc, persona_name, strlen(persona_name), &fb);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("Failed to record feedback", 26);
        return SC_OK;
    }
    char *msg = sc_sprintf(alloc, "Feedback recorded for persona: %s", name);
    if (msg) {
        *out = sc_tool_result_ok_owned(msg, strlen(msg));
    } else {
        *out = sc_tool_result_ok("Feedback recorded", 16);
    }
    return SC_OK;
}

static sc_error_t persona_execute(void *ctx, sc_allocator_t *alloc, const sc_json_value_t *args,
                                  sc_tool_result_t *out) {
    (void)ctx;
    if (!out)
        return SC_ERR_INVALID_ARGUMENT;
    if (!args || args->type != SC_JSON_OBJECT) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_OK;
    }

    const char *action = sc_json_get_string(args, "action");
    if (!action || !action[0]) {
        *out = sc_tool_result_fail("action required", 15);
        return SC_OK;
    }

    const char *name = sc_json_get_string(args, "name");

    if (strcmp(action, "create") == 0 || strcmp(action, "update") == 0) {
        char *msg = sc_sprintf(
            alloc, "Persona %s requires running `seaclaw persona %s` CLI command", action, action);
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
        return SC_OK;
    }
    if (strcmp(action, "show") == 0)
        return do_show(alloc, name, out);
    if (strcmp(action, "list") == 0)
        return do_list(alloc, out);
    if (strcmp(action, "delete") == 0)
        return do_delete(alloc, name, out);
    if (strcmp(action, "switch") == 0)
        return do_switch(alloc, name, out);
    if (strcmp(action, "feedback") == 0) {
        const char *original = sc_json_get_string(args, "original");
        const char *corrected = sc_json_get_string(args, "corrected");
        const char *context = sc_json_get_string(args, "context");
        const char *channel = sc_json_get_string(args, "channel");
        return do_feedback(alloc, name, original, corrected, context, channel, out);
    }

    *out = sc_tool_result_fail("unknown action", 14);
    return SC_OK;
}

static const char *persona_name(void *ctx) {
    (void)ctx;
    return "persona";
}
static const char *persona_desc(void *ctx) {
    (void)ctx;
    return "Create and manage persona profiles for personality cloning";
}
static const char *persona_params(void *ctx) {
    (void)ctx;
    return PERSONA_PARAMS;
}
static void persona_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(persona_tool_ctx_t));
}

static const sc_tool_vtable_t persona_vtable = {
    .execute = persona_execute,
    .name = persona_name,
    .description = persona_desc,
    .parameters_json = persona_params,
    .deinit = persona_deinit,
};

sc_error_t sc_persona_tool_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out)
        return SC_ERR_INVALID_ARGUMENT;
    persona_tool_ctx_t *ctx =
        (persona_tool_ctx_t *)alloc->alloc(alloc->ctx, sizeof(persona_tool_ctx_t));
    if (!ctx)
        return SC_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = alloc;
    out->ctx = ctx;
    out->vtable = &persona_vtable;
    return SC_OK;
}
