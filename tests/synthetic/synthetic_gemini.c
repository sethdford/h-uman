#include "human/core/http.h"
#include "synthetic_harness.h"

struct hu_synth_gemini_ctx {
    char *api_key;
    char *model;
    char *url;
    size_t url_len;
};

hu_error_t hu_synth_gemini_init(hu_allocator_t *alloc, const char *api_key, const char *model,
                                hu_synth_gemini_ctx_t **out) {
    if (!alloc || !api_key || !model || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_synth_gemini_ctx_t *ctx = (hu_synth_gemini_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->api_key = hu_synth_strdup(alloc, api_key, strlen(api_key));
    ctx->model = hu_synth_strdup(alloc, model, strlen(model));
    char url[512];
    int n = snprintf(
        url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent?key=%s", model,
        api_key);
    ctx->url = hu_synth_strdup(alloc, url, (size_t)n);
    ctx->url_len = (size_t)n;
    *out = ctx;
    return HU_OK;
}

static size_t json_escape(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    size_t di = 0;
    for (size_t i = 0; i < src_len && di < dst_cap - 1; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (di + 2 > dst_cap - 1)
                break;
            dst[di++] = '\\';
            dst[di++] = c;
        } else if (c == '\n') {
            if (di + 2 > dst_cap - 1)
                break;
            dst[di++] = '\\';
            dst[di++] = 'n';
        } else if (c == '\r') {
            if (di + 2 > dst_cap - 1)
                break;
            dst[di++] = '\\';
            dst[di++] = 'r';
        } else if (c == '\t') {
            if (di + 2 > dst_cap - 1)
                break;
            dst[di++] = '\\';
            dst[di++] = 't';
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return di;
}

hu_error_t hu_synth_gemini_generate(hu_allocator_t *alloc, hu_synth_gemini_ctx_t *ctx,
                                    const char *prompt, size_t prompt_len, char **response_out,
                                    size_t *response_len_out) {
    if (!alloc || !ctx || !prompt || !response_out || !response_len_out)
        return HU_ERR_INVALID_ARGUMENT;

    size_t esc_cap = prompt_len * 2 + 1;
    char *escaped = (char *)alloc->alloc(alloc->ctx, esc_cap);
    if (!escaped)
        return HU_ERR_OUT_OF_MEMORY;
    size_t esc_len = json_escape(prompt, prompt_len, escaped, esc_cap);

    size_t body_cap = esc_len + 256;
    char *body = (char *)alloc->alloc(alloc->ctx, body_cap);
    if (!body) {
        alloc->free(alloc->ctx, escaped, esc_cap);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int blen = snprintf(body, body_cap,
                        "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}],"
                        "\"generationConfig\":{\"responseMimeType\":\"application/json\"}}",
                        escaped);
    alloc->free(alloc->ctx, escaped, esc_cap);

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json(alloc, ctx->url, NULL, body, (size_t)blen, &resp);
    alloc->free(alloc->ctx, body, body_cap);
    if (err != HU_OK) {
        hu_http_response_free(alloc, &resp);
        return err;
    }
    if (resp.status_code != 200) {
        HU_SYNTH_LOG("Gemini API returned status %ld", resp.status_code);
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK)
        return err;

    hu_json_value_t *candidates = hu_json_object_get(root, "candidates");
    if (!candidates || candidates->type != HU_JSON_ARRAY || candidates->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }
    hu_json_value_t *first = candidates->data.array.items[0];
    hu_json_value_t *content = hu_json_object_get(first, "content");
    if (!content) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }
    hu_json_value_t *parts = hu_json_object_get(content, "parts");
    if (!parts || parts->type != HU_JSON_ARRAY || parts->data.array.len == 0) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }
    const char *text = hu_json_get_string(parts->data.array.items[0], "text");
    if (!text) {
        hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    size_t tlen = strlen(text);
    *response_out = hu_synth_strdup(alloc, text, tlen);
    *response_len_out = tlen;
    hu_json_free(alloc, root);
    return *response_out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
}

hu_error_t hu_synth_gemini_evaluate(hu_allocator_t *alloc, hu_synth_gemini_ctx_t *ctx,
                                    const char *test_desc, const char *actual_output,
                                    hu_synth_verdict_t *verdict_out, char **reason_out,
                                    size_t *reason_len_out) {
    if (!alloc || !ctx || !test_desc || !actual_output || !verdict_out)
        return HU_ERR_INVALID_ARGUMENT;

    char prompt[4096];
    snprintf(prompt, sizeof(prompt),
             "You are a test evaluator. Given a test description and the actual output, "
             "determine if the test passed or failed.\n\n"
             "Test: %s\n\nActual output: %s\n\n"
             "Return JSON: {\"pass\": true/false, \"reason\": \"explanation\"}",
             test_desc, actual_output);

    char *resp = NULL;
    size_t rlen = 0;
    hu_error_t err = hu_synth_gemini_generate(alloc, ctx, prompt, strlen(prompt), &resp, &rlen);
    if (err != HU_OK)
        return err;

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp, rlen, &root);
    hu_synth_strfree(alloc, resp, rlen);
    if (err != HU_OK)
        return err;

    bool pass = hu_json_get_bool(root, "pass", false);
    *verdict_out = pass ? HU_SYNTH_PASS : HU_SYNTH_FAIL;
    const char *reason = hu_json_get_string(root, "reason");
    if (reason && reason_out && reason_len_out) {
        size_t rl = strlen(reason);
        *reason_out = hu_synth_strdup(alloc, reason, rl);
        *reason_len_out = rl;
    }
    hu_json_free(alloc, root);
    return HU_OK;
}

void hu_synth_gemini_deinit(hu_allocator_t *alloc, hu_synth_gemini_ctx_t *ctx) {
    if (!ctx)
        return;
    if (ctx->api_key)
        hu_synth_strfree(alloc, ctx->api_key, strlen(ctx->api_key));
    if (ctx->model)
        hu_synth_strfree(alloc, ctx->model, strlen(ctx->model));
    if (ctx->url)
        hu_synth_strfree(alloc, ctx->url, ctx->url_len);
    alloc->free(alloc->ctx, ctx, sizeof(*ctx));
}
