#include "human/voice/local_stt.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

/* Append one curl multipart field "key=prefixvalue" to req, recording the heap
 * buffer in req->owned so hu_local_stt_request_free can release it. */
static hu_error_t stt_add_field(hu_allocator_t *alloc, hu_local_stt_request_t *req, const char *key,
                                const char *prefix, const char *value) {
    if (req->owned_count >= HU_LOCAL_STT_MAX_OWNED)
        return HU_ERR_INVALID_ARGUMENT;
    size_t cap = strlen(key) + 1 /* '=' */ + strlen(prefix) + strlen(value) + 1 /* NUL */;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    int n = snprintf(buf, cap, "%s=%s%s", key, prefix, value);
    if (n < 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return HU_ERR_INVALID_ARGUMENT;
    }
    req->owned[req->owned_count] = buf;
    req->owned_sizes[req->owned_count] = cap;
    req->owned_count++;
    return HU_OK;
}

hu_error_t hu_local_stt_build_request(hu_allocator_t *alloc, const hu_local_stt_config_t *config,
                                      const char *audio_path, hu_local_stt_request_t *out_req) {
    if (!alloc || !config || !config->endpoint || !config->endpoint[0] || !audio_path ||
        !audio_path[0] || !out_req)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_req, 0, sizeof(*out_req));

    hu_error_t err;
    /* file=@<path> — the whisper.cpp /inference (and OpenAI-compatible) field. */
    if ((err = stt_add_field(alloc, out_req, "file", "@", audio_path)) != HU_OK)
        goto fail;
    /* Pin json so the response is {"text": ...}; don't depend on the server's
     * default response_format. Valid for whisper.cpp and OpenAI-compatible. */
    if ((err = stt_add_field(alloc, out_req, "response_format", "", "json")) != HU_OK)
        goto fail;
    /* whisper.cpp loads its model at startup (-m) and ignores a per-request
     * model; only OpenAI-compatible servers need it. Emit it only when set. */
    if (config->model && config->model[0]) {
        if ((err = stt_add_field(alloc, out_req, "model", "", config->model)) != HU_OK)
            goto fail;
    }
    if (config->language && config->language[0]) {
        if ((err = stt_add_field(alloc, out_req, "language", "", config->language)) != HU_OK)
            goto fail;
    }

    /* curl -s -X POST {-F <field>}... <endpoint> NULL. owned_count is bounded by
     * HU_LOCAL_STT_MAX_OWNED, so this never exceeds HU_LOCAL_STT_MAX_ARGV. */
    size_t argc = 0;
    out_req->argv[argc++] = "curl";
    out_req->argv[argc++] = "-s";
    out_req->argv[argc++] = "-X";
    out_req->argv[argc++] = "POST";
    for (size_t i = 0; i < out_req->owned_count; i++) {
        out_req->argv[argc++] = "-F";
        out_req->argv[argc++] = out_req->owned[i];
    }
    out_req->argv[argc++] = config->endpoint;
    out_req->argv[argc] = NULL;
    out_req->argc = argc;
    return HU_OK;

fail:
    hu_local_stt_request_free(alloc, out_req);
    return err;
}

void hu_local_stt_request_free(hu_allocator_t *alloc, hu_local_stt_request_t *req) {
    if (!alloc || !req)
        return;
    for (size_t i = 0; i < req->owned_count; i++) {
        if (req->owned[i])
            alloc->free(alloc->ctx, req->owned[i], req->owned_sizes[i]);
        req->owned[i] = NULL;
        req->owned_sizes[i] = 0;
    }
    req->owned_count = 0;
    req->argc = 0;
}

hu_error_t hu_local_stt_transcribe(hu_allocator_t *alloc, const hu_local_stt_config_t *config,
                                   const char *audio_path, char **out_text, size_t *out_len) {
    if (!alloc || !config || !config->endpoint || !config->endpoint[0] || !audio_path ||
        !audio_path[0] || !out_text || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_text = NULL;
    *out_len = 0;

#if HU_IS_TEST
    (void)audio_path;
    const char *mock = "Hello world";
    size_t mlen = strlen(mock);
    char *dup = hu_strndup(alloc, mock, mlen);
    if (!dup)
        return HU_ERR_OUT_OF_MEMORY;
    *out_text = dup;
    *out_len = mlen;
    return HU_OK;
#else /* !HU_IS_TEST */
    hu_local_stt_request_t req;
    hu_error_t err = hu_local_stt_build_request(alloc, config, audio_path, &req);
    if (err != HU_OK)
        return err;

    hu_run_result_t result = {0};
    err = hu_process_run(alloc, req.argv, NULL, 4 * 1024 * 1024, &result);
    hu_local_stt_request_free(alloc, &req);

    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf || result.stdout_len == 0) {
        hu_run_result_free(alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, result.stdout_buf, result.stdout_len, &parsed);
    hu_run_result_free(alloc, &result);
    if (err != HU_OK)
        return HU_ERR_PARSE;

    const char *txt = hu_json_get_string(parsed, "text");
    if (!txt || !txt[0]) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    size_t tlen = strlen(txt);
    char *out = hu_strndup(alloc, txt, tlen);
    hu_json_free(alloc, parsed);
    if (!out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_text = out;
    *out_len = tlen;
    return HU_OK;
#endif
}
