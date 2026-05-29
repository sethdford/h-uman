#include "human/core/error.h"
#include "human/doctor.h"
#include "human/doctor/check.h"
#include "human/doctor/check_local_voice.h"
#include "human/doctor/check_outbound_stats.h"
#include "human/doctor/check_prompt_budget.h"
#include "human/doctor/check_provider.h"
#include "human/doctor/check_reaction_collection_wired.h"
#include "human/doctor/check_reflection_loop.h"
#include "human/doctor/check_unified_dispatch.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations of external check vtables */
extern hu_doctor_check_t hu_doctor_check_chatdb;
extern hu_doctor_check_t hu_doctor_check_provider; /* Sprint 54 US-C3.3 */

#define HU_DOCTOR_REGISTRY_INITIAL_CAP 16

typedef struct hu_doctor_registry {
    hu_allocator_t *alloc;
    hu_doctor_check_t *checks;
    size_t count;
    size_t cap;
} hu_doctor_registry_t;

hu_error_t hu_doctor_registry_init(hu_allocator_t *alloc, hu_doctor_registry_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_doctor_registry_t *r =
        (hu_doctor_registry_t *)alloc->alloc(alloc->ctx, sizeof(hu_doctor_registry_t));
    if (!r)
        return HU_ERR_OUT_OF_MEMORY;

    r->alloc = alloc;
    r->checks = (hu_doctor_check_t *)alloc->alloc(alloc->ctx, sizeof(hu_doctor_check_t) *
                                                                  HU_DOCTOR_REGISTRY_INITIAL_CAP);
    if (!r->checks) {
        alloc->free(alloc->ctx, r, sizeof(hu_doctor_registry_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    r->count = 0;
    r->cap = HU_DOCTOR_REGISTRY_INITIAL_CAP;

    *out = r;
    return HU_OK;
}

hu_error_t hu_doctor_registry_register(hu_doctor_registry_t *r, const hu_doctor_check_t *check) {
    if (!r || !check)
        return HU_ERR_INVALID_ARGUMENT;

    if (r->count >= r->cap) {
        size_t new_cap = r->cap * 2;
        hu_doctor_check_t *new_checks = (hu_doctor_check_t *)r->alloc->alloc(
            r->alloc->ctx, sizeof(hu_doctor_check_t) * new_cap);
        if (!new_checks)
            return HU_ERR_OUT_OF_MEMORY;

        memcpy(new_checks, r->checks, sizeof(hu_doctor_check_t) * r->count);
        r->alloc->free(r->alloc->ctx, r->checks, sizeof(hu_doctor_check_t) * r->cap);
        r->checks = new_checks;
        r->cap = new_cap;
    }

    memcpy(&r->checks[r->count], check, sizeof(hu_doctor_check_t));
    r->count++;
    return HU_OK;
}

hu_error_t hu_doctor_registry_run_all(hu_doctor_registry_t *r, void *ctx,
                                      hu_doctor_check_result_t *out_results, size_t *out_count,
                                      size_t cap) {
    if (!r || !out_results || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    if (cap < r->count)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < r->count; i++) {
        hu_doctor_check_t *check = &r->checks[i];
        if (check->run) {
            out_results[i] = check->run(check, ctx);
        } else {
            out_results[i] =
                (hu_doctor_check_result_t){HU_DOCTOR_FAIL, "check has no run function", NULL};
        }
    }

    *out_count = r->count;
    return HU_OK;
}

void hu_doctor_registry_free(hu_doctor_registry_t *r) {
    if (!r)
        return;
    if (r->checks) {
        r->alloc->free(r->alloc->ctx, r->checks, sizeof(hu_doctor_check_t) * r->cap);
    }
    r->alloc->free(r->alloc->ctx, r, sizeof(hu_doctor_registry_t));
}

/* Sprint 55 Phase 2 accessors. */
size_t hu_doctor_registry_count(const hu_doctor_registry_t *r) {
    return r ? r->count : 0;
}

const char *hu_doctor_registry_check_name(const hu_doctor_registry_t *r, size_t index) {
    if (!r || index >= r->count)
        return NULL;
    return r->checks[index].name;
}

/* ────────────────────────────────────────────────────────────────────
 * Default check registration
 * ──────────────────────────────────────────────────────────────────── */

/* Adapter functions: convert existing hu_doctor_check_* functions to
 * vtable entries. Each adapter wraps a legacy check function that returns
 * hu_error_t and appends to diag_item arrays. We adapt it to the
 * hu_doctor_check_result_t return value.
 *
 * 2026-05 fix: the legacy adapters returned `(result_t){verdict, "", NULL}` —
 * discarding the per-item diagnostic messages the wrapped check had
 * populated. That made `human doctor` show "error" with NO reason text
 * for every check that failed via this path (operator had to grep logs
 * to figure out what went wrong). The helper below derives reason +
 * detail_json from the items the wrapped check added during THIS call. */

/* Map severity to stable lowercase wire string. */
static const char *severity_to_string(hu_diag_severity_t s) {
    switch (s) {
    case HU_DIAG_OK:
        return "ok";
    case HU_DIAG_WARN:
        return "warn";
    case HU_DIAG_ERR:
        return "err";
    default:
        return "unknown";
    }
}

/* Append `s` to `buf` at offset `*off`, JSON-escaping `"` `\` and control
 * bytes. Truncates rather than overflows. */
static void append_json_escaped(char *buf, size_t cap, size_t *off, const char *s) {
    if (!s)
        s = "";
    for (const char *p = s; *p && *off + 2 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            buf[(*off)++] = '\\';
            buf[(*off)++] = (char)c;
        } else if (c == '\n') {
            if (*off + 2 < cap) {
                buf[(*off)++] = '\\';
                buf[(*off)++] = 'n';
            }
        } else if (c < 0x20) {
            /* Drop other control bytes. */
            continue;
        } else {
            buf[(*off)++] = (char)c;
        }
    }
    if (*off < cap)
        buf[*off] = '\0';
}

/* Derive (reason, detail_json) from the items the wrapped check appended
 * during ITS call only — `items` should point at uctx->items + start_off
 * and `count` = uctx->count - start_off. The caller passes its own static
 * buffers so each adapter's result stays valid across the run_all loop. */
static hu_doctor_check_result_t derive_check_result(hu_doctor_verdict_t verdict,
                                                    const hu_diag_item_t *items, size_t count,
                                                    char *reason_buf, size_t reason_cap,
                                                    char *detail_buf, size_t detail_cap) {
    if (!items || count == 0 || !reason_buf || reason_cap == 0)
        return (hu_doctor_check_result_t){verdict, "", NULL};

    /* Reason: first ERR item's message, else first WARN, else first item. */
    const hu_diag_item_t *primary = NULL;
    for (size_t i = 0; i < count; i++) {
        if (items[i].severity == HU_DIAG_ERR) {
            primary = &items[i];
            break;
        }
    }
    if (!primary) {
        for (size_t i = 0; i < count; i++) {
            if (items[i].severity == HU_DIAG_WARN) {
                primary = &items[i];
                break;
            }
        }
    }
    if (!primary)
        primary = &items[0];

    snprintf(reason_buf, reason_cap, "%s", primary->message ? primary->message : "");

    /* detail_json: array of {severity, category, message} for every item
     * so the operator can see the FULL diagnostic, not just the first. */
    if (!detail_buf || detail_cap < 4)
        return (hu_doctor_check_result_t){verdict, reason_buf, NULL};

    /* safe_off = offset right after the last item that fit COMPLETELY.
     * If a subsequent item runs out of room mid-construction, we rewind
     * to safe_off and emit "]" — yielding valid JSON with a truncated
     * array rather than malformed text mid-string. */
    size_t off = 0;
    detail_buf[off++] = '[';
    size_t safe_off = off;
    for (size_t i = 0; i < count; i++) {
        size_t item_start = off;
        if (i > 0) {
            if (off + 1 >= detail_cap)
                break;
            detail_buf[off++] = ',';
        }
        const char *sev = severity_to_string(items[i].severity);
        int n = snprintf(detail_buf + off, detail_cap - off, "{\"severity\":\"%s\",\"category\":\"",
                         sev);
        if (n < 0 || (size_t)n >= detail_cap - off) {
            off = item_start;
            break;
        }
        off += (size_t)n;
        append_json_escaped(detail_buf, detail_cap, &off, items[i].category);
        int m = snprintf(detail_buf + off, detail_cap - off, "\",\"message\":\"");
        if (m < 0 || (size_t)m >= detail_cap - off) {
            off = item_start;
            break;
        }
        off += (size_t)m;
        append_json_escaped(detail_buf, detail_cap, &off, items[i].message);
        /* Need room for closing `"}`. */
        if (off + 2 >= detail_cap) {
            off = item_start;
            break;
        }
        detail_buf[off++] = '"';
        detail_buf[off++] = '}';
        safe_off = off;
    }
    /* Close the array at the last safe boundary so the output is always
     * valid JSON, even if it truncates the trailing items. */
    if (safe_off + 1 < detail_cap) {
        detail_buf[safe_off++] = ']';
        detail_buf[safe_off] = '\0';
    } else if (safe_off < detail_cap) {
        detail_buf[safe_off] = '\0';
    }

    return (hu_doctor_check_result_t){verdict, reason_buf, detail_buf};
}

/* Sprint 55 Phase 2 extension: cfg pointer added at the END so
 * legacy callers passing a 4-field struct still read the first 4
 * fields correctly (cfg defaults to NULL → checks fall back to NA).
 * New-style checks (provider_smoke) read cfg via the public ctx
 * struct in human/doctor/check_provider.h. */
typedef struct {
    hu_allocator_t *alloc;
    hu_diag_item_t *items;
    size_t count;
    size_t cap;
    const void *cfg; /* Sprint 55 Phase 2 — borrowed `const hu_config *` */
} hu_doctor_adapter_ctx_t;

/* Each adapter snapshots uctx->count BEFORE the wrapped check appends so
 * derive_check_result sees ONLY the items this check produced. Static
 * buffers are function-local: each adapter owns its own scratch space,
 * so result.reason / result.detail_json stay live across the whole
 * registry_run_all loop. */
#define ADAPTER_RETURN(verdict_expr)                                                              \
    do {                                                                                          \
        static char s_reason[256];                                                                \
        static char s_detail[2048];                                                               \
        return derive_check_result((verdict_expr), uctx->items + start_off,                       \
                                   uctx->count - start_off, s_reason, sizeof(s_reason), s_detail, \
                                   sizeof(s_detail));                                             \
    } while (0)

/* Wrapper: install check */
static hu_doctor_check_result_t run_install_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    hu_error_t err =
        hu_doctor_check_install(uctx->alloc, NULL, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: config_semantics check */
static hu_doctor_check_result_t run_config_semantics_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    /* config_semantics REQUIRES cfg — passing NULL makes it return
     * INVALID_ARGUMENT silently without populating any item. Plumb the
     * cfg from uctx (set in main.c's adapter_ctx). */
    const hu_config_t *cfg = (const hu_config_t *)uctx->cfg;
    hu_error_t err = hu_doctor_check_config_semantics(uctx->alloc, cfg, &uctx->items, &uctx->count);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: security check */
static hu_doctor_check_result_t run_security_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    hu_error_t err = hu_doctor_check_security(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: memory_health check */
static hu_doctor_check_result_t run_memory_health_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    /* memory_health REQUIRES cfg to know which backend to probe. */
    const hu_config_t *cfg = (const hu_config_t *)uctx->cfg;
    hu_error_t err =
        hu_doctor_check_memory_health(uctx->alloc, cfg, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: skills check */
static hu_doctor_check_result_t run_skills_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    hu_error_t err = hu_doctor_check_skills(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: imessage check */
static hu_doctor_check_result_t run_imessage_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    /* Use a stale_after_secs of 600 (10 minutes) by default */
    hu_error_t err =
        hu_doctor_check_imessage(uctx->alloc, 0, 600, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: verifier check */
static hu_doctor_check_result_t run_verifier_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    hu_error_t err =
        hu_doctor_check_verifier(uctx->alloc, 0, 600, 0.3, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: scheduler check */
static hu_doctor_check_result_t run_scheduler_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    hu_error_t err =
        hu_doctor_check_scheduler(uctx->alloc, 0, 600, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: response_pipeline check */
static hu_doctor_check_result_t run_response_pipeline_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    hu_error_t err =
        hu_doctor_check_response_pipeline(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: inference check */
static hu_doctor_check_result_t run_inference_check(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    size_t start_off = uctx->count;
    hu_error_t err = hu_doctor_check_inference(uctx->alloc, &uctx->items, &uctx->count, &uctx->cap);
    ADAPTER_RETURN((err == HU_OK) ? HU_DOCTOR_PASS : HU_DOCTOR_FAIL);
}

/* Wrapper: chatdb_readable check (external vtable) */
static hu_doctor_check_result_t run_chatdb_readable_check(hu_doctor_check_t *self, void *ctx) {
    (void)ctx;
    /* Delegate directly to the external check vtable */
    return hu_doctor_check_chatdb.run(self, ctx);
}

/* Wrapper: provider_smoke check (external vtable) — Sprint 54 US-C3.3 /
 *                                                    Sprint 55 Phase 2.
 *
 * Constructs the public ctx struct {alloc, cfg} from our private
 * adapter struct, then delegates. The external check is allocator-
 * and config-aware (needed for hu_provider_create_from_config).
 *
 * NULL cfg is handled inside the check (returns NA). */
static hu_doctor_check_result_t run_provider_smoke_check(hu_doctor_check_t *self, void *ctx) {
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_doctor_check_provider_ctx_t pctx = {
        .alloc = uctx ? uctx->alloc : NULL,
        .cfg = uctx ? uctx->cfg : NULL,
    };
    return hu_doctor_check_provider.run(self, &pctx);
}

/* Wrapper: prompt_budget check (external vtable) — Sprint 55 B3 Task 5.
 * Constructs the public ctx struct {cfg} from the adapter and delegates.
 * NULL cfg is handled inside the check (returns NA). */
static hu_doctor_check_result_t run_prompt_budget_check(hu_doctor_check_t *self, void *ctx) {
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_doctor_check_prompt_budget_ctx_t pbctx = {
        .cfg = uctx ? (const struct hu_config *)uctx->cfg : NULL,
    };
    return hu_doctor_check_prompt_budget.run(self, &pbctx);
}

/* Wrapper: reaction_collection_wired check — 2026-05 audit follow-up.
 * Catches the silent failure where cfg.enabled=true but the binary was
 * built with HU_ENABLE_RL_FULL=OFF (recorder compiled out). */
static hu_doctor_check_result_t run_reaction_collection_wired_check(hu_doctor_check_t *self,
                                                                    void *ctx) {
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_doctor_check_reaction_collection_wired_ctx_t rwctx = {
        .cfg = uctx ? (const struct hu_config *)uctx->cfg : NULL,
    };
    return hu_doctor_check_reaction_collection_wired.run(self, &rwctx);
}

static hu_doctor_check_result_t run_local_voice_check(hu_doctor_check_t *self, void *ctx) {
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_doctor_check_local_voice_ctx_t lvctx = {
        .cfg = uctx ? (const struct hu_config *)uctx->cfg : NULL,
    };
    return hu_doctor_check_local_voice.run(self, &lvctx);
}

/* Sprint 60 (sprint-59 STATUS.md item #5) — outbound pipeline stats.
 * The check is informational (always PASS); ctx is unused because
 * the snapshot reads from process-wide static state in
 * src/agent/outbound/stats.c. */
static hu_doctor_check_result_t run_outbound_stats_check(hu_doctor_check_t *self, void *ctx) {
    (void)ctx;
    return hu_doctor_check_outbound_stats.run(self, NULL);
}

/* M3 Dispatch — unified-dispatch health check. Same shape as
 * outbound_stats: ctx ignored, reads process-wide atomics directly
 * via hu_guard_reject_stats_snapshot() inside the check body. */
static hu_doctor_check_result_t run_unified_dispatch_check(hu_doctor_check_t *self, void *ctx) {
    (void)ctx;
    return hu_doctor_check_unified_dispatch.run(self, NULL);
}

/* T12 — M2 reflection-loop health check. Reads cfg from the adapter
 * ctx; db is NULL in the doctor CLI path (no daemon) → check returns
 * NA "no db available". Tests call the underlying vtable directly
 * with a fully-populated ctx. */
static hu_doctor_check_result_t run_reflection_loop_check(hu_doctor_check_t *self, void *ctx) {
    hu_doctor_adapter_ctx_t *uctx = (hu_doctor_adapter_ctx_t *)ctx;
    hu_doctor_check_reflection_loop_ctx_t rctx = {
        .cfg = uctx ? (const struct hu_config *)uctx->cfg : NULL,
        .db = NULL, /* doctor CLI has no db handle; check returns NA */
    };
    return hu_doctor_check_reflection_loop.run(self, &rctx);
}

hu_error_t hu_doctor_registry_register_defaults(hu_doctor_registry_t *r) {
    if (!r)
        return HU_ERR_INVALID_ARGUMENT;

    /* Define check entries in registration order per architecture.md §2 */
    hu_doctor_check_t checks[] = {
        {"install", "Verifies binary and config layout", run_install_check, NULL, NULL},
        {"config_semantics", "Checks configuration semantics", run_config_semantics_check, NULL,
         NULL},
        {"security", "Validates security posture", run_security_check, NULL, NULL},
        {"memory_health", "Checks memory backend health", run_memory_health_check, NULL, NULL},
        {"skills", "Verifies skill registry", run_skills_check, NULL, NULL},
        {"chatdb_readable", "Verifies ~/Library/Messages/chat.db is readable (FDA check)",
         run_chatdb_readable_check, NULL, NULL},
        {"provider_smoke", "Verifies the configured AI provider can be instantiated",
         run_provider_smoke_check, NULL, NULL},
        {"prompt_budget", "Reports prompt-budget config state (observer + trim gate)",
         run_prompt_budget_check, NULL, NULL},
        {"reaction_collection_wired",
         "Catches reaction_collection.enabled=true but HU_ENABLE_RL_FULL=OFF "
         "(silent-failure guard)",
         run_reaction_collection_wired_check, NULL, NULL},
        {"local_voice", "Reports local Gemma+LoRA voice-path readiness (routing/adapter/url/curl)",
         run_local_voice_check, NULL, NULL},
        {"outbound_stats", "Per-stage × per-verdict counters for the outbound pipeline (Sprint 60)",
         run_outbound_stats_check, NULL, NULL},
        {"unified_dispatch",
         "M3 unified-dispatch G9 retry-outcome health (rescued/thrashed/starved)",
         run_unified_dispatch_check, NULL, NULL},
        {"reflection_loop", "M2 reflection-loop health (enabled/cold-start/healthy/broken/stale)",
         run_reflection_loop_check, NULL, NULL},
        {"imessage", "Diagnoses iMessage channel", run_imessage_check, NULL, NULL},
        {"verifier", "Checks response verifier health", run_verifier_check, NULL, NULL},
        {"scheduler", "Checks scheduler status", run_scheduler_check, NULL, NULL},
        {"response_pipeline", "Checks response pipeline", run_response_pipeline_check, NULL, NULL},
        {"inference", "Validates inference configuration", run_inference_check, NULL, NULL},
    };

    size_t num_checks = sizeof(checks) / sizeof(checks[0]);
    for (size_t i = 0; i < num_checks; i++) {
        hu_error_t err = hu_doctor_registry_register(r, &checks[i]);
        if (err != HU_OK)
            return err;
    }

    return HU_OK;
}
