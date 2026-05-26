/* outbound/pipeline.c — Sprint 59 outbound validation pipeline runner.
 *
 * The runner is intentionally thin: it walks an ordered list of
 * stages, dispatches each one's `run` function, acts on the verdict.
 *
 * Verdict semantics (from include/human/agent/outbound_pipeline.h):
 *   SEND       → advance to next stage; deliver if last
 *   REWRITE    → apply verdict.replacement to msg, restart at stage[0]
 *                (capped at one rewrite per pipeline run)
 *   REGENERATE → bubble up to caller; caller re-prompts LLM
 *   REJECT     → bubble up to caller; caller drops message
 *
 * The pipeline does NOT make LLM calls; it only emits verdicts.
 * REGENERATE is a contract back to the caller, not a recursive
 * pipeline invocation.
 *
 * Per ~/.claude/rules/silent-config-gated-subsystems.md: when the
 * pipeline runs for the first time in a process, it logs the active
 * stage list. See pipeline_configs.c for the per-path table.
 */

#include "human/agent/outbound_pipeline.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "human/core/log.h"

/* Pipeline is just an ordered array of stage pointers.
 * The stage structs themselves live as static singletons in
 * pipeline_configs.c — the pipeline borrows them. */
struct hu_outbound_pipeline {
    hu_outbound_stage_t **stages; /* borrowed; not owned by pipeline */
    size_t stage_count;
    hu_outbound_path_t path;
    hu_allocator_t *alloc;
};

/* Defined in pipeline_configs.c — builds the stages array for a path. */
hu_error_t hu_outbound_pipeline_configs_build_stages(hu_allocator_t *alloc, hu_outbound_path_t path,
                                                     hu_outbound_stage_t ***out_stages,
                                                     size_t *out_count);

/* One-shot startup log per path. */
static atomic_uint_least32_t s_startup_log_mask = 0;

static void log_startup_if_first(hu_outbound_path_t path, hu_outbound_stage_t **stages,
                                 size_t count) {
    uint32_t bit = (uint32_t)1u << (uint32_t)path;
    uint32_t prev = atomic_fetch_or_explicit(&s_startup_log_mask, bit, memory_order_relaxed);
    if (prev & bit)
        return;

    char buf[256];
    size_t off = 0;
    int n = snprintf(buf, sizeof(buf), "outbound-pipeline path=%s stages=[",
                     hu_outbound_path_name(path) ? hu_outbound_path_name(path) : "?");
    if (n > 0 && (size_t)n < sizeof(buf))
        off = (size_t)n;
    for (size_t i = 0; i < count && off + 32 < sizeof(buf); i++) {
        n = snprintf(buf + off, sizeof(buf) - off, "%s%s", i ? "," : "",
                     stages[i]->name ? stages[i]->name : "?");
        if (n > 0 && (size_t)n < sizeof(buf) - off)
            off += (size_t)n;
    }
    if (off + 2 < sizeof(buf)) {
        buf[off++] = ']';
        buf[off] = '\0';
    }
    hu_log_info("outbound", NULL, "%s", buf);
}

hu_error_t hu_outbound_pipeline_for_path(hu_allocator_t *alloc, hu_outbound_path_t path,
                                         hu_outbound_pipeline_t **out) {
    if (!alloc || !out || path >= HU_OUTBOUND_PATH_COUNT)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

    hu_outbound_stage_t **stages = NULL;
    size_t count = 0;
    hu_error_t err = hu_outbound_pipeline_configs_build_stages(alloc, path, &stages, &count);
    if (err != HU_OK)
        return err;

    hu_outbound_pipeline_t *pipeline =
        (hu_outbound_pipeline_t *)alloc->alloc(alloc->ctx, sizeof(*pipeline));
    if (!pipeline) {
        if (stages)
            alloc->free(alloc->ctx, stages, count * sizeof(*stages));
        return HU_ERR_OUT_OF_MEMORY;
    }
    pipeline->stages = stages;
    pipeline->stage_count = count;
    pipeline->path = path;
    pipeline->alloc = alloc;

    log_startup_if_first(path, stages, count);
    *out = pipeline;
    return HU_OK;
}

void hu_outbound_pipeline_destroy(hu_outbound_pipeline_t *pipeline) {
    if (!pipeline)
        return;
    hu_allocator_t *alloc = pipeline->alloc;
    if (pipeline->stages)
        alloc->free(alloc->ctx, pipeline->stages,
                    pipeline->stage_count * sizeof(*pipeline->stages));
    alloc->free(alloc->ctx, pipeline, sizeof(*pipeline));
}

void hu_outbound_verdict_clear(hu_outbound_verdict_t *verdict, hu_allocator_t *alloc) {
    if (!verdict || !alloc)
        return;
    if (verdict->replacement) {
        alloc->free(alloc->ctx, verdict->replacement, verdict->replacement_len + 1);
        verdict->replacement = NULL;
        verdict->replacement_len = 0;
    }
}

