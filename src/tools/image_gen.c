/* image_generate — OpenAI images API (gpt-image-1, HTTPS). Mocked when HU_IS_TEST.
 *
 * DALL·E was shut down 2026-05-12, and every request this file sent since was
 * a 400 ("Unknown parameter: 'response_format'"). gpt-image answers with
 * base64 only, so the image is decoded straight to a temp file: there is no
 * URL to hand out any more. */
#include "human/tools/image_gen.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/multimodal.h"
#include "human/platform.h"
#include "human/tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IG_PROMPT_MAX 4000
#define IG_MODEL      "gpt-image-1"

static const char *ig_name(void *ctx) {
    (void)ctx;
    return "image_generate";
}
static const char *ig_desc(void *ctx) {
    (void)ctx;
    return "Generate an image from a text description (OpenAI gpt-image-1). Returns the path "
           "of the saved PNG.";
}
static const char *ig_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\","
           "\"description\":\"Image description\"},\"size\":{\"type\":\"string\","
           "\"enum\":[\"1024x1024\",\"1536x1024\",\"1024x1536\"],"
           "\"description\":\"Image dimensions (default 1024x1024)\"},\"quality\":{"
           "\"type\":\"string\",\"enum\":[\"low\",\"medium\",\"high\"],"
           "\"description\":\"Image quality (default medium)\"}},"
           "\"required\":[\"prompt\"]}";
}

/* gpt-image values, plus the DALL·E ones older callers still pass. */
static const char *ig_map_size(const char *s) {
    if (!s || strcmp(s, "1024x1024") == 0)
        return "1024x1024";
    if (strcmp(s, "1536x1024") == 0 || strcmp(s, "1792x1024") == 0)
        return "1536x1024";
    if (strcmp(s, "1024x1536") == 0 || strcmp(s, "1024x1792") == 0)
        return "1024x1536";
    return NULL;
}
static const char *ig_map_quality(const char *q) {
    if (!q || strcmp(q, "standard") == 0 || strcmp(q, "medium") == 0)
        return "medium";
    if (strcmp(q, "hd") == 0 || strcmp(q, "high") == 0)
        return "high";
    if (strcmp(q, "low") == 0)
        return "low";
    return NULL;
}

