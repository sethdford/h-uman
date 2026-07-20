/*
 * Transcribe meeting audio via Google Cloud Speech-to-Text v2 (Chirp) or v1, then store in BFF.
 */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/file.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/multimodal.h"
#include "human/security.h"
#include "human/tool.h"
#include "human/tools/validation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_MEETING_NAME "meeting_transcribe"
#define HU_MEETING_DESC                                                                       \
    "Transcribe audio (sync, max ~8MB). Uses Speech-to-Text v2 with model chirp_2 when "      \
    "gcp_project is set (arg or GOOGLE_CLOUD_PROJECT/GCP_PROJECT env) and speech_api is not " \
    "v1; otherwise v1. v2 URL: .../locations/{speech_location}/recognizers/_:recognize. "     \
    "OAuth access_token. Env: BFF_*, optional SPEECH_LOCATION."
#define HU_MEETING_PARAMS                                                                         \
    "{\"type\":\"object\",\"properties\":{\"audio_path\":{\"type\":\"string\"},\"access_token\":" \
    "{\"type\":\"string\",\"description\":\"GCP OAuth2 access token\"},\"client_id\":{\"type\":"  \
    "\"string\"},\"gcp_project\":{\"type\":\"string\",\"description\":\"GCP project id for "      \
    "Speech v2\"},\"speech_location\":{\"type\":\"string\",\"default\":\"us\"},\"speech_api\":"   \
    "{\"type\":\"string\",\"description\":\"v2 (default when project set) or v1\"},"              \
    "\"encoding\":{\"type\":\"string\",\"description\":\"LINEAR16, FLAC, MP3, OGG_OPUS, "         \
    "WEBM_OPUS (default LINEAR16)\"},\"sample_rate_hertz\":{\"type\":\"number\","                 \
    "\"default\":16000},\"language_code\":{\"type\":\"string\",\"default\":\"en-US\"},"           \
    "\"model\":{\"type\":\"string\",\"description\":\"v2 default chirp_2; v1 default "            \
    "latest_long\"},\"session_id\":{\"type\":\"string\"}},\"required\":[\"audio_path\","          \
    "\"access_token\",\"client_id\"]}"

#define SPEECH_V1_URL "https://speech.googleapis.com/v1/speech:recognize"
#define AUDIO_CAP     (8 * 1024 * 1024)

typedef struct {
    const char *workspace_dir;
    size_t workspace_dir_len;
    hu_security_policy_t *policy;
} meeting_ctx_t;

#if !HU_IS_TEST
/* v1 and v2 JSON both include "transcript":"..." under alternatives. */
static char *meeting_extract_transcript(hu_allocator_t *alloc, const char *body, size_t bl) {
    const char *needle = "\"transcript\"";
    const char *found = NULL;
    for (size_t i = 0; i + 12 < bl; i++) {
        if (memcmp(body + i, needle, 12) == 0) {
            found = body + i;
            break;
        }
    }
    char *transcript = hu_strndup(alloc, "(no transcript in response)", 27);
    if (found) {
        const char *q = strchr(found, ':');
        if (q) {
            q++;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '"') {
                q++;
                const char *end = strchr(q, '"');
                if (end && end > q) {
                    alloc->free(alloc->ctx, transcript, 28);
                    transcript = hu_strndup(alloc, q, (size_t)(end - q));
                }
            }
        }
    }
    return transcript;
}

static void strip_slash(char *s) {
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/')
        s[--n] = '\0';
}

static const char *bff_bearer(void) {
    static char a[4096];
    const char *t = getenv("BFF_AUTH_TOKEN");
    if (!t || !t[0])
        return NULL;
    snprintf(a, sizeof(a), "Bearer %s", t);
    return a;
}

static int bff_store(hu_allocator_t *alloc, const char *base, const char *bff_auth,
                     const char *tenant, const char *json, size_t json_len) {
    char url[768];
    snprintf(url, sizeof(url), "%s/v1/memory/store", base);
    char xt[256];
    const char *ex = NULL;
    if (tenant && tenant[0]) {
        snprintf(xt, sizeof(xt), "X-Tenant-ID: %s\n", tenant);
        ex = xt;
    }
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_post_json_ex(alloc, url, bff_auth, ex, json, json_len, &resp);
    if (err != HU_OK) {
        if (resp.owned && resp.body)
            hu_http_response_free(alloc, &resp);
        return -1;
    }
    long sc = resp.status_code;
    if (resp.owned && resp.body)
        hu_http_response_free(alloc, &resp);
    return (sc >= 200 && sc < 300) ? 0 : -1;
}
#endif

