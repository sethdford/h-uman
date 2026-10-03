/* SHIELD-005 inbound crisis handling. See include/human/daemon/crisis.h. */
#include "human/core/log.h"
#include "human/core/log_redact.h"
#include "human/daemon/crisis.h"
#include "human/security/moderation.h"

#include <string.h>

hu_self_harm_tier_t hu_daemon_inbound_crisis_tier(hu_allocator_t *alloc, const char *text,
                                                  size_t text_len, const char *who, size_t who_len,
                                                  hu_observer_t *obs) {
    if (!alloc || !text || text_len == 0)
        return HU_SELF_HARM_NONE;
    hu_moderation_result_t mod;
    memset(&mod, 0, sizeof(mod));
    if (hu_moderation_check(alloc, text, text_len, &mod) != HU_OK)
        return HU_SELF_HARM_NONE;
    hu_gate_mode_t mode = hu_crisis_tiers_mode();
    hu_self_harm_tier_t tier;
    if (mode == HU_GATE_LIVE) {
        tier = mod.self_harm_tier;
    } else {
        /* Legacy: any self_harm hit got the full crisis directive. */
        tier = mod.self_harm ? HU_SELF_HARM_EXPLICIT : HU_SELF_HARM_NONE;
        if (mode == HU_GATE_SHADOW) {
            hu_self_harm_tier_t would = hu_self_harm_classify(text, text_len);
            if (would != HU_SELF_HARM_NONE || tier != HU_SELF_HARM_NONE)
                hu_log_info("human", obs,
                            "[HU_CRISIS_TIERS shadow] legacy=%s tier=%s changed=%d contact=%s",
                            hu_self_harm_tier_name(tier), hu_self_harm_tier_name(would),
                            would != tier ? 1 : 0, HU_LOG_WHO(who, who_len));
        }
    }
    if (tier != HU_SELF_HARM_NONE)
        hu_log_error("human", obs, "INBOUND crisis detected (tier=%s score=%.2f) contact=%s",
                     hu_self_harm_tier_name(tier), mod.self_harm_score, HU_LOG_WHO(who, who_len));
    return tier;
}

bool hu_daemon_crisis_prepend(hu_allocator_t *alloc, hu_self_harm_tier_t tier, char **ctx,
                              size_t *ctx_len) {
    size_t dlen = 0;
    const char *d = hu_self_harm_directive(tier, &dlen);
    if (!d)
        return false;
    if (!alloc || !ctx || !ctx_len)
        return true;
    size_t old_len = *ctx ? *ctx_len : 0;
    size_t new_len = dlen + old_len;
    char *merged = (char *)alloc->alloc(alloc->ctx, new_len + 1);
    if (merged) {
        memcpy(merged, d, dlen);
        if (*ctx && old_len > 0)
            memcpy(merged + dlen, *ctx, old_len);
        merged[new_len] = '\0';
        if (*ctx)
            alloc->free(alloc->ctx, *ctx, *ctx_len + 1);
        *ctx = merged;
        *ctx_len = new_len;
    }
    return true;
}

bool hu_daemon_crisis_ensure_resources(hu_allocator_t *alloc, hu_self_harm_tier_t inbound_tier,
                                       char **reply, size_t *reply_len) {
    if (!alloc || !reply || !*reply || !reply_len || inbound_tier != HU_SELF_HARM_EXPLICIT)
        return false;
    for (size_t i = 0; i + 3 <= *reply_len; i++)
        if (memcmp(*reply + i, "988", 3) == 0)
            return false; /* the model already gave the line: never twice */
    size_t rl = 0;
    const char *line = hu_self_harm_resource_line(&rl);
    size_t new_len = *reply_len + 2 + rl;
    char *merged = (char *)alloc->alloc(alloc->ctx, new_len + 1);
    if (!merged)
        return false;
    memcpy(merged, *reply, *reply_len);
    merged[*reply_len] = '\n';
    merged[*reply_len + 1] = '\n';
    memcpy(merged + *reply_len + 2, line, rl);
    merged[new_len] = '\0';
    alloc->free(alloc->ctx, *reply, *reply_len + 1);
    *reply = merged;
    *reply_len = new_len;
    return true;
}

bool hu_daemon_reply_blocked(hu_allocator_t *alloc, const char *reply, size_t reply_len,
                             hu_observer_t *obs) {
    if (!alloc || !reply || reply_len == 0)
        return false;
    hu_moderation_result_t mod;
    memset(&mod, 0, sizeof(mod));
    if (hu_moderation_check(alloc, reply, reply_len, &mod) != HU_OK)
        return false;
    if (!mod.violence && !mod.hate && !mod.sexual)
        return false;
    hu_log_warn("human", obs,
                "moderation blocked the reply (violence=%d hate=%d sexual=%d, %zu chars); "
                "nothing sent in its place",
                (int)mod.violence, (int)mod.hate, (int)mod.sexual, reply_len);
    return true;
}

bool hu_daemon_crisis_screen(hu_allocator_t *alloc, hu_self_harm_tier_t inbound_tier,
                             const char **text, size_t *len, char **owned, hu_observer_t *obs) {
    if (!text || !len || !hu_daemon_reply_blocked(alloc, *text, *len, obs))
        return true;
    if (owned && *owned) {
        alloc->free(alloc->ctx, *owned, *len + 1);
        *owned = NULL;
    }
    size_t n = 0;
    const char *floor = hu_self_harm_crisis_floor(inbound_tier, &n);
    *text = floor ? floor : "";
    *len = n;
    return floor != NULL;
}
