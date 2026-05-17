#include "human/agent/output_validator.h"
#include "human/agent/validators/builtin.h"
#include "human/context/conversation.h"
#include <string.h>

static hu_error_t channel_tags_validate(void *ctx, hu_allocator_t *alloc,
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

    size_t new_len = hu_conversation_strip_channel_tags(buf, response_len);

    if (new_len == response_len && memcmp(buf, response, response_len) == 0) {
        alloc->free(alloc->ctx, buf, response_len + 1);
        out->decision = HU_VALIDATOR_PASS;
        return HU_OK;
    }

    /* The strip narrowed the content in-place (new_len < response_len).
     * Copy into a correctly-sized buffer so the caller can free with
     * text_len + 1, then discard the over-sized working buffer. */
    char *out_buf = (char *)alloc->alloc(alloc->ctx, new_len + 1);
    if (!out_buf) {
        alloc->free(alloc->ctx, buf, response_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(out_buf, buf, new_len);
    out_buf[new_len] = '\0';
    alloc->free(alloc->ctx, buf, response_len + 1);

    out->decision = HU_VALIDATOR_REWRITE;
    out->text = out_buf;
    out->text_len = new_len;
    out->text_owned = true;
    return HU_OK;
}

static const char *channel_tags_name(void *ctx) {
    (void)ctx;
    return "channel_tags";
}

static const hu_output_validator_vtable_t channel_tags_vtable = {
    .validate = channel_tags_validate,
    .name = channel_tags_name,
    .deinit = NULL,
};

hu_error_t hu_validator_channel_tags_create(hu_allocator_t *alloc, hu_output_validator_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    out->ctx = NULL;
    out->vtable = &channel_tags_vtable;
    return HU_OK;
}
