/* media_vertex_common — Vertex AI plumbing shared by the media_gif,
 * media_video and media_image tools. See the header for the contract. */

#include "human/tools/media_vertex_common.h"
#include "human/agent.h"
#include "human/agent/tool_context.h"
#include "human/config.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define mvc_sleep_secs(s) Sleep((s) * 1000)
#else
#include <unistd.h>
#define mvc_sleep_secs(s) sleep((unsigned)(s))
#endif

/* "Bearer " + an OAuth2 access token; Google's are a few hundred bytes. */
#define MVC_BEARER_CAP            1024
#define MVC_MOCK_PROMPT_MAX       30
#define MVC_VEO_POLL_INTERVAL_SEC 15
#define MVC_VEO_POLL_MAX_ATTEMPTS 12 /* 12 * 15s = 3 min max */

/* ── credentials + target ───────────────────────────────────────────────── */

bool hu_media_vertex_resolve_target(const char **project, const char **region,
                                    hu_tool_result_t *out) {
    if (!project || !region || !out)
        return false;
    const char *p = NULL;
    const char *r = NULL;
    hu_agent_t *agent = hu_agent_get_current_for_tools();
    if (agent && agent->config) {
        p = agent->config->media_gen.vertex_project;
        r = agent->config->media_gen.vertex_region;
    }
    if (!p)
        p = getenv("GOOGLE_CLOUD_PROJECT");
    if (!p)
        p = getenv("VERTEX_PROJECT");
    if (!r)
        r = getenv("GOOGLE_CLOUD_LOCATION");
    if (!r)
        r = "us-central1";
    if (!p || !p[0]) {
        *out = hu_tool_result_fail("GOOGLE_CLOUD_PROJECT not set", 28);
        return false;
    }
    *project = p;
    *region = r;
    return true;
}

bool hu_media_vertex_open(hu_vertex_auth_t *vauth, hu_allocator_t *alloc, const char **project,
                          const char **region, hu_tool_result_t *out) {
    if (!vauth || !alloc || !project || !region || !out)
        return false;
    memset(vauth, 0, sizeof(*vauth));
    if (hu_vertex_auth_load_adc(vauth, alloc) != HU_OK) {
        *out = hu_tool_result_fail("Vertex AI credentials not configured", 36);
        return false;
    }
    if (hu_vertex_auth_ensure_token(vauth, alloc) != HU_OK) {
        hu_vertex_auth_free(vauth);
        *out = hu_tool_result_fail("failed to obtain Vertex AI token", 32);
        return false;
    }
    if (!hu_media_vertex_resolve_target(project, region, out)) {
        hu_vertex_auth_free(vauth);
        return false;
    }
    return true;
}

/* ── bearer-gated HTTP ──────────────────────────────────────────────────── */

hu_error_t hu_media_vertex_post_json(const hu_vertex_auth_t *vauth, hu_allocator_t *alloc,
                                     const char *url, const char *body, size_t body_len,
                                     hu_http_response_t *resp) {
    char bearer[MVC_BEARER_CAP];
    hu_error_t err = hu_vertex_auth_get_bearer(vauth, bearer, sizeof(bearer));
    if (err != HU_OK)
        return err;
    return hu_http_post_json(alloc, url, bearer, body, body_len, resp);
}

hu_error_t hu_media_vertex_get(const hu_vertex_auth_t *vauth, hu_allocator_t *alloc,
                               const char *url, hu_http_response_t *resp) {
    char bearer[MVC_BEARER_CAP];
    hu_error_t err = hu_vertex_auth_get_bearer(vauth, bearer, sizeof(bearer));
    if (err != HU_OK)
        return err;
    return hu_http_get(alloc, url, bearer, resp);
}

/* ── results ────────────────────────────────────────────────────────────── */