static hu_error_t meeting_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                  hu_tool_result_t *out) {
    meeting_ctx_t *c = (meeting_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
#if HU_IS_TEST
    (void)alloc;
    *out = hu_tool_result_ok("{\"transcript\":\"(test)\",\"stored\":true}", 38);
    return HU_OK;
#else
    const char *path = hu_json_get_string(args, "audio_path");
    const char *tok = hu_json_get_string(args, "access_token");
    const char *client = hu_json_get_string(args, "client_id");
    if (!path || !tok || !client) {
        *out = hu_tool_result_fail("audio_path, access_token, client_id required", 40);
        return HU_OK;
    }
    const char *enc = hu_json_get_string(args, "encoding");
    if (!enc || !enc[0])
        enc = "LINEAR16";
    int sr = (int)hu_json_get_number(args, "sample_rate_hertz", 16000);
    const char *lang = hu_json_get_string(args, "language_code");
    if (!lang || !lang[0])
        lang = "en-US";
    const char *gcp_proj = hu_json_get_string(args, "gcp_project");
    if (!gcp_proj || !gcp_proj[0])
        gcp_proj = getenv("GOOGLE_CLOUD_PROJECT");
    if (!gcp_proj || !gcp_proj[0])
        gcp_proj = getenv("GCP_PROJECT");
    const char *speech_api = hu_json_get_string(args, "speech_api");
    bool force_v1 = speech_api && strcmp(speech_api, "v1") == 0;
    bool use_v2 = !force_v1 && gcp_proj && gcp_proj[0];
    const char *sloc = hu_json_get_string(args, "speech_location");
    if (!sloc || !sloc[0])
        sloc = getenv("SPEECH_LOCATION");
    if (!sloc || !sloc[0])
        sloc = "us";

    const char *model = hu_json_get_string(args, "model");
    if (!model || !model[0])
        model = use_v2 ? "chirp_2" : "latest_long";
    const char *sid = hu_json_get_string(args, "session_id");

    hu_error_t perr =
        hu_tool_validate_path(path, c->workspace_dir, c->workspace_dir ? c->workspace_dir_len : 0);
    if (perr != HU_OK) {
        *out = hu_tool_result_fail("path traversal or invalid path", 30);
        return HU_OK;
    }
    char resolved[4096];
    const char *open_path = path;
    bool is_abs = (path[0] == '/') ||
                  (strlen(path) >= 2 && path[1] == ':' &&
                   ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')));
    if (c->workspace_dir && c->workspace_dir_len > 0 && !is_abs) {
        size_t n = c->workspace_dir_len;
        if (n >= sizeof(resolved) - 1) {
            *out = hu_tool_result_fail("path too long", 13);
            return HU_OK;
        }
        memcpy(resolved, c->workspace_dir, n);
        if (n > 0 && resolved[n - 1] != '/') {
            resolved[n] = '/';
            n++;
        }
        if (n + strlen(path) >= sizeof(resolved)) {
            *out = hu_tool_result_fail("path too long", 13);
            return HU_OK;
        }
        memcpy(resolved + n, path, strlen(path) + 1);
        open_path = resolved;
    }
    if (!c->policy || !hu_security_path_allowed(c->policy, open_path, strlen(open_path))) {
        *out = hu_tool_result_fail("path not allowed by policy", 26);
        return HU_OK;
    }

    char *raw = NULL;
    size_t rd = 0;
    hu_error_t rerr = hu_file_slurp(alloc, open_path, (size_t)AUDIO_CAP, &raw, &rd);
    if (rerr == HU_ERR_NOT_FOUND) {
        *out = hu_tool_result_fail("file not found", 14);
        return HU_OK;
    }
    if (rerr == HU_ERR_LIMIT_REACHED) {
        *out = hu_tool_result_fail("audio too large (max 8MB for sync API)", 38);
        return HU_OK;
    }
    if (rerr == HU_ERR_OUT_OF_MEMORY) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (rerr != HU_OK) {
        *out = hu_tool_result_fail("seek failed", 11);
        return HU_OK;
    }

    char *b64 = NULL;
    size_t b64_len = 0;
    if (hu_multimodal_encode_base64(alloc, raw, rd, &b64, &b64_len) != HU_OK) {
        alloc->free(alloc->ctx, raw, rd + 1);
        *out = hu_tool_result_fail("base64 encode failed", 20);
        return HU_OK;
    }
    alloc->free(alloc->ctx, raw, rd + 1);

    char gauth[256];
    snprintf(gauth, sizeof(gauth), "Bearer %s", tok);

    char speech_url[384];
    if (use_v2) {
        snprintf(
            speech_url, sizeof(speech_url),
            "https://speech.googleapis.com/v2/projects/%s/locations/%s/recognizers/_:recognize",
            gcp_proj, sloc);
    } else {
        strncpy(speech_url, SPEECH_V1_URL, sizeof(speech_url) - 1);
        speech_url[sizeof(speech_url) - 1] = '\0';
    }

    size_t req_cap = b64_len + 1536;
    char *req = (char *)alloc->alloc(alloc->ctx, req_cap);
    if (!req) {
        alloc->free(alloc->ctx, b64, b64_len + 1);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    int rn;
    if (use_v2) {
        rn = snprintf(req, req_cap,
                      "{\"config\":{\"explicitDecodingConfig\":{\"encoding\":\"%s\","
                      "\"sampleRateHertz\":%d,\"audioChannelCount\":1},\"languageCodes\":[\"%s\"],"
                      "\"model\":\"%s\"},\"content\":\"",
                      enc, sr, lang, model);
    } else {
        rn = snprintf(req, req_cap,
                      "{\"config\":{\"encoding\":\"%s\",\"sampleRateHertz\":%d,\"languageCode\":"
                      "\"%s\",\"model\":\"%s\"},\"audio\":{\"content\":\"",
                      enc, sr, lang, model);
    }
    if (rn < 0 || (size_t)rn >= req_cap) {
        alloc->free(alloc->ctx, req, req_cap);
        alloc->free(alloc->ctx, b64, b64_len + 1);
        *out = hu_tool_result_fail("request too large", 15);
        return HU_OK;
    }
    if ((size_t)rn + b64_len + 16 >= req_cap) {
        alloc->free(alloc->ctx, req, req_cap);
        alloc->free(alloc->ctx, b64, b64_len + 1);
        *out = hu_tool_result_fail("request too large", 15);
        return HU_OK;
    }
    memcpy(req + rn, b64, b64_len);
    rn += (int)b64_len;
    {
        size_t req_pos = (size_t)rn;
        if (use_v2)
            req_pos = hu_buf_appendf(req, req_cap, req_pos, "\"}");
        else
            req_pos = hu_buf_appendf(req, req_cap, req_pos, "\"}}");
        rn = (int)req_pos;
    }
    alloc->free(alloc->ctx, b64, b64_len + 1);

    hu_http_response_t resp = {0};
    hu_error_t herr = hu_http_post_json_ex(alloc, speech_url, gauth, NULL, req, (size_t)rn, &resp);
    alloc->free(alloc->ctx, req, req_cap);
    if (herr != HU_OK || resp.status_code < 200 || resp.status_code >= 300) {
        if (resp.owned && resp.body) {
            char *er = hu_strndup(alloc, resp.body, resp.body_len < 512 ? resp.body_len : 512);
            hu_http_response_free(alloc, &resp);
            *out = hu_tool_result_ok_owned(er, er ? strlen(er) : 0);
            return HU_OK;
        }
        *out = hu_tool_result_fail("speech api failed", 17);
        return HU_OK;
    }

    char *transcript = meeting_extract_transcript(alloc, resp.body, resp.body_len);
    hu_http_response_free(alloc, &resp);
    if (!transcript) {
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }

    const char *eb = getenv("BFF_BASE_URL");
    const char *bff_auth = bff_bearer();
    if (!eb || !bff_auth) {
        size_t tlen_early = strlen(transcript);
        char *wrap = hu_sprintf(
            alloc, "{\"transcript_chars\":%zu,\"bff_store\":false,\"note\":\"set BFF_*\"}",
            tlen_early);
        alloc->free(alloc->ctx, transcript, tlen_early + 1);
        *out = hu_tool_result_ok_owned(wrap, wrap ? strlen(wrap) : 0);
        return HU_OK;
    }
    char base[512];
    strncpy(base, eb, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    strip_slash(base);

    char keyb[256];
    snprintf(keyb, sizeof(keyb), "%s:meeting:transcript", client);
    size_t tlen = strlen(transcript);
    size_t jb_cap = tlen * 2 + 256;
    char *jb = (char *)alloc->alloc(alloc->ctx, jb_cap);
    if (!jb) {
        alloc->free(alloc->ctx, transcript, tlen + 1);
        *out = hu_tool_result_fail("out of memory", 12);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t j = 0;
    j = hu_buf_appendf(jb, jb_cap, j, "{\"key\":\"%s\",\"content\":\"", keyb);
    for (size_t k = 0; k < tlen; k++) {
        unsigned char ch = (unsigned char)transcript[k];
        if (ch == '"' || ch == '\\') {
            if (j + 2 < jb_cap)
                j = hu_buf_appendf(jb, jb_cap, j, "\\%c", (char)ch);
        } else if (ch < 32) {
            if (j + 1 < jb_cap)
                jb[j++] = ' ';
        } else {
            if (j + 1 < jb_cap)
                jb[j++] = (char)ch;
        }
    }
    j = hu_buf_appendf(jb, jb_cap, j, "\"");
    if (sid && sid[0])
        j = hu_buf_appendf(jb, jb_cap, j, ",\"session_id\":\"%s\"", sid);
    j = hu_buf_appendf(jb, jb_cap, j, "}");
    alloc->free(alloc->ctx, transcript, tlen + 1);

    const char *tenant = getenv("BFF_TENANT_ID");
    int ok = bff_store(alloc, base, bff_auth, tenant, jb, j);
    alloc->free(alloc->ctx, jb, jb_cap);
    char *fin =
        hu_sprintf(alloc, "{\"transcript_chars\":%zu,\"transcript_stored\":%s,\"bff_ok\":%s}", tlen,
                   ok == 0 ? "true" : "false", ok == 0 ? "true" : "false");
    *out = hu_tool_result_ok_owned(fin, fin ? strlen(fin) : 0);
    return HU_OK;
#endif
}

static const char *meeting_name(void *ctx) {
    (void)ctx;
    return HU_MEETING_NAME;
}
static const char *meeting_description(void *ctx) {
    (void)ctx;
    return HU_MEETING_DESC;
}
static const char *meeting_parameters_json(void *ctx) {
    (void)ctx;
    return HU_MEETING_PARAMS;
}
static void meeting_deinit(void *ctx, hu_allocator_t *alloc) {
    meeting_ctx_t *c = (meeting_ctx_t *)ctx;
    if (!c || !alloc)
        return;
    if (c->workspace_dir)
        alloc->free(alloc->ctx, (void *)c->workspace_dir, c->workspace_dir_len + 1);
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const hu_tool_vtable_t meeting_vtable = {
    .execute = meeting_execute,
    .name = meeting_name,
    .description = meeting_description,
    .parameters_json = meeting_parameters_json,
    .deinit = meeting_deinit,
};

hu_error_t hu_meeting_transcribe_create(hu_allocator_t *alloc, const char *workspace_dir,
                                        size_t workspace_dir_len, hu_security_policy_t *policy,
                                        hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    meeting_ctx_t *c = (meeting_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    if (workspace_dir && workspace_dir_len > 0) {
        c->workspace_dir = hu_strndup(alloc, workspace_dir, workspace_dir_len);
        if (!c->workspace_dir) {
            alloc->free(alloc->ctx, c, sizeof(*c));
            return HU_ERR_OUT_OF_MEMORY;
        }
        c->workspace_dir_len = workspace_dir_len;
    }
    c->policy = policy;
    out->ctx = c;
    out->vtable = &meeting_vtable;
    return HU_OK;
}
