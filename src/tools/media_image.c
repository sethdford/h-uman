/* media_image — Generate images via Gemini native image generation
 * (gemini-3-flash-preview) or Imagen 4 on Vertex AI. Writes the decoded PNG
 * to a temp file and returns the local path in media_path so the daemon can
 * attach it to a channel send. */

#include "human/tools/media_image.h"
#include "human/agent.h"
#include "human/agent/tool_context.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/log.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/core/vertex_auth.h"
#include "human/tool.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MI_PROMPT_MAX 4000

/* ── base64 decode (self-contained, mirrors vault.c) ────────────────────── */

static int mi_b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static hu_error_t mi_b64_decode(const char *in, size_t in_len, unsigned char *out, size_t out_cap,
                                size_t *out_len) {
    while (in_len > 0 && in[in_len - 1] == '=')
        in_len--;
    size_t byte_len = (in_len * 3) / 4;
    if (out_cap < byte_len)
        return HU_ERR_INVALID_ARGUMENT;
    size_t j = 0;
    for (size_t i = 0; i + 4 <= in_len; i += 4) {
        int a = mi_b64_val(in[i]), b = mi_b64_val(in[i + 1]);
        int c = mi_b64_val(in[i + 2]), d = mi_b64_val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (uint32_t)(b << 12) | (uint32_t)(c << 6) | (uint32_t)d;
        out[j++] = (unsigned char)(val >> 16);
        out[j++] = (unsigned char)(val >> 8);
        out[j++] = (unsigned char)val;
    }
    if (in_len % 4 == 2) {
        int a = mi_b64_val(in[in_len - 2]), b = mi_b64_val(in[in_len - 1]);
        if (a < 0 || b < 0) return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (uint32_t)(b << 12);
        out[j++] = (unsigned char)(val >> 16);
    } else if (in_len % 4 == 3) {
        int a = mi_b64_val(in[in_len - 3]), b = mi_b64_val(in[in_len - 2]);
        int c = mi_b64_val(in[in_len - 1]);
        if (a < 0 || b < 0 || c < 0) return HU_ERR_PARSE;
        uint32_t val = (uint32_t)(a << 18) | (uint32_t)(b << 12) | (uint32_t)(c << 6);
        out[j++] = (unsigned char)(val >> 16);
        out[j++] = (unsigned char)(val >> 8);
    }
    *out_len = j;
    return HU_OK;
}

/* ── write decoded bytes to a temp file ─────────────────────────────────── */

static hu_error_t mi_write_temp(const unsigned char *data, size_t data_len, const char *ext,
                                char *path_out, size_t path_cap) {
    int n = snprintf(path_out, path_cap, "/tmp/human_img_%lx.%s",
                     (unsigned long)time(NULL) ^ (unsigned long)data_len, ext);
    if (n <= 0 || (size_t)n >= path_cap)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path_out, "wb");
    if (!f)
        return HU_ERR_IO;
    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);
    return written == data_len ? HU_OK : HU_ERR_IO;
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static const char *mi_name(void *ctx) { (void)ctx; return "media_image"; }

static const char *mi_desc(void *ctx) {
    (void)ctx;
    return "Generate an image from a text prompt using Gemini native image generation "
           "or Imagen 4. Returns a local file path to the generated PNG.";
}

static const char *mi_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{"
           "\"prompt\":{\"type\":\"string\",\"description\":\"Image description\"},"
           "\"model\":{\"type\":\"string\","
           "\"enum\":[\"gemini\",\"imagen4\",\"imagen4_fast\",\"imagen4_ultra\"],"
           "\"description\":\"Model to use (default gemini)\"},"
           "\"aspect_ratio\":{\"type\":\"string\","
           "\"enum\":[\"1:1\",\"16:9\",\"9:16\",\"3:4\",\"4:3\"],"
           "\"description\":\"Aspect ratio (default 1:1)\"}},"
           "\"required\":[\"prompt\"]}";
}

static bool mi_aspect_ok(const char *s) {
    if (!s) return false;
    return strcmp(s, "1:1") == 0 || strcmp(s, "16:9") == 0 || strcmp(s, "9:16") == 0 ||
           strcmp(s, "3:4") == 0 || strcmp(s, "4:3") == 0;
}

static bool mi_model_ok(const char *s) {
    if (!s) return false;
    return strcmp(s, "gemini") == 0 || strcmp(s, "nano_banana") == 0 ||
           strcmp(s, "imagen4") == 0 || strcmp(s, "imagen4_fast") == 0 ||
           strcmp(s, "imagen4_ultra") == 0;
}