hu_error_t hu_media_vertex_result_from_path(hu_allocator_t *alloc, const char *fmt,
                                            const char *path, hu_tool_result_t *out) {
    if (!alloc || !fmt || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;
    size_t pl = strlen(path);
    char *path_copy = hu_strndup(alloc, path, pl);
    char *desc = hu_sprintf(alloc, fmt, path);
    if (!path_copy || !desc) {
        if (path_copy)
            alloc->free(alloc->ctx, path_copy, pl + 1);
        if (desc)
            alloc->free(alloc->ctx, desc, strlen(desc) + 1);
        *out = hu_tool_result_fail("out of memory", 13);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = hu_tool_result_ok_with_media(desc, strlen(desc), path_copy, pl);
    return HU_OK;
}

/* ── Veo pipeline ───────────────────────────────────────────────────────── */

const char *hu_media_vertex_veo_storage_uri(const char *project, char *buf, size_t buf_cap) {
    const char *uri = NULL;
    hu_agent_t *agent = hu_agent_get_current_for_tools();
    if (agent && agent->config)
        uri = agent->config->media_gen.veo_storage_uri;
    if (!uri || !uri[0])
        uri = getenv("HU_VEO_STORAGE_URI");
    if ((!uri || !uri[0]) && project && buf && buf_cap > 0) {
        int n = snprintf(buf, buf_cap, "gs://%s-human-media/veo/", project);
        uri = (n > 0 && (size_t)n < buf_cap) ? buf : NULL;
    }
    return (uri && uri[0]) ? uri : NULL;
}

static hu_error_t veo_fail(hu_tool_result_t *out, const char *msg, hu_error_t code) {
    *out = hu_tool_result_fail(msg, strlen(msg));
    return code;
}

static hu_error_t veo_model_url(char *url, size_t cap, const char *project, const char *region,
                                const char *model_id, const char *verb) {
    int n = snprintf(url, cap,
                     "https://%s-aiplatform.googleapis.com/v1/projects/%s/locations/%s/"
                     "publishers/google/models/%s:%s",
                     region, project, region, model_id, verb);
    return (n > 0 && (size_t)n < cap) ? HU_OK : HU_ERR_INVALID_ARGUMENT;
}

/* { instances: [{prompt}], parameters: {storageUri, aspectRatio, sampleCount,
 * durationSeconds} }. storageUri is required: Veo writes the output video to
 * that bucket, which must already exist. */
static hu_error_t veo_submit_body(hu_allocator_t *alloc, const char *project,
                                  const hu_media_veo_request_t *req, char **body, size_t *body_len,
                                  hu_tool_result_t *out) {
    char storage_buf[256] = {0};
    const char *storage_uri =
        hu_media_vertex_veo_storage_uri(project, storage_buf, sizeof(storage_buf));
    if (!storage_uri)
        return veo_fail(out, "veo_storage_uri not configured (set in config or HU_VEO_STORAGE_URI)",
                        HU_ERR_INVALID_ARGUMENT);

    hu_json_value_t *root = hu_json_object_new(alloc);
    hu_json_value_t *instances = hu_json_array_new(alloc);
    hu_json_value_t *inst = hu_json_object_new(alloc);
    hu_json_value_t *pv = hu_json_string_new(alloc, req->prompt, strlen(req->prompt));
    if (!root || !instances || !inst || !pv) {
        hu_json_free(alloc, root);
        hu_json_free(alloc, instances);
        hu_json_free(alloc, inst);
        hu_json_free(alloc, pv);
        return veo_fail(out, "out of memory", HU_ERR_OUT_OF_MEMORY);
    }
    hu_json_object_set(alloc, inst, "prompt", pv);
    hu_json_array_push(alloc, instances, inst);
    hu_json_object_set(alloc, root, "instances", instances);

    hu_json_value_t *params = hu_json_object_new(alloc);
    if (params) {
        hu_json_value_t *su = hu_json_string_new(alloc, storage_uri, strlen(storage_uri));
        if (su)
            hu_json_object_set(alloc, params, "storageUri", su);
        hu_json_value_t *ar = hu_json_string_new(alloc, req->aspect, strlen(req->aspect));
        if (ar)
            hu_json_object_set(alloc, params, "aspectRatio", ar);
        hu_json_value_t *sc = hu_json_number_new(alloc, 1.0);
        if (sc)
            hu_json_object_set(alloc, params, "sampleCount", sc);
        hu_json_value_t *dur = hu_json_number_new(alloc, req->duration_secs);
        if (dur)
            hu_json_object_set(alloc, params, "durationSeconds", dur);
        hu_json_object_set(alloc, root, "parameters", params);
    }

    hu_error_t err = hu_json_stringify(alloc, root, body, body_len);
    hu_json_free(alloc, root);
    if (err != HU_OK || !*body)
        return veo_fail(out, "failed to build request", HU_ERR_INVALID_ARGUMENT);
    return HU_OK;
}

/* POST predictLongRunning; the operation name to poll lands in op_name. */
static hu_error_t veo_submit(hu_allocator_t *alloc, const hu_vertex_auth_t *vauth,
                             const char *project, const char *region,
                             const hu_media_veo_request_t *req, char *op_name, size_t op_cap,
                             hu_tool_result_t *out) {
    char url[512];
    if (veo_model_url(url, sizeof(url), project, region, req->model_id, "predictLongRunning") !=
        HU_OK)
        return veo_fail(out, "URL too long", HU_ERR_INVALID_ARGUMENT);

    char *body = NULL;
    size_t body_len = 0;
    hu_error_t err = veo_submit_body(alloc, project, req, &body, &body_len, out);
    if (err != HU_OK)
        return err;

    hu_http_response_t resp = {0};
    err = hu_media_vertex_post_json(vauth, alloc, url, body, body_len, &resp);
    alloc->free(alloc->ctx, body, body_len + 1);
    if (err != HU_OK || resp.status_code < 200 || resp.status_code >= 300) {
        hu_http_response_free(alloc, &resp);
        return veo_fail(out, "Veo API submit failed", err != HU_OK ? err : HU_ERR_IO);
    }

    hu_json_value_t *submit_json = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &submit_json);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !submit_json) {
        hu_json_free(alloc, submit_json);
        return veo_fail(out, "failed to parse submit response", HU_ERR_PARSE);
    }
    const char *name = hu_json_get_string(submit_json, "name");
    int n = (name && name[0]) ? snprintf(op_name, op_cap, "%s", name) : 0;
    hu_json_free(alloc, submit_json);
    if (n <= 0 || (size_t)n >= op_cap)
        return veo_fail(out, "no operation name in response", HU_ERR_PARSE);
    return HU_OK;
}

