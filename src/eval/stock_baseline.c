#include "human/eval/stock_baseline.h"

#include "human/providers/helpers.h"

#include <string.h>

typedef struct hu_stock_baseline_ctx {
    hu_provider_t *provider; /* borrowed — not freed in deinit */
} hu_stock_baseline_ctx_t;

static hu_error_t stock_judge(struct hu_eval_judge_external *self,
                              const hu_eval_judge_pair_t *pair,
                              hu_eval_judge_verdict_t *out) {
    if (!self || !self->ctx || !pair || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_stock_baseline_ctx_t *ctx = (hu_stock_baseline_ctx_t *)self->ctx;
    hu_provider_t *prov = ctx->provider;
    if (!prov || !prov->vtable || !prov->vtable->chat)
        return HU_ERR_NOT_SUPPORTED;

    (void)hu_provider_unload_adapter(prov, "", 1);

    hu_allocator_t alloc = hu_system_allocator();
    hu_chat_request_t req;
    memset(&req, 0, sizeof(req));
    hu_chat_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.role = HU_ROLE_USER;
    msg.content = pair->prompt;
    msg.content_len = pair->prompt ? strlen(pair->prompt) : 0;
    req.messages = &msg;
    req.messages_count = 1;

    hu_chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    hu_error_t e = prov->vtable->chat(prov->ctx, &alloc, &req, "stock", 5, 0.0, &resp);
    if (e != HU_OK)
        return e;

    int prefer_a = 0;
    if (resp.content && pair->response_a && strstr(resp.content, pair->response_a))
        prefer_a = 1;
    else if (resp.content && pair->response_b && strstr(resp.content, pair->response_b))
        prefer_a = 0;
    else
        prefer_a = -1;

    out->prefer_a = prefer_a;
    out->confidence = 0.5;
    out->rationale = "stock baseline chat";
    hu_chat_response_free(&alloc, &resp);
    return HU_OK;
}

static const char *stock_name(struct hu_eval_judge_external *self) {
    (void)self;
    return "stock";
}

static void stock_deinit(struct hu_eval_judge_external *self) {
    if (!self || !self->ctx)
        return;
    hu_allocator_t alloc = hu_system_allocator();
    alloc.free(alloc.ctx, self->ctx, sizeof(hu_stock_baseline_ctx_t));
    self->ctx = NULL;
    self->vtable = NULL;
}

static const hu_eval_judge_external_vtable_t STOCK_VTABLE = {
    .judge = stock_judge,
    .name = stock_name,
    .deinit = stock_deinit,
};

hu_error_t hu_stock_baseline_create(hu_allocator_t *alloc, hu_provider_t *provider,
                                    hu_eval_judge_external_t *out) {
    if (!alloc || !provider || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_stock_baseline_ctx_t *ctx =
        (hu_stock_baseline_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*ctx));
    if (!ctx)
        return HU_ERR_OUT_OF_MEMORY;
    ctx->provider = provider;
    out->vtable = &STOCK_VTABLE;
    out->ctx = ctx;
    return HU_OK;
}