static const char *mi_imagen4_model_id(const char *model) {
    if (strcmp(model, "imagen4_fast") == 0) return "imagen-4.0-fast-generate-001";
    if (strcmp(model, "imagen4_ultra") == 0) return "imagen-4.0-ultra-generate-001";
    return "imagen-4.0-generate-001";
}

static hu_error_t mi_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                             hu_tool_result_t *out) {
    (void)ctx;
    if (!alloc || !args || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *prompt = hu_json_get_string(args, "prompt");
    if (!prompt || !prompt[0]) {
        *out = hu_tool_result_fail("prompt is required", 18);
        return HU_OK;
    }
    if (strlen(prompt) > MI_PROMPT_MAX) {
        *out = hu_tool_result_fail("prompt too long", 15);
        return HU_OK;
    }

    const char *model = hu_json_get_string(args, "model");
    if (!model) {
        const char *env_model = getenv("HU_DEFAULT_IMAGE_MODEL");
        if (env_model && mi_model_ok(env_model))
            model = env_model;
        else
            model = "gemini";
    } else if (!mi_model_ok(model)) {
        *out = hu_tool_result_fail("invalid model", 13);
        return HU_OK;
    }

    const char *aspect = hu_json_get_string(args, "aspect_ratio");
    if (aspect && !mi_aspect_ok(aspect)) {
        *out = hu_tool_result_fail("invalid aspect_ratio", 20);
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)mi_b64_decode;
    (void)mi_write_temp;
    (void)mi_imagen4_model_id;
    char mock_path[256];
    size_t plen = strlen(prompt);
    if (plen > 40) plen = 40;
    int n = snprintf(mock_path, sizeof(mock_path), "/tmp/human_img_mock_%.*s.png", (int)plen, prompt);
    if (n <= 0 || (size_t)n >= sizeof(mock_path)) {
        *out = hu_tool_result_fail("mock path overflow", 18);
        return HU_OK;
    }
    char *path_copy = hu_strndup(alloc, mock_path, (size_t)n);
    char *desc = hu_strndup(alloc, mock_path, (size_t)n);
    if (!path_copy || !desc) {
        if (path_copy) alloc->free(alloc->ctx, path_copy, (size_t)n + 1);
        if (desc) alloc->free(alloc->ctx, desc, (size_t)n + 1);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_with_media(desc, (size_t)n, path_copy, (size_t)n);
    return HU_OK;
#else
    bool use_imagen4 = strncmp(model, "imagen4", 7) == 0;

    hu_vertex_auth_t vauth = {0};
    hu_error_t err = hu_vertex_auth_load_adc(&vauth, alloc);

    if (use_imagen4) {
        /* Imagen 4 via Vertex AI :predict — requires ADC */
        if (err != HU_OK) {
            *out = hu_tool_result_fail("Vertex AI credentials not configured", 36);
            return HU_OK;
        }
        err = hu_vertex_auth_ensure_token(&vauth, alloc);
        if (err != HU_OK) {
            hu_vertex_auth_free(&vauth);
            *out = hu_tool_result_fail("failed to obtain Vertex AI token", 32);
            return HU_OK;
        }

        const char *project = NULL;
        const char *region = NULL;
        hu_agent_t *mi_agent = hu_agent_get_current_for_tools();
        if (mi_agent && mi_agent->config) {
            project = mi_agent->config->media_gen.vertex_project;
            region = mi_agent->config->media_gen.vertex_region;
        }
        if (!project) project = getenv("GOOGLE_CLOUD_PROJECT");
        if (!project) project = getenv("VERTEX_PROJECT");
        if (!region) region = getenv("GOOGLE_CLOUD_LOCATION");
        if (!region) region = "us-central1";

        if (!project || !project[0]) {
            hu_vertex_auth_free(&vauth);
            *out = hu_tool_result_fail("GOOGLE_CLOUD_PROJECT not set", 28);
            return HU_OK;
        }

        const char *model_id = mi_imagen4_model_id(model);
        char url[512];
        int ulen = snprintf(url, sizeof(url),
                            "https://%s-aiplatform.googleapis.com/v1/projects/%s/locations/%s/"
                            "publishers/google/models/%s:predict",
                            region, project, region, model_id);
        if (ulen <= 0 || (size_t)ulen >= sizeof(url)) {
            hu_vertex_auth_free(&vauth);
            *out = hu_tool_result_fail("URL too long", 12);
            return HU_OK;
        }

        /* Build request JSON */
        hu_json_value_t *root = hu_json_object_new(alloc);
        hu_json_value_t *instances = hu_json_array_new(alloc);
        hu_json_value_t *inst = hu_json_object_new(alloc);
        hu_json_value_t *pv = hu_json_string_new(alloc, prompt, strlen(prompt));
        if (!root || !instances || !inst || !pv) {
            hu_json_free(alloc, root); hu_json_free(alloc, instances);
            hu_json_free(alloc, inst); hu_json_free(alloc, pv);
            hu_vertex_auth_free(&vauth);
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_object_set(alloc, inst, "prompt", pv);
        hu_json_array_push(alloc, instances, inst);
        hu_json_object_set(alloc, root, "instances", instances);

        hu_json_value_t *params = hu_json_object_new(alloc);
        hu_json_value_t *sc = hu_json_number_new(alloc, 1.0);
        if (params && sc) {
            hu_json_object_set(alloc, params, "sampleCount", sc);
            if (aspect) {
                hu_json_value_t *ar = hu_json_string_new(alloc, aspect, strlen(aspect));
                if (ar) hu_json_object_set(alloc, params, "aspectRatio", ar);
            }
            hu_json_object_set(alloc, root, "parameters", params);
        }

        char *body = NULL;
        size_t body_len = 0;
        err = hu_json_stringify(alloc, root, &body, &body_len);
        hu_json_free(alloc, root);
        if (err != HU_OK || !body) {
            hu_vertex_auth_free(&vauth);
            *out = hu_tool_result_fail("failed to build request", 23);
            return HU_OK;
        }

        char auth_buf[1024];
        hu_vertex_auth_get_bearer(&vauth, auth_buf, sizeof(auth_buf));

        hu_http_response_t resp = {0};
        err = hu_http_post_json(alloc, url, auth_buf, body, body_len, &resp);
        alloc->free(alloc->ctx, body, body_len + 1);
        hu_vertex_auth_free(&vauth);

        if (err != HU_OK) {
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("Imagen 4 API request failed", 27);
            return HU_OK;
        }
        if (resp.status_code < 200 || resp.status_code >= 300) {
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("Imagen 4 API error", 18);
            return HU_OK;
        }

        /* Parse: predictions[0].bytesBase64Encoded */
        hu_json_value_t *json = NULL;
        err = hu_json_parse(alloc, resp.body, resp.body_len, &json);
        hu_http_response_free(alloc, &resp);
        if (err != HU_OK || !json) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("failed to parse Imagen 4 response", 33);
            return HU_OK;
        }

        const hu_json_value_t *preds = hu_json_object_get(json, "predictions");
        const char *b64_data = NULL;
        if (preds && preds->type == HU_JSON_ARRAY && preds->data.array.len > 0) {
            const hu_json_value_t *first = preds->data.array.items[0];
            if (first && first->type == HU_JSON_OBJECT)
                b64_data = hu_json_get_string(first, "bytesBase64Encoded");
        }

        if (!b64_data || !b64_data[0]) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("no image data in Imagen 4 response", 34);
            return HU_OK;
        }

        size_t b64_len = strlen(b64_data);
        if (b64_len > SIZE_MAX / 3) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("base64 payload too large", 24);
            return HU_OK;
        }
        size_t raw_cap = (b64_len * 3) / 4 + 4;
        unsigned char *raw = (unsigned char *)alloc->alloc(alloc->ctx, raw_cap);
        if (!raw) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t raw_len = 0;
        err = mi_b64_decode(b64_data, b64_len, raw, raw_cap, &raw_len);
        hu_json_free(alloc, json);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, raw, raw_cap);
            *out = hu_tool_result_fail("base64 decode failed", 20);
            return HU_OK;
        }

        char path_buf[256];
        err = mi_write_temp(raw, raw_len, "png", path_buf, sizeof(path_buf));
        alloc->free(alloc->ctx, raw, raw_cap);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("failed to write image file", 26);
            return HU_OK;
        }

        size_t pl = strlen(path_buf);
        char *path_copy = hu_strndup(alloc, path_buf, pl);
        char *desc = hu_sprintf(alloc, "Generated image saved to %s", path_buf);
        if (!path_copy || !desc) {
            if (path_copy) alloc->free(alloc->ctx, path_copy, pl + 1);
            if (desc) alloc->free(alloc->ctx, desc, strlen(desc) + 1);
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_with_media(desc, strlen(desc), path_copy, pl);
        return HU_OK;

    } else {
        /* Gemini native image generation via generateContent API. */
        const char *api_key = getenv("GEMINI_API_KEY");
        if (!api_key) api_key = getenv("GOOGLE_API_KEY");

        char auth_buf[1024];
        char url[512];
        const char *nb_model = "gemini-3.5-flash"; /* 3-flash-preview superseded; see CLAUDE.md model lineup */

        if (api_key && api_key[0]) {
            /* API keys in query strings leak via Referer, logs, and proxies; Vertex + ADC is preferred. */
            hu_log_warn("media_image", NULL,
                        "Gemini image generation using API key path; prefer Vertex AI + ADC");
            int ulen = snprintf(url, sizeof(url),
                                "https://generativelanguage.googleapis.com/v1beta/models/%s"
                                ":generateContent?key=%s",
                                nb_model, api_key);
            if (ulen <= 0 || (size_t)ulen >= sizeof(url)) {
                *out = hu_tool_result_fail("URL too long", 12);
                return HU_OK;
            }
            auth_buf[0] = '\0';
        } else if (err == HU_OK) {
            /* ADC path */
            err = hu_vertex_auth_ensure_token(&vauth, alloc);
            if (err != HU_OK) {
                hu_vertex_auth_free(&vauth);
                *out = hu_tool_result_fail("no API key or Vertex AI credentials", 35);
                return HU_OK;
            }
            hu_vertex_auth_get_bearer(&vauth, auth_buf, sizeof(auth_buf));

            const char *project = NULL;
            const char *region = NULL;
            hu_agent_t *nb_agent = hu_agent_get_current_for_tools();
            if (nb_agent && nb_agent->config) {
                project = nb_agent->config->media_gen.vertex_project;
                region = nb_agent->config->media_gen.vertex_region;
            }
            if (!project) project = getenv("GOOGLE_CLOUD_PROJECT");
            if (!project) project = getenv("VERTEX_PROJECT");
            if (!region) region = getenv("GOOGLE_CLOUD_LOCATION");
            if (!region) region = "us-central1";
            if (!project || !project[0]) {
                hu_vertex_auth_free(&vauth);
                *out = hu_tool_result_fail("GOOGLE_CLOUD_PROJECT not set", 28);
                return HU_OK;
            }
            int ulen = snprintf(url, sizeof(url),
                                "https://%s-aiplatform.googleapis.com/v1/projects/%s/locations/%s/"
                                "publishers/google/models/%s:generateContent",
                                region, project, region, nb_model);
            if (ulen <= 0 || (size_t)ulen >= sizeof(url)) {
                hu_vertex_auth_free(&vauth);
                *out = hu_tool_result_fail("URL too long", 12);
                return HU_OK;
            }
        } else {
            *out = hu_tool_result_fail("GEMINI_API_KEY not set and no Vertex AI credentials", 51);
            return HU_OK;
        }

        /* Build generateContent request with responseModalities=["TEXT","IMAGE"] */
        hu_json_value_t *root = hu_json_object_new(alloc);
        hu_json_value_t *contents = hu_json_array_new(alloc);
        hu_json_value_t *msg = hu_json_object_new(alloc);
        hu_json_value_t *parts = hu_json_array_new(alloc);
        hu_json_value_t *part = hu_json_object_new(alloc);
        hu_json_value_t *text_val = hu_json_string_new(alloc, prompt, strlen(prompt));
        if (!root || !contents || !msg || !parts || !part || !text_val) {
            hu_json_free(alloc, root); hu_json_free(alloc, contents);
            hu_json_free(alloc, msg); hu_json_free(alloc, parts);
            hu_json_free(alloc, part); hu_json_free(alloc, text_val);
            hu_vertex_auth_free(&vauth);
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        hu_json_object_set(alloc, part, "text", text_val);
        hu_json_array_push(alloc, parts, part);
        hu_json_object_set(alloc, msg, "parts", parts);
        hu_json_array_push(alloc, contents, msg);
        hu_json_object_set(alloc, root, "contents", contents);

        /* generationConfig: responseModalities + imageConfig */
        hu_json_value_t *gen_cfg = hu_json_object_new(alloc);
        hu_json_value_t *modalities = hu_json_array_new(alloc);
        if (gen_cfg && modalities) {
            hu_json_value_t *text_mod = hu_json_string_new(alloc, "TEXT", 4);
            hu_json_value_t *img_mod = hu_json_string_new(alloc, "IMAGE", 5);
            if (text_mod) hu_json_array_push(alloc, modalities, text_mod);
            if (img_mod) hu_json_array_push(alloc, modalities, img_mod);
            hu_json_object_set(alloc, gen_cfg, "responseModalities", modalities);
            if (aspect) {
                hu_json_value_t *img_cfg = hu_json_object_new(alloc);
                hu_json_value_t *ar = hu_json_string_new(alloc, aspect, strlen(aspect));
                if (img_cfg && ar) {
                    hu_json_object_set(alloc, img_cfg, "aspectRatio", ar);
                    hu_json_object_set(alloc, gen_cfg, "imageConfig", img_cfg);
                }
            }
            hu_json_object_set(alloc, root, "generationConfig", gen_cfg);
        }

        char *body = NULL;
        size_t body_len = 0;
        err = hu_json_stringify(alloc, root, &body, &body_len);
        hu_json_free(alloc, root);
        if (err != HU_OK || !body) {
            hu_vertex_auth_free(&vauth);
            *out = hu_tool_result_fail("failed to build request", 23);
            return HU_OK;
        }

        hu_http_response_t resp = {0};
        const char *auth_ptr = auth_buf[0] ? auth_buf : NULL;
        err = hu_http_post_json(alloc, url, auth_ptr, body, body_len, &resp);
        alloc->free(alloc->ctx, body, body_len + 1);
        hu_vertex_auth_free(&vauth);

        if (err != HU_OK) {
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("Gemini image API request failed", 31);
            return HU_OK;
        }
        if (resp.status_code < 200 || resp.status_code >= 300) {
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_fail("Gemini image API error", 22);
            return HU_OK;
        }

        /* Parse: candidates[0].content.parts[*] — find inlineData */
        hu_json_value_t *json = NULL;
        err = hu_json_parse(alloc, resp.body, resp.body_len, &json);
        hu_http_response_free(alloc, &resp);
        if (err != HU_OK || !json) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("failed to parse response", 24);
            return HU_OK;
        }

        const char *b64_data = NULL;
        const hu_json_value_t *candidates = hu_json_object_get(json, "candidates");
        if (candidates && candidates->type == HU_JSON_ARRAY && candidates->data.array.len > 0) {
            const hu_json_value_t *c0 = candidates->data.array.items[0];
            const hu_json_value_t *content = c0 ? hu_json_object_get(c0, "content") : NULL;
            const hu_json_value_t *cparts = content ? hu_json_object_get(content, "parts") : NULL;
            if (cparts && cparts->type == HU_JSON_ARRAY) {
                for (size_t i = 0; i < cparts->data.array.len; i++) {
                    const hu_json_value_t *p = cparts->data.array.items[i];
                    const hu_json_value_t *inline_data = p ? hu_json_object_get(p, "inlineData") : NULL;
                    if (inline_data && inline_data->type == HU_JSON_OBJECT) {
                        b64_data = hu_json_get_string(inline_data, "data");
                        if (b64_data && b64_data[0]) break;
                    }
                }
            }
        }

        if (!b64_data || !b64_data[0]) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("no image data in response", 25);
            return HU_OK;
        }

        size_t b64_len = strlen(b64_data);
        if (b64_len > SIZE_MAX / 3) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("base64 payload too large", 24);
            return HU_OK;
        }
        size_t raw_cap = (b64_len * 3) / 4 + 4;
        unsigned char *raw = (unsigned char *)alloc->alloc(alloc->ctx, raw_cap);
        if (!raw) {
            hu_json_free(alloc, json);
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        size_t raw_len = 0;
        err = mi_b64_decode(b64_data, b64_len, raw, raw_cap, &raw_len);
        hu_json_free(alloc, json);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, raw, raw_cap);
            *out = hu_tool_result_fail("base64 decode failed", 20);
            return HU_OK;
        }

        char path_buf[256];
        err = mi_write_temp(raw, raw_len, "png", path_buf, sizeof(path_buf));
        alloc->free(alloc->ctx, raw, raw_cap);
        if (err != HU_OK) {
            *out = hu_tool_result_fail("failed to write image file", 26);
            return HU_OK;
        }

        size_t pl = strlen(path_buf);
        char *path_copy = hu_strndup(alloc, path_buf, pl);
        char *desc_str = hu_sprintf(alloc, "Generated image saved to %s", path_buf);
        if (!path_copy || !desc_str) {
            if (path_copy) alloc->free(alloc->ctx, path_copy, pl + 1);
            if (desc_str) alloc->free(alloc->ctx, desc_str, strlen(desc_str) + 1);
            *out = hu_tool_result_fail("out of memory", 13);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = hu_tool_result_ok_with_media(desc_str, strlen(desc_str), path_copy, pl);
        return HU_OK;
    }
#endif
}

static const hu_tool_vtable_t media_image_vtable = {
    .execute = mi_execute,
    .name = mi_name,
    .description = mi_desc,
    .parameters_json = mi_params,
    .deinit = NULL,
};

hu_error_t hu_media_image_create(hu_allocator_t *alloc, hu_tool_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->vtable = &media_image_vtable;
    out->ctx = NULL;
    return HU_OK;
}