/* response.videos[0].gcsUri, or gcs_uri left empty. */
static void veo_extract_gcs_uri(const hu_json_value_t *poll_json, char *gcs_uri, size_t cap) {
    const hu_json_value_t *response = hu_json_object_get(poll_json, "response");
    if (!response || response->type != HU_JSON_OBJECT)
        return;
    const hu_json_value_t *videos = hu_json_object_get(response, "videos");
    if (!videos || videos->type != HU_JSON_ARRAY || videos->data.array.len == 0)
        return;
    const hu_json_value_t *v0 = videos->data.array.items[0];
    if (!v0 || v0->type != HU_JSON_OBJECT)
        return;
    const char *uri = hu_json_get_string(v0, "gcsUri");
    if (uri && uri[0])
        snprintf(gcs_uri, cap, "%s", uri);
}

/* Poll fetchPredictOperation until done or the attempt budget runs out. A
 * transient poll failure is skipped, a token failure ends the loop. */
static hu_error_t veo_poll(hu_allocator_t *alloc, hu_vertex_auth_t *vauth, const char *project,
                           const char *region, const char *model_id, const char *op_name,
                           char *gcs_uri, size_t gcs_cap, hu_tool_result_t *out) {
    char poll_url[512];
    if (veo_model_url(poll_url, sizeof(poll_url), project, region, model_id,
                      "fetchPredictOperation") != HU_OK)
        return veo_fail(out, "poll URL too long", HU_ERR_INVALID_ARGUMENT);

    gcs_uri[0] = '\0';
    for (int attempt = 0; attempt < MVC_VEO_POLL_MAX_ATTEMPTS; attempt++) {
        mvc_sleep_secs(MVC_VEO_POLL_INTERVAL_SEC);
        if (hu_vertex_auth_ensure_token(vauth, alloc) != HU_OK)
            break;

        hu_json_value_t *poll_root = hu_json_object_new(alloc);
        hu_json_value_t *on_val = hu_json_string_new(alloc, op_name, strlen(op_name));
        if (poll_root && on_val)
            hu_json_object_set(alloc, poll_root, "operationName", on_val);
        else
            hu_json_free(alloc, on_val);
        char *poll_body = NULL;
        size_t poll_body_len = 0;
        hu_json_stringify(alloc, poll_root, &poll_body, &poll_body_len);
        hu_json_free(alloc, poll_root);
        if (!poll_body)
            continue;

        hu_http_response_t poll_resp = {0};
        hu_error_t err =
            hu_media_vertex_post_json(vauth, alloc, poll_url, poll_body, poll_body_len, &poll_resp);
        alloc->free(alloc->ctx, poll_body, poll_body_len + 1);
        if (err != HU_OK) {
            hu_http_response_free(alloc, &poll_resp);
            continue;
        }

        hu_json_value_t *poll_json = NULL;
        err = hu_json_parse(alloc, poll_resp.body, poll_resp.body_len, &poll_json);
        hu_http_response_free(alloc, &poll_resp);
        if (err != HU_OK || !poll_json) {
            hu_json_free(alloc, poll_json);
            continue;
        }
        bool done = hu_json_get_bool(poll_json, "done", false);
        if (done)
            veo_extract_gcs_uri(poll_json, gcs_uri, gcs_cap);
        hu_json_free(alloc, poll_json);
        if (done)
            break;
    }
    if (!gcs_uri[0])
        return veo_fail(out, "Veo generation timed out or failed", HU_ERR_TIMEOUT);
    return HU_OK;
}

