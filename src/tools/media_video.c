/* media_video — Generate videos via Veo 3.1 on Vertex AI.
 * Submits a predictLongRunning request, polls until done, downloads the
 * resulting MP4 from the GCS URI, and writes it to a temp file. */

#include "human/tools/media_video.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/media_vertex_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define mv_sleep_secs(s) Sleep((s) * 1000)
#else
#include <unistd.h>
#define mv_sleep_secs(s) sleep((unsigned)(s))
#endif

#define MV_PROMPT_MAX        4000
#define MV_POLL_INTERVAL_SEC 15
#define MV_POLL_MAX_ATTEMPTS 12 /* 12 * 15s = 3 min max */

static const char *mv_name(void *ctx) {
    (void)ctx;
    return "media_video";
}

static const char *mv_desc(void *ctx) {
    (void)ctx;
    return "Generate a short video from a text prompt using Veo 3.1 on Vertex AI. "
           "Returns a local file path to the generated MP4.";
}

static const char *mv_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{"
           "\"prompt\":{\"type\":\"string\",\"description\":\"Video description\"},"
           "\"duration\":{\"type\":\"integer\","
           "\"enum\":[4,6,8],\"description\":\"Duration in seconds (default 8)\"},"
           "\"aspect_ratio\":{\"type\":\"string\","
           "\"enum\":[\"16:9\",\"9:16\"],\"description\":\"Aspect ratio (default 16:9)\"},"
           "\"model\":{\"type\":\"string\","
           "\"enum\":[\"veo_3.1\",\"veo_3.1_fast\",\"veo_3.1_lite\"],"
           "\"description\":\"Model variant (default veo_3.1)\"}},"
           "\"required\":[\"prompt\"]}";
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static const char *mv_model_id(const char *model) {
    if (strcmp(model, "veo_3.1_fast") == 0)
        return "veo-3.1-fast-generate-001";
    if (strcmp(model, "veo_3.1_lite") == 0)
        return "veo-3.1-lite-generate-001";
    return "veo-3.1-generate-001";
}
#endif

static bool mv_model_ok(const char *s) {
    if (!s)
        return false;
    return strcmp(s, "veo_3.1") == 0 || strcmp(s, "veo_3.1_fast") == 0 ||
           strcmp(s, "veo_3.1_lite") == 0;
}

static hu_error_t mv_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                             hu_tool_result_t *out) {
    (void)ctx;
    if (!alloc || !args || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *prompt = hu_json_get_string(args, "prompt");
    if (!prompt || !prompt[0]) {
        *out = hu_tool_result_fail("prompt is required", 18);
        return HU_OK;
    }
    if (strlen(prompt) > MV_PROMPT_MAX) {
        *out = hu_tool_result_fail("prompt too long", 15);
        return HU_OK;
    }

    const char *model = hu_json_get_string(args, "model");
    if (!model) {
        const char *env_model = getenv("HU_DEFAULT_VIDEO_MODEL");
        if (env_model && mv_model_ok(env_model))
            model = env_model;
        else
            model = "veo_3.1";
    } else if (!mv_model_ok(model)) {
        *out = hu_tool_result_fail("invalid model", 13);
        return HU_OK;
    }

    double duration = hu_json_get_number(args, "duration", 8.0);
    if (duration != 4.0 && duration != 6.0 && duration != 8.0)
        duration = 8.0;

    const char *aspect = hu_json_get_string(args, "aspect_ratio");
    if (!aspect)
        aspect = "16:9";
    else if (strcmp(aspect, "16:9") != 0 && strcmp(aspect, "9:16") != 0) {
        *out = hu_tool_result_fail("invalid aspect_ratio", 20);
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    return hu_media_vertex_mock_result(alloc, "vid", "mp4", prompt, out);
#else
    hu_vertex_auth_t vauth;
    const char *project = NULL;
    const char *region = NULL;
    if (!hu_media_vertex_open(&vauth, alloc, &project, &region, out))
        return HU_OK;

    hu_media_veo_request_t req = {
        .model_id = mv_model_id(model),
        .prompt = prompt,
        .aspect = aspect,
        .duration_secs = duration,
        .file_tag = "vid",
    };
    char mp4_path[256];
    hu_error_t err = hu_media_vertex_veo_generate(alloc, &vauth, project, region, &req, mp4_path,
                                                  sizeof(mp4_path), out);
    if (err != HU_OK)
        return err == HU_ERR_OUT_OF_MEMORY ? err : HU_OK; /* *out carries the failure */
    return hu_media_vertex_result_from_path(alloc, "Generated video saved to %s", mp4_path, out);
#endif
}

static const hu_tool_vtable_t media_video_vtable = {
    .execute = mv_execute,
    .name = mv_name,
    .description = mv_desc,
    .parameters_json = mv_params,
    .deinit = NULL,
};

hu_error_t hu_media_video_create(hu_allocator_t *alloc, hu_tool_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->vtable = &media_video_vtable;
    out->ctx = NULL;
    return HU_OK;
}
