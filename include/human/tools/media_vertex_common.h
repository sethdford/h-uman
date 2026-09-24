#ifndef HU_TOOLS_MEDIA_VERTEX_COMMON_H
#define HU_TOOLS_MEDIA_VERTEX_COMMON_H

/* Vertex AI plumbing shared by the media_gif, media_video and media_image
 * tools. Each tool used to carry its own copy of credential bootstrap,
 * project/region resolution, the bearer-then-POST sequence and (for the two
 * Veo tools) the whole submit/poll/download pipeline; the bearer lookup's
 * return was unchecked at all eight sites. One implementation here checks
 * it once. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/vertex_auth.h"
#include "human/tool.h"
#include <stdbool.h>
#include <stddef.h>

/* Load ADC credentials into *vauth (zeroed first), obtain a token, and
 * resolve the Vertex project and region for the current tool call:
 *   agent config -> GOOGLE_CLOUD_PROJECT / VERTEX_PROJECT   (project required)
 *   agent config -> GOOGLE_CLOUD_LOCATION -> "us-central1"
 * On failure *out holds the tool failure, *vauth is released, and false is
 * returned so the caller can `return HU_OK` directly. The returned strings
 * borrow from config or the environment. */
bool hu_media_vertex_open(hu_vertex_auth_t *vauth, hu_allocator_t *alloc, const char **project,
                          const char **region, hu_tool_result_t *out);

/* The resolution half of hu_media_vertex_open, for callers that manage their
 * own credentials (media_image's Gemini path, where an API key may win). */
bool hu_media_vertex_resolve_target(const char **project, const char **region,
                                    hu_tool_result_t *out);

/* Bearer-authenticated POST / GET. A failed bearer lookup returns that error
 * and never issues the request (*resp is left untouched), so a missing token
 * surfaces as HU_ERR_PROVIDER_AUTH instead of an unauthenticated 401. */
hu_error_t hu_media_vertex_post_json(const hu_vertex_auth_t *vauth, hu_allocator_t *alloc,
                                     const char *url, const char *body, size_t body_len,
                                     hu_http_response_t *resp);
hu_error_t hu_media_vertex_get(const hu_vertex_auth_t *vauth, hu_allocator_t *alloc,
                               const char *url, hu_http_response_t *resp);

/* Veo output bucket: agent config -> HU_VEO_STORAGE_URI ->
 * "gs://<project>-human-media/veo/" rendered into buf. NULL when none. */
const char *hu_media_vertex_veo_storage_uri(const char *project, char *buf, size_t buf_cap);

typedef struct hu_media_veo_request {
    const char *model_id; /* Vertex publisher model id, e.g. "veo-3.1-generate-001" */
    const char *prompt;
    const char *aspect;   /* "16:9" or "9:16" */
    double duration_secs; /* 4, 6 or 8 */
    const char *file_tag; /* output lands at /tmp/human_<file_tag>_<hex>.mp4 */
} hu_media_veo_request_t;

/* Veo end to end: predictLongRunning -> fetchPredictOperation polling ->
 * GCS download -> temp MP4. *vauth is released on every path.
 * HU_OK: mp4_path holds the file. Otherwise *out holds the tool failure and
 * the code says why (HU_ERR_OUT_OF_MEMORY when an allocation failed;
 * HU_ERR_PARSE / HU_ERR_TIMEOUT / HU_ERR_IO / ... otherwise). A tool reports
 * everything but OOM through *out: `return err == HU_ERR_OUT_OF_MEMORY ? err : HU_OK;` */
hu_error_t hu_media_vertex_veo_generate(hu_allocator_t *alloc, hu_vertex_auth_t *vauth,
                                        const char *project, const char *region,
                                        const hu_media_veo_request_t *req, char *mp4_path,
                                        size_t mp4_cap, hu_tool_result_t *out);

/* Owned media result: media_path is a copy of path and output is fmt rendered
 * with path (fmt takes exactly one %s). HU_ERR_OUT_OF_MEMORY with *out set to
 * the failure when a copy could not be made. */
hu_error_t hu_media_vertex_result_from_path(hu_allocator_t *alloc, const char *fmt,
                                            const char *path, hu_tool_result_t *out);

#if defined(HU_IS_TEST) && HU_IS_TEST
/* Test-build stand-in for a generated file: fills *out with an owned
 * "/tmp/human_<kind>_mock_<prompt, first 30 chars>.<ext>" media result. */
hu_error_t hu_media_vertex_mock_result(hu_allocator_t *alloc, const char *kind, const char *ext,
                                       const char *prompt, hu_tool_result_t *out);
#endif

#endif /* HU_TOOLS_MEDIA_VERTEX_COMMON_H */