/* GET the finished video (gs:// rewritten to the storage.googleapis.com
 * endpoint) and write it to /tmp/human_<file_tag>_<hex>.mp4. */
static hu_error_t veo_download(hu_allocator_t *alloc, const hu_vertex_auth_t *vauth,
                               const char *gcs_uri, const char *file_tag, char *mp4_path,
                               size_t mp4_cap, hu_tool_result_t *out) {
    char dl_url[2048];
    int dl =
        (strncmp(gcs_uri, "gs://", 5) == 0)
            ? snprintf(dl_url, sizeof(dl_url), "https://storage.googleapis.com/%s", gcs_uri + 5)
            : snprintf(dl_url, sizeof(dl_url), "%s", gcs_uri);
    if (dl <= 0 || (size_t)dl >= sizeof(dl_url))
        return veo_fail(out, "download URL too long", HU_ERR_INVALID_ARGUMENT);

    hu_http_response_t dl_resp = {0};
    hu_error_t err = hu_media_vertex_get(vauth, alloc, dl_url, &dl_resp);
    if (err != HU_OK || dl_resp.status_code < 200 || dl_resp.status_code >= 300) {
        hu_http_response_free(alloc, &dl_resp);
        return veo_fail(out, "failed to download Veo output", err != HU_OK ? err : HU_ERR_IO);
    }

    int pn =
        snprintf(mp4_path, mp4_cap, "/tmp/human_%s_%lx.mp4", file_tag, (unsigned long)time(NULL));
    if (pn <= 0 || (size_t)pn >= mp4_cap) {
        hu_http_response_free(alloc, &dl_resp);
        return veo_fail(out, "path overflow", HU_ERR_INVALID_ARGUMENT);
    }
    FILE *f = fopen(mp4_path, "wb");
    if (!f) {
        hu_http_response_free(alloc, &dl_resp);
        return veo_fail(out, "failed to write video file", HU_ERR_IO);
    }
    size_t want = dl_resp.body_len;
    size_t written = fwrite(dl_resp.body, 1, want, f);
    fclose(f);
    hu_http_response_free(alloc, &dl_resp);
    if (written != want)
        return veo_fail(out, "failed to write video file", HU_ERR_IO);
    return HU_OK;
}

hu_error_t hu_media_vertex_veo_generate(hu_allocator_t *alloc, hu_vertex_auth_t *vauth,
                                        const char *project, const char *region,
                                        const hu_media_veo_request_t *req, char *mp4_path,
                                        size_t mp4_cap, hu_tool_result_t *out) {
    if (!alloc || !vauth || !project || !region || !req || !req->model_id || !req->prompt ||
        !req->aspect || !req->file_tag || !mp4_path || mp4_cap == 0 || !out) {
        if (vauth)
            hu_vertex_auth_free(vauth);
        return HU_ERR_INVALID_ARGUMENT;
    }
    mp4_path[0] = '\0';
    char op_name[512];
    char gcs_uri[1024];
    hu_error_t err = veo_submit(alloc, vauth, project, region, req, op_name, sizeof(op_name), out);
    if (err == HU_OK)
        err = veo_poll(alloc, vauth, project, region, req->model_id, op_name, gcs_uri,
                       sizeof(gcs_uri), out);
    if (err == HU_OK)
        err = veo_download(alloc, vauth, gcs_uri, req->file_tag, mp4_path, mp4_cap, out);
    hu_vertex_auth_free(vauth);
    return err;
}

/* ── test-build mock ────────────────────────────────────────────────────── */

#if defined(HU_IS_TEST) && HU_IS_TEST
hu_error_t hu_media_vertex_mock_result(hu_allocator_t *alloc, const char *kind, const char *ext,
                                       const char *prompt, hu_tool_result_t *out) {
    if (!alloc || !kind || !ext || !prompt || !out)
        return HU_ERR_INVALID_ARGUMENT;
    size_t plen = strlen(prompt);
    if (plen > MVC_MOCK_PROMPT_MAX)
        plen = MVC_MOCK_PROMPT_MAX;
    char mock_path[256];
    int n = snprintf(mock_path, sizeof(mock_path), "/tmp/human_%s_mock_%.*s.%s", kind, (int)plen,
                     prompt, ext);
    if (n <= 0 || (size_t)n >= sizeof(mock_path)) {
        *out = hu_tool_result_fail("mock path overflow", 18);
        return HU_OK;
    }
    return hu_media_vertex_result_from_path(alloc, "%s", mock_path, out);
}
#endif /* HU_IS_TEST */