static char *ig_dup_slice(hu_allocator_t *alloc, const char *s, size_t len) {
    char *p = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (!p)
        return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static hu_error_t ig_set_str(hu_allocator_t *alloc, hu_json_value_t *obj, const char *key,
                             const char *v, size_t len) {
    hu_json_value_t *jv = hu_json_string_new(alloc, v, len);
    if (!jv)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t e = hu_json_object_set(alloc, obj, key, jv);
    if (e != HU_OK)
        hu_json_free(alloc, jv);
    return e;
}

hu_error_t hu_image_gen_build_request(hu_allocator_t *alloc, const char *prompt, size_t prompt_len,
                                      const char *size, const char *quality, char **out_body,
                                      size_t *out_len) {
    if (!alloc || !prompt || prompt_len == 0 || !out_body || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_body = NULL;
    *out_len = 0;
    const char *sz = ig_map_size(size);
    const char *ql = ig_map_quality(quality);
    if (!sz || !ql || prompt_len > IG_PROMPT_MAX)
        return HU_ERR_INVALID_ARGUMENT;
    hu_json_value_t *root = hu_json_object_new(alloc);
    if (!root)
        return HU_ERR_OUT_OF_MEMORY;
    hu_error_t e = ig_set_str(alloc, root, "model", IG_MODEL, sizeof(IG_MODEL) - 1);
    if (e == HU_OK)
        e = ig_set_str(alloc, root, "prompt", prompt, prompt_len);
    if (e == HU_OK)
        e = ig_set_str(alloc, root, "size", sz, strlen(sz));
    if (e == HU_OK)
        e = ig_set_str(alloc, root, "quality", ql, strlen(ql));
    if (e == HU_OK) {
        hu_json_value_t *n = hu_json_number_new(alloc, 1.0);
        e = n ? hu_json_object_set(alloc, root, "n", n) : HU_ERR_OUT_OF_MEMORY;
        if (e != HU_OK && n)
            hu_json_free(alloc, n);
    }
    if (e == HU_OK)
        e = hu_json_stringify(alloc, root, out_body, out_len);
    hu_json_free(alloc, root);
    return e;
}

hu_error_t hu_image_gen_parse_image(hu_allocator_t *alloc, const char *body, size_t body_len,
                                    void **out_bytes, size_t *out_len) {
    if (!alloc || !body || !out_bytes || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out_bytes = NULL;
    *out_len = 0;
    hu_json_value_t *json = NULL;
    if (hu_json_parse(alloc, body, body_len, &json) != HU_OK || !json) {
        hu_json_free(alloc, json);
        return HU_ERR_IO;
    }
    const char *b64 = NULL;
    const hu_json_value_t *data = hu_json_object_get(json, "data");
    if (data && data->type == HU_JSON_ARRAY && data->data.array.len > 0 && data->data.array.items &&
        data->data.array.items[0] && data->data.array.items[0]->type == HU_JSON_OBJECT)
        b64 = hu_json_get_string(data->data.array.items[0], "b64_json");
    hu_error_t e = HU_ERR_IO;
    if (b64 && b64[0])
        e = hu_multimodal_decode_base64(alloc, b64, strlen(b64), out_bytes, out_len);
    hu_json_free(alloc, json);
    if (e != HU_OK) {
        *out_bytes = NULL;
        *out_len = 0;
        return HU_ERR_IO;
    }
    return HU_OK;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
/* Write bytes to <tmp>/hu_img_XXXXXX.png; the path goes to out_path. */
static hu_error_t ig_write_temp_png(hu_allocator_t *alloc, const void *bytes, size_t n,
                                    char *out_path, size_t out_path_cap) {
    char *tmpdir = hu_platform_get_temp_dir(alloc);
    if (!tmpdir)
        return HU_ERR_IO;
    char tpl[512];
    int tn = snprintf(tpl, sizeof(tpl), "%s/hu_img_XXXXXX.png", tmpdir);
    alloc->free(alloc->ctx, tmpdir, strlen(tmpdir) + 1);
    if (tn <= 0 || (size_t)tn >= sizeof(tpl) || (size_t)tn >= out_path_cap)
        return HU_ERR_IO;
    int fd = mkstemps(tpl, 4);
    if (fd < 0)
        return HU_ERR_IO;
    ssize_t written = write(fd, bytes, n);
    close(fd);
    if (written < 0 || (size_t)written != n) {
        unlink(tpl);
        return HU_ERR_IO;
    }
    memcpy(out_path, tpl, (size_t)tn + 1);
    return HU_OK;
}

/* Generate one image and save it as a temp PNG. On an API error the message
 * (alloc-owned) goes to *err_msg when non-NULL. */
static hu_error_t ig_generate_to_file(hu_allocator_t *alloc, const char *prompt, size_t prompt_len,
                                      const char *size, const char *quality, char *out_path,
                                      size_t out_path_cap, char **err_msg, size_t *err_len) {
    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key || !api_key[0])
        return HU_ERR_NOT_SUPPORTED;
    char auth[512];
    int alen = snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    if (alen <= 0 || (size_t)alen >= sizeof(auth))
        return HU_ERR_INVALID_ARGUMENT;
    char *body = NULL;
    size_t body_len = 0;
    hu_error_t e =
        hu_image_gen_build_request(alloc, prompt, prompt_len, size, quality, &body, &body_len);
    if (e != HU_OK)
        return e;
    hu_http_response_t resp = {0};
    e = hu_http_post_json(alloc, "https://api.openai.com/v1/images/generations", auth, body,
                          body_len, &resp);
    alloc->free(alloc->ctx, body, body_len + 1);
    if (e != HU_OK || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return e != HU_OK ? e : HU_ERR_IO;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        hu_json_value_t *ej = NULL;
        if (err_msg && hu_json_parse(alloc, resp.body, resp.body_len, &ej) == HU_OK && ej) {
            const hu_json_value_t *er = hu_json_object_get(ej, "error");
            const char *m =
                (er && er->type == HU_JSON_OBJECT) ? hu_json_get_string(er, "message") : NULL;
            if (m && m[0]) {
                *err_len = strlen(m);
                *err_msg = ig_dup_slice(alloc, m, *err_len);
            }
        }
        hu_json_free(alloc, ej);
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }
    void *bytes = NULL;
    size_t n = 0;
    e = hu_image_gen_parse_image(alloc, resp.body, resp.body_len, &bytes, &n);
    hu_http_response_free(alloc, &resp);
    if (e != HU_OK)
        return e;
    e = ig_write_temp_png(alloc, bytes, n, out_path, out_path_cap);
    alloc->free(alloc->ctx, bytes, n);
    return e;
}
#endif

static hu_error_t ig_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                             hu_tool_result_t *out) {
    (void)ctx;
    if (!alloc || !args || !out)
        return HU_ERR_INVALID_ARGUMENT;
#if defined(HU_IS_TEST) && HU_IS_TEST
    const char *prompt = hu_json_get_string(args, "prompt");
    if (!prompt || !prompt[0]) {
        *out = hu_tool_result_fail("prompt is required", 18);
        return HU_OK;
    }
    size_t plen = strlen(prompt);
    if (plen > 50)
        plen = 50;
    char mock[512];
    int n = snprintf(mock, sizeof(mock),
                     "https://oaidalleapiprodscus.blob.core.windows.net/mock/%.*s.png", (int)plen,
                     prompt);
    if (n <= 0 || (size_t)n >= sizeof(mock)) {
        *out = hu_tool_result_fail("mock url overflow", 17);
        return HU_OK;
    }
    char *copy = ig_dup_slice(alloc, mock, (size_t)n);
    if (!copy) {
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_owned(copy, (size_t)n);
    return HU_OK;
#else
    if (args->type != HU_JSON_OBJECT) {
        *out = hu_tool_result_fail("invalid arguments", 17);
        return HU_OK;
    }
    const char *prompt = hu_json_get_string(args, "prompt");
    if (!prompt || !prompt[0]) {
        *out = hu_tool_result_fail("prompt is required", 18);
        return HU_OK;
    }
    size_t plen = strlen(prompt);
    if (plen > IG_PROMPT_MAX) {
        *out = hu_tool_result_fail("prompt too long", 15);
        return HU_OK;
    }
    const char *size = hu_json_get_string(args, "size");
    const char *quality = hu_json_get_string(args, "quality");
    if (!ig_map_size(size)) {
        *out = hu_tool_result_fail("invalid size", 12);
        return HU_OK;
    }
    if (!ig_map_quality(quality)) {
        *out = hu_tool_result_fail("invalid quality", 15);
        return HU_OK;
    }
    char path[512];
    char *emsg = NULL;
    size_t elen = 0;
    hu_error_t e =
        ig_generate_to_file(alloc, prompt, plen, size, quality, path, sizeof(path), &emsg, &elen);
    if (e == HU_OK) {
        size_t pl = strlen(path);
        char *copy = ig_dup_slice(alloc, path, pl);
        if (!copy) {
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_owned(copy, pl);
        return HU_OK;
    }
    if (emsg) {
        *out = hu_tool_result_fail_owned(emsg, elen);
        return HU_OK;
    }
    if (e == HU_ERR_NOT_SUPPORTED)
        *out = hu_tool_result_fail("OPENAI_API_KEY not set", 22);
    else
        *out = hu_tool_result_fail("image generation failed", 23);
    return HU_OK;
#endif
}

static const hu_tool_vtable_t image_gen_vtable = {
    .execute = ig_execute,
    .name = ig_name,
    .description = ig_desc,
    .parameters_json = ig_params,
    .deinit = NULL,
};

hu_error_t hu_image_gen_create(hu_allocator_t *alloc, hu_tool_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->vtable = &image_gen_vtable;
    out->ctx = NULL;
    return HU_OK;
}

hu_error_t hu_image_gen_url_into_buffer(hu_allocator_t *alloc, const char *query, size_t query_len,
                                        char *out_url, size_t out_url_cap) {
    if (!alloc || !query || query_len == 0 || !out_url || out_url_cap < 16)
        return HU_ERR_INVALID_ARGUMENT;
    if (query_len > IG_PROMPT_MAX)
        return HU_ERR_INVALID_ARGUMENT;
#if defined(HU_IS_TEST) && HU_IS_TEST
    int n = snprintf(out_url, out_url_cap,
                     "https://oaidalleapiprodscus.blob.core.windows.net/mock/%.*s.png",
                     (int)(query_len > 50 ? 50 : query_len), query);
    return (n > 0 && (size_t)n < out_url_cap) ? HU_OK : HU_ERR_INVALID_ARGUMENT;
#else
    /* gpt-image returns image bytes, never a URL; callers that need a link
     * keep their own fallback (visual/content.c: an image search link). */
    return HU_ERR_NOT_SUPPORTED;
#endif
}

hu_error_t hu_image_gen_download(hu_allocator_t *alloc, const char *prompt, size_t prompt_len,
                                 char *out_path, size_t out_path_cap) {
    if (!alloc || !prompt || !prompt_len || !out_path || out_path_cap < 32)
        return HU_ERR_INVALID_ARGUMENT;
#if defined(HU_IS_TEST) && HU_IS_TEST
    int n = snprintf(out_path, out_path_cap, "/tmp/hu_test_image_%.*s.png",
                     (int)(prompt_len > 20 ? 20 : prompt_len), prompt);
    return (n > 0 && (size_t)n < out_path_cap) ? HU_OK : HU_ERR_IO;
#else
    if (prompt_len > IG_PROMPT_MAX)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t e = ig_generate_to_file(alloc, prompt, prompt_len, NULL, NULL, out_path,
                                       out_path_cap, NULL, NULL);
    return e == HU_OK ? HU_OK : HU_ERR_IO;
#endif
}
