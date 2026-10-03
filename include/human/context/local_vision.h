#ifndef HU_CONTEXT_LOCAL_VISION_H
#define HU_CONTEXT_LOCAL_VISION_H

/* Local, on-device photo understanding (HU_LOCAL_VISION=off|shadow|live,
 * default off).
 *
 * Under local_only=enforce every inbound photo became "[They sent a photo]":
 * the cloud is off-limits and the serving model is text-only. This path keeps
 * the pixels on the machine:
 *
 *   - caption: a small VLM (Gemma 4 E2B 4-bit) behind mlx_vlm.server on
 *     127.0.0.1:8746, OpenAI /v1/chat/completions with an image_url part;
 *   - text:    the Apple Vision OCR helper (tools/hu-vision-ocr), which is the
 *     ONLY source of quoted words. A VLM invents text ("ernest" on a photo
 *     with none, 2026-10-02 spike), so a quoted span in the caption that the
 *     OCR does not contain is cut, never passed on.
 *
 * Both run in parallel under one 8 s budget. Any failure, an empty result or
 * a URL that is not http://127.0.0.1 leaves the caller on today's placeholder.
 * Deliberately NOT routed through hu_vision_describe_image: that is the cloud
 * path and its privacy kill-switch; this one never leaves loopback.
 *
 * Env:
 *   HU_LOCAL_VISION        off | shadow | live (default off)
 *   HU_LOCAL_VISION_URL    default HU_LOCAL_VISION_DEFAULT_URL; must be loopback
 *   HU_LOCAL_VISION_MODEL  default HU_LOCAL_VISION_DEFAULT_MODEL
 *   HU_LOCAL_VISION_OCR    OCR helper path, default $HOME/.local/bin/hu-vision-ocr
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

#define HU_LOCAL_VISION_DEFAULT_URL   "http://127.0.0.1:8746/v1/chat/completions"
#define HU_LOCAL_VISION_DEFAULT_MODEL "mlx-community/gemma-4-e2b-it-4bit"
#define HU_LOCAL_VISION_TIMEOUT_MS    8000L

/* HU_LOCAL_VISION, read on every call (default OFF; unknown values are OFF). */
hu_gate_mode_t hu_local_vision_mode(void);

/* True only for http://127.0.0.1 with an optional :port and path. Anything
 * else — localhost, another host, userinfo, "127.0.0.1.example.com" — is
 * refused, so a mistyped URL can never carry a photo off the machine. */
bool hu_local_vision_url_is_loopback(const char *url);

/* Pure. The description that goes inside "[They sent a photo: ...]":
 *   "<caption>. Text in it: \"<ocr>\""   (either half may be absent)
 * Quoted spans in the caption that the OCR text does not contain are removed
 * together with their lead-in ("that says", "with the text"); *disagree is set
 * when that happened. Both halves are flattened to one line, brackets become
 * parentheses and double quotes in the OCR become single quotes, so neither
 * can close the marker. HU_ERR_NOT_FOUND when nothing usable is left.
 * Caller frees *out (len + 1). */
hu_error_t hu_local_vision_compose(hu_allocator_t *alloc, const char *caption, size_t caption_len,
                                   const char *ocr, size_t ocr_len, char **out, size_t *out_len,
                                   bool *disagree);

/* Run the pipeline for the image at path, per hu_local_vision_mode():
 *   OFF    -> HU_ERR_NOT_SUPPORTED, nothing runs.
 *   SHADOW -> runs, logs one "[HU_LOCAL_VISION shadow]" aggregate line
 *             (latency, byte counts, disagreement flag — never the text), then
 *             HU_ERR_NOT_SUPPORTED with *out NULL: the caller does what it
 *             does today.
 *   LIVE   -> HU_OK with the composed description (caller frees len + 1), or
 *             an error with *out NULL when it failed or timed out. */
hu_error_t hu_local_vision_describe(hu_allocator_t *alloc, const char *path, size_t path_len,
                                    char **out, size_t *out_len);

#if defined(HU_IS_TEST) && HU_IS_TEST
/* Test seams. Without them a test build never opens a socket or runs a
 * process: both stages answer HU_ERR_NOT_SUPPORTED. */
typedef hu_error_t (*hu_local_vision_caption_fn)(hu_allocator_t *alloc, const char *url,
                                                 const char *body, size_t body_len, long timeout_ms,
                                                 char **resp, size_t *resp_len);
typedef hu_error_t (*hu_local_vision_ocr_fn)(hu_allocator_t *alloc, const char *path,
                                             long timeout_ms, char **json, size_t *json_len);
void hu_local_vision_set_test_hooks(hu_local_vision_caption_fn caption, hu_local_vision_ocr_fn ocr);
/* The last aggregate log line written (shadow or live), "" before any. */
const char *hu_local_vision_test_last_log(void);
#endif

#endif /* HU_CONTEXT_LOCAL_VISION_H */
