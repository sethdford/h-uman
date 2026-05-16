#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/context/conversation.h"
#include <string.h>

static hu_error_t ai_phrases_validate(void *ctx, hu_allocator_t *alloc,
                                      const hu_validator_context_t *vctx, const char *response,
                                      size_t response_len, hu_validator_result_t *out) {
    (void)ctx;
    (void)vctx;
    memset(out, 0, sizeof(*out));

    char *buf = (char *)alloc->alloc(alloc->ctx, response_len + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, response, response_len);
    buf[response_len] = '\0';

    size_t new_len = hu_conversation_strip_ai_phrases(buf, response_len);

    if (new_len == response_len && memcmp(buf, response, response_len) == 0) {
        alloc->free(alloc->ctx, buf, response_len + 1);
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    out->decision = HU_VALIDATOR_REWRITE;
    out->text = buf;
    out->text_len = new_len;
    out->text_owned = true;
    return HU_OK;
}

static const char *ai_phrases_name(void *ctx) {
    (void)ctx;
    return "ai_phrases";
}

static const hu_output_validator_vtable_t ai_phrases_vtable = {
    .validate = ai_phrases_validate,
    .name = ai_phrases_name,
    .deinit = NULL,
};

hu_error_t hu_validator_ai_phrases_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &ai_phrases_vtable;
    return HU_OK;
}
