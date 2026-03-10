/*
 * Superhuman emotional first aid service — detects crisis and distress keywords.
 */
#include "human/agent/superhuman.h"
#include "human/agent/superhuman_emotional.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CRISIS_KEYWORDS[] = {"suicidal", "kill myself", "end it all", "can't go on",
                                        "want to die", NULL};

static const char *DISTRESS_KEYWORDS[] = {"overwhelmed", "can't cope", "breaking down", "panic",
                                           "crisis", NULL};

static bool contains_keyword(const char *text, size_t text_len, const char *keyword) {
    size_t klen = strlen(keyword);
    if (klen > text_len)
        return false;
    for (size_t i = 0; i <= text_len - klen; i++) {
        bool match = true;
        for (size_t j = 0; j < klen; j++) {
            char a = (char)((unsigned char)text[i + j]);
            char b = (char)((unsigned char)keyword[j]);
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

static bool has_crisis(const char *text, size_t text_len) {
    for (size_t i = 0; CRISIS_KEYWORDS[i]; i++) {
        if (contains_keyword(text, text_len, CRISIS_KEYWORDS[i]))
            return true;
    }
    return false;
}

static bool has_distress(const char *text, size_t text_len) {
    for (size_t i = 0; DISTRESS_KEYWORDS[i]; i++) {
        if (contains_keyword(text, text_len, DISTRESS_KEYWORDS[i]))
            return true;
    }
    return false;
}

static hu_error_t emotional_build_context(void *ctx, hu_allocator_t *alloc, char **out,
                                           size_t *out_len) {
    hu_superhuman_emotional_ctx_t *ectx = (hu_superhuman_emotional_ctx_t *)ctx;
    if (!ectx || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *text = ectx->last_text;
    size_t text_len = ectx->last_text_len;
    if (text_len == 0)
        return HU_OK;

    if (has_crisis(text, text_len)) {
        static const char SAFETY[] =
            "SAFETY: The user may be in crisis. Prioritize empathy, validation, and crisis "
            "resources. Do not minimize. Suggest professional help (988, crisis line) if "
            "appropriate. Stay present and non-judgmental.";
        *out = hu_strndup(alloc, SAFETY, sizeof(SAFETY) - 1);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = sizeof(SAFETY) - 1;
        return HU_OK;
    }

    if (has_distress(text, text_len)) {
        static const char CONTAINING[] =
            "The user may be experiencing distress. Offer containing presence: validate "
            "feelings, avoid rushing to solutions. Ask how they are and what might help. "
            "Be warm and steady.";
        *out = hu_strndup(alloc, CONTAINING, sizeof(CONTAINING) - 1);
        if (!*out)
            return HU_ERR_OUT_OF_MEMORY;
        *out_len = sizeof(CONTAINING) - 1;
        return HU_OK;
    }

    return HU_OK;
}

static hu_error_t emotional_observe(void *ctx, hu_allocator_t *alloc, const char *text,
                                     size_t text_len, const char *role, size_t role_len) {
    hu_superhuman_emotional_ctx_t *ectx = (hu_superhuman_emotional_ctx_t *)ctx;
    if (!ectx)
        return HU_ERR_INVALID_ARGUMENT;

    size_t copy_len = text_len;
    if (copy_len >= HU_EMOTIONAL_LAST_TEXT_CAP)
        copy_len = HU_EMOTIONAL_LAST_TEXT_CAP - 1;
    if (text && copy_len > 0)
        memcpy(ectx->last_text, text, copy_len);
    ectx->last_text[copy_len] = '\0';
    ectx->last_text_len = copy_len;

    size_t role_copy = role_len;
    if (role_copy >= sizeof(ectx->last_role))
        role_copy = sizeof(ectx->last_role) - 1;
    if (role && role_copy > 0)
        memcpy(ectx->last_role, role, role_copy);
    ectx->last_role[role_copy] = '\0';
    ectx->last_role_len = role_copy;

    (void)alloc;
    return HU_OK;
}

hu_error_t hu_superhuman_emotional_service(hu_superhuman_emotional_ctx_t *ctx,
                                           hu_superhuman_service_t *out) {
    if (!ctx || !out)
        return HU_ERR_INVALID_ARGUMENT;
    out->name = "Emotional First Aid";
    out->build_context = emotional_build_context;
    out->observe = emotional_observe;
    out->ctx = ctx;
    return HU_OK;
}
