/*
 * Embedded model provider — local inference via llama.cpp/llama-cli.
 * Requires HU_ENABLE_EMBEDDED_MODEL and HU_GATEWAY_POSIX for production.
 * Without them, chat_with_system returns HU_ERR_NOT_SUPPORTED.
 */
#include "human/providers/embedded.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/paths.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/platform.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HU_ENABLE_EMBEDDED_MODEL
#ifdef HU_GATEWAY_POSIX
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#endif

#define EMBEDDED_PROMPT_MAX 32768
#define EMBEDDED_LLAMA_CLI  "llama-cli"

typedef struct {
    hu_embedded_config_t config;
} embedded_ctx_t;

#if defined(HU_ENABLE_EMBEDDED_MODEL) && defined(HU_GATEWAY_POSIX) && !HU_IS_TEST
static const char *find_model_path(hu_allocator_t *alloc, const hu_embedded_config_t *config,
                                   char *buf, size_t buf_size) {
    if (!alloc || !buf || buf_size < 2)
        return NULL;

    /* Prefer config.model_path if set and readable */
    if (config->model_path && config->model_path[0]) {
        if (access(config->model_path, R_OK) == 0) {
            size_t len = strlen(config->model_path);
            if (len < buf_size) {
                memcpy(buf, config->model_path, len + 1);
                return buf;
            }
        }
        return NULL;
    }

    /* Fallback: first .gguf in ~/.human/models/ */
    char *home = hu_platform_get_home_dir(alloc);
    if (!home)
        return NULL;

    int n = hu_paths_state(buf, buf_size, "models");
    if (n <= 0 || (size_t)n >= buf_size) {
        alloc->free(alloc->ctx, home, strlen(home) + 1);
        return NULL;
    }

    DIR *d = opendir(buf);
    if (!d) {
        alloc->free(alloc->ctx, home, strlen(home) + 1);
        return NULL;
    }

    const char *found = NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        size_t nlen = strlen(e->d_name);
        if (nlen < 5)
            continue;
        if (strcmp(e->d_name + nlen - 5, ".gguf") != 0)
            continue;

        n = hu_paths_state(buf, buf_size, "models/%s", e->d_name);
        if (n > 0 && (size_t)n < buf_size && access(buf, R_OK) == 0)
            found = buf;
        break;
    }
    closedir(d);
    alloc->free(alloc->ctx, home, strlen(home) + 1);
    return found;
}
#endif

static hu_error_t embedded_chat_with_system(void *ctx, hu_allocator_t *alloc,
                                            const char *system_prompt, size_t system_prompt_len,
                                            const char *message, size_t message_len,
                                            const char *model, size_t model_len, double temperature,
                                            char **out, size_t *out_len) {
    (void)model;
    (void)model_len;
    (void)temperature;

    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (!message || message_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    (void)ctx;
    (void)system_prompt;
    (void)system_prompt_len;
    const char *resp = "Mock embedded response";
    size_t rlen = strlen(resp);
    *out = alloc->alloc(alloc->ctx, rlen + 1);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(*out, resp, rlen + 1);
    *out_len = rlen;
    return HU_OK;
#else
#if defined(HU_ENABLE_EMBEDDED_MODEL) && defined(HU_GATEWAY_POSIX)
    {
        embedded_ctx_t *ec = (embedded_ctx_t *)ctx;
        if (!ec)
            return HU_ERR_INVALID_ARGUMENT;

        char model_path_buf[1024];
        const char *model_path =
            find_model_path(alloc, &ec->config, model_path_buf, sizeof(model_path_buf));
        if (!model_path) {
            /* No model file found. Return NOT_SUPPORTED; user should place a .gguf
             * in ~/.human/models/ or configure model_path. */
            return HU_ERR_NOT_SUPPORTED;
        }

        /* Build combined prompt: system + user */
        char prompt_buf[EMBEDDED_PROMPT_MAX];
        size_t prompt_len;
        if (system_prompt && system_prompt_len > 0) {
            if (system_prompt_len + message_len + 4 >= sizeof(prompt_buf))
                return HU_ERR_INVALID_ARGUMENT;
            memcpy(prompt_buf, system_prompt, system_prompt_len);
            prompt_buf[system_prompt_len] = '\n';
            prompt_buf[system_prompt_len + 1] = '\n';
            memcpy(prompt_buf + system_prompt_len + 2, message, message_len);
            prompt_len = system_prompt_len + 2 + message_len;
        } else {
            if (message_len >= sizeof(prompt_buf))
                return HU_ERR_INVALID_ARGUMENT;
            memcpy(prompt_buf, message, message_len);
            prompt_len = message_len;
        }
        prompt_buf[prompt_len] = '\0';

        /* argv: llama-cli -m model_path -p "prompt" */
        const char *argv[] = {EMBEDDED_LLAMA_CLI, "-m", model_path, "-p", prompt_buf, NULL};

        hu_run_result_t result = {0};
        hu_error_t err = hu_process_run(alloc, argv, NULL, 4 * 1024 * 1024, &result);
        if (err != HU_OK) {
            hu_run_result_free(alloc, &result);
            return err;
        }
        if (!result.success || !result.stdout_buf) {
            hu_run_result_free(alloc, &result);
            return HU_ERR_PROVIDER_RESPONSE;
        }

        size_t len = result.stdout_len;
        while (len > 0 &&
               (result.stdout_buf[len - 1] == ' ' || result.stdout_buf[len - 1] == '\t' ||
                result.stdout_buf[len - 1] == '\r' || result.stdout_buf[len - 1] == '\n'))
            len--;

        *out = hu_strndup(alloc, result.stdout_buf, len);
        hu_run_result_free(alloc, &result);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = len;
        return HU_OK;
    }
#else
    (void)ctx;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#endif
#endif
}

static const char *embedded_get_name(void *ctx) {
    (void)ctx;
    return "embedded";
}
static bool embedded_supports_native_tools(void *ctx) {
    (void)ctx;
    return false;
}
static void embedded_deinit(void *ctx, hu_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(embedded_ctx_t));
}

static const hu_provider_vtable_t embedded_vtable = {
    .chat_with_system = embedded_chat_with_system,
    .chat = NULL,
    .get_name = embedded_get_name,
    .supports_native_tools = embedded_supports_native_tools,
    .deinit = embedded_deinit,
};

hu_error_t hu_embedded_provider_create(hu_allocator_t *alloc, const hu_embedded_config_t *config,
                                       hu_provider_t *out) {
    if (!alloc || !config || !out)
        return HU_ERR_INVALID_ARGUMENT;
    embedded_ctx_t *ctx = alloc->alloc(alloc->ctx, sizeof(embedded_ctx_t));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->config = *config;
    out->ctx = ctx;
    out->vtable = &embedded_vtable;
    return HU_OK;
}