hu_outbound_verdict_t hu_outbound_verdict_send(void) {
    hu_outbound_verdict_t v = {0};
    v.kind = HU_OUTBOUND_SEND;
    return v;
}

hu_outbound_verdict_t hu_outbound_verdict_reject(const char *reason) {
    hu_outbound_verdict_t v = {0};
    v.kind = HU_OUTBOUND_REJECT;
    v.reason = reason;
    return v;
}

hu_outbound_verdict_t hu_outbound_verdict_regenerate(const char *reason, const char *hint) {
    hu_outbound_verdict_t v = {0};
    v.kind = HU_OUTBOUND_REGENERATE;
    v.reason = reason;
    v.regenerate_hint = hint;
    return v;
}

hu_outbound_verdict_t hu_outbound_verdict_rewrite(const char *reason, char *replacement,
                                                  size_t replacement_len) {
    hu_outbound_verdict_t v = {0};
    v.kind = HU_OUTBOUND_REWRITE;
    v.reason = reason;
    v.replacement = replacement;
    v.replacement_len = replacement_len;
    return v;
}

const char *hu_outbound_path_name(hu_outbound_path_t path) {
    switch (path) {
    case HU_OUTBOUND_PATH_REACTIVE:
        return "reactive";
    case HU_OUTBOUND_PATH_PROACTIVE:
        return "proactive";
    case HU_OUTBOUND_PATH_F25:
        return "f25";
    case HU_OUTBOUND_PATH_TEMPORAL:
        return "temporal";
    case HU_OUTBOUND_PATH_SCHEDULED:
        return "scheduled";
    case HU_OUTBOUND_PATH_BURST:
        return "burst";
    case HU_OUTBOUND_PATH_COUNT:
        return NULL;
    }
    return NULL;
}

/* Apply a REWRITE verdict in-place: the verdict owns `replacement`;
 * transfer ownership into msg->content (freeing the old content).
 * After this call, the verdict's replacement pointer is cleared so
 * verdict_clear is a no-op. */
static void apply_rewrite(hu_outbound_message_t *msg, hu_outbound_verdict_t *verdict,
                          hu_allocator_t *alloc) {
    if (msg->content)
        alloc->free(alloc->ctx, msg->content, msg->content_len + 1);
    msg->content = verdict->replacement;
    msg->content_len = verdict->replacement_len;
    verdict->replacement = NULL;
    verdict->replacement_len = 0;
}

hu_error_t hu_outbound_pipeline_run(hu_outbound_pipeline_t *pipeline, hu_outbound_message_t *msg,
                                    hu_outbound_context_t *ctx,
                                    hu_outbound_verdict_t *out_verdict) {
    if (!pipeline || !msg || !ctx || !out_verdict)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ctx->alloc)
        return HU_ERR_INVALID_ARGUMENT;

    *out_verdict = hu_outbound_verdict_send();

    int rewrites_remaining = 1; /* hard cap; design.md Phase A spec */

restart:
    for (size_t i = 0; i < pipeline->stage_count; i++) {
        hu_outbound_stage_t *stage = pipeline->stages[i];
        if (!stage || !stage->run)
            continue;

        hu_outbound_verdict_t v = stage->run(stage, msg, ctx);

        /* Structured log per stage. Cheap; helps operators grep. */
        hu_log_info("outbound", NULL, "stage=%s verdict=%d reason=%s path=%s",
                    stage->name ? stage->name : "?", (int)v.kind, v.reason ? v.reason : "-",
                    hu_outbound_path_name(ctx->path));

        switch (v.kind) {
        case HU_OUTBOUND_SEND:
            continue;
        case HU_OUTBOUND_REWRITE:
            if (rewrites_remaining <= 0) {
                /* Rewrite budget exhausted — verdict_clear frees the
                 * rewrite buffer; treat as REJECT to be safe. */
                hu_outbound_verdict_clear(&v, ctx->alloc);
                *out_verdict = hu_outbound_verdict_reject("rewrite_budget_exhausted");
                return HU_OK;
            }
            rewrites_remaining--;
            apply_rewrite(msg, &v, ctx->alloc);
            goto restart;
        case HU_OUTBOUND_REGENERATE:
            if (ctx->regenerate_budget <= 0) {
                *out_verdict = hu_outbound_verdict_reject("regenerate_budget_exhausted");
                return HU_OK;
            }
            ctx->regenerate_budget--;
            *out_verdict = v;
            return HU_OK;
        case HU_OUTBOUND_REJECT:
            *out_verdict = v;
            return HU_OK;
        }
    }

    /* All stages returned SEND. */
    *out_verdict = hu_outbound_verdict_send();
    return HU_OK;
}
