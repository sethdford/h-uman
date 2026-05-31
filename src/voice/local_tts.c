#if defined(__linux__)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif
#endif
#include "human/voice/local_tts.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

hu_error_t hu_local_tts_build_body(hu_allocator_t *alloc, const hu_local_tts_config_t *config,
                                   const char *text, char **out_json, size_t *out_len) {
    if (!alloc || !config || !text || !text[0] || !out_json || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    *out_len = 0;

    hu_json_buf_t body = {0};
    if (hu_json_buf_init(&body, alloc) != HU_OK)
        return HU_ERR_OUT_OF_MEMORY;

    /* {"model":"<m>","voice":"<v>","input":"<text>"}. hu_json_append_string
     * emits the value ALREADY quoted+escaped, so each key prefix ends at the
     * colon and must NOT add its own opening quote — the prior inline builder
     * appended "model":" (trailing quote) + the quoted value, producing
     * malformed "model":""<m>"" and breaking every model/voice-configured
     * request (Kokoro requires `voice`, so that was every turnkey request). */
    if (hu_json_buf_append_raw(&body, "{", 1) != HU_OK)
        goto fail;
    if (config->model && config->model[0]) {
        if (hu_json_buf_append_raw(&body, "\"model\":", 8) != HU_OK)
            goto fail;
        if (hu_json_append_string(&body, config->model, strlen(config->model)) != HU_OK)
            goto fail;
        if (hu_json_buf_append_raw(&body, ",", 1) != HU_OK)
            goto fail;
    }
    if (config->voice && config->voice[0]) {
        if (hu_json_buf_append_raw(&body, "\"voice\":", 8) != HU_OK)
            goto fail;
        if (hu_json_append_string(&body, config->voice, strlen(config->voice)) != HU_OK)
            goto fail;
        if (hu_json_buf_append_raw(&body, ",", 1) != HU_OK)
            goto fail;
    }
    if (hu_json_buf_append_raw(&body, "\"input\":", 8) != HU_OK)
        goto fail;
    if (hu_json_append_string(&body, text, strlen(text)) != HU_OK)
        goto fail;
    if (hu_json_buf_append_raw(&body, "}", 1) != HU_OK)
        goto fail;

    size_t len = body.len;
    char *out = hu_strndup(alloc, body.ptr, len);
    hu_json_buf_free(&body);
    if (!out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_json = out;
    *out_len = len;
    return HU_OK;

fail:
    hu_json_buf_free(&body);
    return HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_local_tts_synthesize(hu_allocator_t *alloc, const hu_local_tts_config_t *config,
                                   const char *text, char **out_path) {
    if (!alloc || !config || !config->endpoint || !config->endpoint[0] || !out_path)
        return HU_ERR_INVALID_ARGUMENT;
    *out_path = NULL;
    if (!text || !text[0])
        return HU_ERR_INVALID_ARGUMENT;

#if HU_IS_TEST
    char tmpl[] = "/tmp/hu_lttsXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return HU_ERR_IO;
    (void)close(fd);
    size_t plen = strlen(tmpl);
    char *copy = hu_strndup(alloc, tmpl, plen);
    if (!copy) {
        (void)unlink(tmpl);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out_path = copy;
    return HU_OK;
#else
    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = hu_local_tts_build_body(alloc, config, text, &body, &body_len);
    if (err != HU_OK)
        return err;

    char *tmp_dir = hu_platform_get_temp_dir(alloc);
    if (!tmp_dir) {
        alloc->free(alloc->ctx, body, body_len + 1);
        return HU_ERR_IO;
    }
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
    int pid = (int)getpid();
#else
    int pid = 0;
#endif
    char json_path[256];
    int n = snprintf(json_path, sizeof(json_path), "%s/hu_ltts_%d.json", tmp_dir, pid);
    alloc->free(alloc->ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(json_path)) {
        alloc->free(alloc->ctx, body, body_len + 1);
        return HU_ERR_IO;
    }

    FILE *jf = fopen(json_path, "wb");
    if (!jf) {
        alloc->free(alloc->ctx, body, body_len + 1);
        return HU_ERR_IO;
    }
    if (fwrite(body, 1, body_len, jf) != body_len) {
        fclose(jf);
        unlink(json_path);
        alloc->free(alloc->ctx, body, body_len + 1);
        return HU_ERR_IO;
    }
    fclose(jf);
    alloc->free(alloc->ctx, body, body_len + 1);

    char out_tmpl[] = "/tmp/hu_ltts_outXXXXXX";
    int out_fd = mkstemp(out_tmpl);
    if (out_fd < 0) {
        unlink(json_path);
        return HU_ERR_IO;
    }
    (void)close(out_fd);

    char data_arg[280];
    n = snprintf(data_arg, sizeof(data_arg), "@%s", json_path);
    if (n < 0 || (size_t)n >= sizeof(data_arg)) {
        unlink(json_path);
        unlink(out_tmpl);
        return HU_ERR_IO;
    }

    const char *argv[] = {
        "curl",   "-s", "-X",     "POST",           "-H", "Content-Type: application/json", "-d",
        data_arg, "-o", out_tmpl, config->endpoint, NULL};

    hu_run_result_t run = {0};
    err = hu_process_run(alloc, argv, NULL, 256, &run);
    unlink(json_path);
    if (err != HU_OK) {
        unlink(out_tmpl);
        hu_run_result_free(alloc, &run);
        return err;
    }
    hu_run_result_free(alloc, &run);

    size_t olen = strlen(out_tmpl);
    char *pcopy = hu_strndup(alloc, out_tmpl, olen);
    if (!pcopy) {
        unlink(out_tmpl);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out_path = pcopy;
    return HU_OK;
#endif
}
