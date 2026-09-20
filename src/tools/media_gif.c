/* media_gif — Generate GIFs via Veo 3.1 Lite on Vertex AI.
 * Generates a short 4-second video, then converts to GIF using ffmpeg.
 * Falls back to returning the MP4 if ffmpeg is not available. */

#include "human/tools/media_gif.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/tool.h"
#include "human/tools/media_vertex_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#define mg_sleep_secs(s) Sleep((s) * 1000)
#else
#define mg_sleep_secs(s) sleep((unsigned)(s))
#endif

#define MG_PROMPT_MAX        4000
#define MG_POLL_INTERVAL_SEC 15
#define MG_POLL_MAX_ATTEMPTS 12

static const char *mg_name(void *ctx) {
    (void)ctx;
    return "media_gif";
}

static const char *mg_desc(void *ctx) {
    (void)ctx;
    return "Generate a custom GIF from a text prompt using Veo 3.1 Lite on Vertex AI. "
           "Creates a 4-second video and converts to GIF. Returns a local file path.";
}

static const char *mg_params(void *ctx) {
    (void)ctx;
    return "{\"type\":\"object\",\"properties\":{"
           "\"prompt\":{\"type\":\"string\",\"description\":\"GIF description\"},"
           "\"aspect_ratio\":{\"type\":\"string\","
           "\"enum\":[\"16:9\",\"9:16\"],\"description\":\"Aspect ratio (default 16:9)\"}},"
           "\"required\":[\"prompt\"]}";
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static hu_error_t mg_convert_to_gif(hu_allocator_t *alloc, const char *mp4_path, char *gif_path,
                                    size_t gif_cap) {
    int n = snprintf(gif_path, gif_cap, "%.*s.gif", (int)(strlen(mp4_path) - 4), mp4_path);
    if (n <= 0 || (size_t)n >= gif_cap)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    return HU_OK;
#else
    if (!hu_exe_on_path("ffmpeg"))
        return HU_ERR_NOT_SUPPORTED;

    const char *argv[] = {
        "ffmpeg",    "-i",         mp4_path, "-vf",    "fps=15,scale=480:-1:flags=lanczos",
        "-gifflags", "+transdiff", "-y",     gif_path, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 1024 * 64, &result);
    bool ok = (err == HU_OK && result.success);
    hu_run_result_free(alloc, &result);
    if (!ok)
        return HU_ERR_IO;

    return HU_OK;
#endif
}
#endif /* !(HU_IS_TEST) */

static hu_error_t mg_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                             hu_tool_result_t *out) {
    (void)ctx;
    if (!alloc || !args || !out)
        return HU_ERR_INVALID_ARGUMENT;

    const char *prompt = hu_json_get_string(args, "prompt");
    if (!prompt || !prompt[0]) {
        *out = hu_tool_result_fail("prompt is required", 18);
        return HU_OK;
    }
    if (strlen(prompt) > MG_PROMPT_MAX) {
        *out = hu_tool_result_fail("prompt too long", 15);
        return HU_OK;
    }

    const char *aspect = hu_json_get_string(args, "aspect_ratio");
    if (!aspect)
        aspect = "16:9";
    else if (strcmp(aspect, "16:9") != 0 && strcmp(aspect, "9:16") != 0) {
        *out = hu_tool_result_fail("invalid aspect_ratio", 20);
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    return hu_media_vertex_mock_result(alloc, "gif", "gif", prompt, out);
#else
    hu_vertex_auth_t vauth;
    const char *project = NULL;
    const char *region = NULL;
    if (!hu_media_vertex_open(&vauth, alloc, &project, &region, out))
        return HU_OK;

    hu_media_veo_request_t req = {
        .model_id = "veo-3.1-lite-generate-001",
        .prompt = prompt,
        .aspect = aspect,
        .duration_secs = 4.0,
        .file_tag = "gif",
    };
    char mp4_path[256];
    hu_error_t err = hu_media_vertex_veo_generate(alloc, &vauth, project, region, &req, mp4_path,
                                                  sizeof(mp4_path), out);
    if (err != HU_OK)
        return err == HU_ERR_OUT_OF_MEMORY ? err : HU_OK; /* *out carries the failure */

    /* Convert MP4 to GIF — fall back to MP4 if ffmpeg unavailable */
    char gif_path[256];
    const char *final_path = mp4_path;
    if (mg_convert_to_gif(alloc, mp4_path, gif_path, sizeof(gif_path)) == HU_OK) {
        (void)unlink(mp4_path);
        final_path = gif_path;
    }
    const char *fmt = (final_path == gif_path)
                          ? "Generated GIF saved to %s"
                          : "Generated video (GIF conversion unavailable) saved to %s";
    return hu_media_vertex_result_from_path(alloc, fmt, final_path, out);
#endif
}

static const hu_tool_vtable_t media_gif_vtable = {
    .execute = mg_execute,
    .name = mg_name,
    .description = mg_desc,
    .parameters_json = mg_params,
    .deinit = NULL,
};

hu_error_t hu_media_gif_create(hu_allocator_t *alloc, hu_tool_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->vtable = &media_gif_vtable;
    out->ctx = NULL;
    return HU_OK;
}
