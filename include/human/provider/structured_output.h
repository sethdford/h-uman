/* Structured-output contract for chat replies.
 *
 * Canonical schema:
 *   {
 *     "reply": "the text the persona is sending",
 *     "reasoning": "optional internal CoT (never reaches the wire)"
 *   }
 *
 * Only providers that opt in via request->response_format == "json_schema"
 * (with request->response_schema populated) will emit this contract.
 * For opted-in providers, the agent passes the response body through
 * hu_structured_output_extract_reply to get the wire text. Providers
 * that don't natively support JSON enforcement can use the sentinel
 * fallback (<REPLY>...</REPLY>) and hu_structured_output_extract_sentinel.
 */
#ifndef HU_PROVIDER_STRUCTURED_OUTPUT_H
#define HU_PROVIDER_STRUCTURED_OUTPUT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the canonical chat-reply schema string (static storage). */
const char *hu_structured_output_chat_reply_schema(void);
size_t hu_structured_output_chat_reply_schema_len(void);

/* Parse a JSON body; extract "reply" + optional "reasoning". On success,
 * *out_reply and (if non-NULL) *out_reasoning are allocator-owned
 * NUL-terminated strings. Returns HU_ERR_PARSE on malformed JSON. */
hu_error_t hu_structured_output_extract_reply(hu_allocator_t *alloc, const char *body,
                                              size_t body_len, char **out_reply,
                                              size_t *out_reply_len, char **out_reasoning,
                                              size_t *out_reasoning_len);

/* Sentinel-extract fallback: find <REPLY>...</REPLY> and return the
 * inner text. Returns HU_ERR_PARSE if no markers found. */
hu_error_t hu_structured_output_extract_sentinel(hu_allocator_t *alloc, const char *body,
                                                 size_t body_len, char **out_reply,
                                                 size_t *out_reply_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_PROVIDER_STRUCTURED_OUTPUT_H */
