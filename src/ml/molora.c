/* molora.c — Sprint 7 US-7.8 (Init #02 Phase 1): static per-channel router.
 *
 * See include/human/ml/molora.h for the contract. This TU is only compiled
 * when HU_ENABLE_MOLORA is defined (gated in CMakeLists.txt). The header
 * itself is also fully guarded so callers in an OFF build see no symbols.
 *
 * Allocation discipline: the router owns no heap. `_init` only copies the
 * borrowed `adapter_path` pointers from the config; `_select` runs an
 * in-place stack-buffer normalization and a linear scan. This is safe to
 * call on every agent turn (the hot path). */

#ifdef HU_ENABLE_MOLORA

#include "human/ml/molora.h"

#include "human/config_types.h"
#include "human/core/error.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Forward declaration of the full config struct. We only touch the
 * personalization sub-struct here, which is part of hu_config_t in
 * config.h — including config_types.h above gives us the inner types
 * but the outer hu_config_t is defined in human/config.h. We accept it
 * as the `struct hu_config` opaque type and reach in via casting. */
#include "human/config.h"

/* ── Normalizer ─────────────────────────────────────────────────────────── */

size_t hu_molora_router_normalize_channel(const char *in, size_t in_len, char *out,
                                          size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!in || in_len == 0)
        return 0;

    /* Trim leading whitespace. */
    size_t start = 0;
    while (start < in_len && isspace((unsigned char)in[start]))
        start++;

    /* Determine effective end: first ':' or trailing whitespace, whichever
     * comes first, after start. */
    size_t end = start;
    while (end < in_len && in[end] != ':' && !isspace((unsigned char)in[end]))
        end++;

    if (end <= start)
        return 0;

    size_t copy_len = end - start;
    /* Truncate to out_cap - 1 so we always have room for the terminator. */
    if (copy_len >= out_cap)
        copy_len = out_cap - 1;

    for (size_t i = 0; i < copy_len; i++) {
        unsigned char ch = (unsigned char)in[start + i];
        out[i] = (char)tolower(ch);
    }
    out[copy_len] = '\0';
    return copy_len;
}

/* ── Init ───────────────────────────────────────────────────────────────── */

hu_error_t hu_molora_router_init(hu_molora_router_t *r, const struct hu_config *cfg) {
    if (!r)
        return HU_ERR_INVALID_ARGUMENT;

    /* Zero out unconditionally — any prior state is replaced. A `{0}`
     * struct is the disabled state (AC-7.8.4). */
    memset(r, 0, sizeof(*r));

    if (!cfg)
        return HU_OK;

    const hu_personalization_config_t *p = &cfg->personalization;
    r->default_adapter_path = p->lora_adapter_path; /* may be NULL */

    if (!p->molora.enabled)
        return HU_OK;

    /* Per design Q2: when personalization itself is disabled, molora stays
     * disabled too (router enabled flag remains false). The chat-time hook
     * checks `r->enabled` before calling _select, so this honors the gate
     * without needing extra branching at the call site. */
    if (!p->enabled)
        return HU_OK;

    r->enabled = true;

    size_t want = p->molora.count;
    if (want > HU_MOLORA_MAX_CHANNELS)
        want = HU_MOLORA_MAX_CHANNELS;

    for (size_t i = 0; i < want; i++) {
        const hu_molora_channel_entry_t *src = &p->molora.entries[i];
        if (!src->adapter_path || src->channel[0] == '\0')
            continue;
        hu_molora_entry_t *dst = &r->entries[r->count];
        /* Re-normalize defensively. Parser already normalized but a
         * config-mutator round-trip could in theory bypass that path,
         * and the cost is one strncpy + tolower per startup entry. */
        size_t n = hu_molora_router_normalize_channel(src->channel, strlen(src->channel),
                                                      dst->channel, sizeof(dst->channel));
        if (n == 0)
            continue;
        dst->adapter_path = src->adapter_path;
        r->count++;
    }
    return HU_OK;
}

/* ── Select ─────────────────────────────────────────────────────────────── */

const char *hu_molora_router_select(const hu_molora_router_t *r, const char *channel,
                                    size_t channel_len) {
    if (!r)
        return NULL;
    /* Disabled router → caller falls through to today's behavior. AC-7.8.4
     * also exercises this path with a `{0}` struct. */
    if (!r->enabled)
        return NULL;

    char key[HU_MOLORA_CHANNEL_NAME_MAX];
    size_t key_len = hu_molora_router_normalize_channel(channel, channel_len, key, sizeof(key));
    /* Empty channel id → no per-channel match; surface default. */
    if (key_len == 0)
        return r->default_adapter_path;

    for (size_t i = 0; i < r->count; i++) {
        const hu_molora_entry_t *e = &r->entries[i];
        if (e->channel[0] == '\0' || !e->adapter_path)
            continue;
        if (strncmp(e->channel, key, sizeof(e->channel)) == 0)
            return e->adapter_path;
    }
    return r->default_adapter_path;
}

#endif /* HU_ENABLE_MOLORA */
