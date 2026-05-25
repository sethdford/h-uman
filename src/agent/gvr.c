#include "human/agent/gvr.h"
#include "human/core/log.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef HU_IS_TEST
#define HU_IS_TEST 0
#endif

#if !defined(HU_IS_TEST) || !HU_IS_TEST
static const char GVR_VERIFY_SYS[] =
    "You are a verification assistant. Given the original prompt and a response, "
    "check for factual errors, hallucinations, tool-output misuse, or incomplete answers.\n\n"
    "IMPORTANT: The response may be written in a specific persona style (casual, terse, "
    "lowercase, slang). This is INTENTIONAL — do NOT flag persona style as an error. "
    "Only flag genuine factual errors, hallucinations, or completely missing information.\n"
    "A short, casual response that addresses the question is VALID.\n\n"
    "Reply with exactly one line: PASS or FAIL followed by a brief critique.\n"
    "Format: PASS: <reason> or FAIL: <critique>";

static const char GVR_REVISE_SYS[] =
    "You are a revision assistant. Given the original prompt, a previous response, "
    "and a critique identifying errors, produce a corrected response that addresses "
    "all issues raised in the critique.\n\n"
    "IMPORTANT: Preserve the original response's tone, style, and length. If the original "
    "was casual and short, the revision should be casual and short. Do NOT make it longer "
    "or more formal unless the critique specifically requires more information.";
#endif

static char *gvr_dup(hu_allocator_t *alloc, const char *s, size_t len) {
    if (!s || len == 0)
        return NULL;
    char *d = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

#if !defined(HU_IS_TEST) || !HU_IS_TEST
static hu_gvr_verdict_t parse_verdict(const char *text, size_t text_len,
                                      const char **critique_start, size_t *critique_out_len) {
    *critique_start = NULL;
    *critique_out_len = 0;

    if (!text || text_len == 0)
        return HU_GVR_FAIL;

    /* Skip leading whitespace */
    size_t i = 0;
    while (i < text_len && (text[i] == ' ' || text[i] == '\n' || text[i] == '\r'))
        i++;

    if (text_len - i >= 4 && (memcmp(text + i, "PASS", 4) == 0)) {
        if (i + 4 < text_len && text[i + 4] == ':') {
            size_t cs = i + 5;
            while (cs < text_len && text[cs] == ' ')
                cs++;
            *critique_start = text + cs;
            *critique_out_len = text_len - cs;
        }
        return HU_GVR_PASS;
    }

    if (text_len - i >= 4 && (memcmp(text + i, "FAIL", 4) == 0)) {
        if (i + 4 < text_len && text[i + 4] == ':') {
            size_t cs = i + 5;
            while (cs < text_len && text[cs] == ' ')
                cs++;
            *critique_start = text + cs;
            *critique_out_len = text_len - cs;
        } else {
            *critique_start = "Verification failed";
            *critique_out_len = 19;
        }
        return HU_GVR_FAIL;
    }

    /* Default: treat unrecognized output as FAIL */
    *critique_start = text + i;
    *critique_out_len = text_len - i;
    return HU_GVR_FAIL;
}
#endif

hu_error_t hu_gvr_check(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                        size_t model_len, const char *original_prompt, size_t original_prompt_len,
                        const char *response, size_t response_len, hu_gvr_check_result_t *out) {
    if (!alloc || !provider || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    if (!original_prompt || !response) {
        out->verdict = HU_GVR_PASS;
        return HU_OK;
    }

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Deterministic mock: responses containing "error" or "wrong" fail verification */
    bool has_error = false;
    for (size_t i = 0; i + 4 < response_len && !has_error; i++) {
        if (memcmp(response + i, "error", 5) == 0 || memcmp(response + i, "wrong", 5) == 0)
            has_error = true;
    }
    if (has_error) {
        out->verdict = HU_GVR_FAIL;
        out->critique = gvr_dup(alloc, "Contains factual error", 22);
        out->critique_len = 22;
    } else {
        out->verdict = HU_GVR_PASS;
    }
    (void)provider;
    (void)model;
    (void)model_len;
    (void)original_prompt_len;
    return HU_OK;
#else
    if (!provider->vtable || !provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;

    char user_msg[8192];
    int n = snprintf(user_msg, sizeof(user_msg),
                     "Original prompt:\n\"%.*s\"\n\nResponse to verify:\n\"%.*s\"",
                     (int)(original_prompt_len < 3000 ? original_prompt_len : 3000),
                     original_prompt, (int)(response_len < 4000 ? response_len : 4000), response);
    if (n < 0)
        n = 0;

    char *verify_out = NULL;
    size_t verify_out_len = 0;
    hu_error_t err = provider->vtable->chat_with_system(
        provider->ctx, alloc, GVR_VERIFY_SYS, sizeof(GVR_VERIFY_SYS) - 1, user_msg, (size_t)n,
        model ? model : "", model_len, 0.0, &verify_out, &verify_out_len);

    if (err != HU_OK)
        return err;

    const char *crit_start = NULL;
    size_t crit_len = 0;
    out->verdict = parse_verdict(verify_out, verify_out_len, &crit_start, &crit_len);
    if (crit_start && crit_len > 0)
        out->critique = gvr_dup(alloc, crit_start, crit_len);
    out->critique_len = out->critique ? crit_len : 0;

    if (verify_out)
        alloc->free(alloc->ctx, verify_out, verify_out_len + 1);

    return HU_OK;
#endif
}

hu_error_t hu_gvr_revise(hu_allocator_t *alloc, hu_provider_t *provider, const char *model,
                         size_t model_len, const char *original_prompt, size_t original_prompt_len,
                         const char *response, size_t response_len, const char *critique,
                         size_t critique_len, char **revised_out, size_t *revised_out_len) {
    if (!alloc || !provider || !revised_out || !revised_out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *revised_out = NULL;
    *revised_out_len = 0;

#if defined(HU_IS_TEST) && HU_IS_TEST
    /* Deterministic mock: replace "error"/"wrong" with "correct" */
    const char *fixed = "The corrected response with accurate information.";
    size_t fixed_len = strlen(fixed);
    *revised_out = gvr_dup(alloc, fixed, fixed_len);
    *revised_out_len = fixed_len;
    (void)provider;
    (void)model;
    (void)model_len;
    (void)original_prompt;
    (void)original_prompt_len;
    (void)response;
    (void)response_len;
    (void)critique;
    (void)critique_len;
    return *revised_out ? HU_OK : HU_ERR_OUT_OF_MEMORY;
#else
    if (!provider->vtable || !provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;

    char user_msg[12288];
    int n = snprintf(user_msg, sizeof(user_msg),
                     "Original prompt:\n\"%.*s\"\n\n"
                     "Previous response:\n\"%.*s\"\n\n"
                     "Critique (issues to fix):\n\"%.*s\"\n\n"
                     "Produce a corrected response:",
                     (int)(original_prompt_len < 2000 ? original_prompt_len : 2000),
                     original_prompt, (int)(response_len < 4000 ? response_len : 4000), response,
                     (int)(critique_len < 2000 ? critique_len : 2000), critique);
    if (n < 0)
        n = 0;

    return provider->vtable->chat_with_system(
        provider->ctx, alloc, GVR_REVISE_SYS, sizeof(GVR_REVISE_SYS) - 1, user_msg, (size_t)n,
        model ? model : "", model_len, 0.0, revised_out, revised_out_len);
#endif
}

hu_error_t hu_gvr_pipeline(hu_allocator_t *alloc, hu_provider_t *provider,
                           const hu_gvr_config_t *config, const char *generator_model,
                           size_t generator_model_len, const char *original_prompt,
                           size_t original_prompt_len, const char *initial_response,
                           size_t initial_response_len, hu_gvr_pipeline_result_t *out) {
    if (!alloc || !provider || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    if (!config || !config->enabled) {
        static atomic_bool warned_gvr_disabled = false;
        hu_log_info_once(&warned_gvr_disabled, "gvr", NULL,
                         "GVR (generator-verifier-revision) disabled — "
                         "set gvr.enabled=true in config.json to enable "
                         "response verification");
        out->final_verdict = HU_GVR_PASS;
        out->revisions_performed = 0;
        out->final_content = gvr_dup(alloc, initial_response, initial_response_len);
        out->final_content_len = initial_response_len;
        return out->final_content ? HU_OK : HU_ERR_OUT_OF_MEMORY;
    }

    uint32_t max_rev = config->max_revisions > 0 ? config->max_revisions : 2;
    const char *verify_model = config->verifier_model ? config->verifier_model : generator_model;
    size_t verify_model_len =
        config->verifier_model ? config->verifier_model_len : generator_model_len;

    const char *current = initial_response;
    size_t current_len = initial_response_len;
    char *owned_current = NULL;

    for (uint32_t rev = 0; rev <= max_rev; rev++) {
        hu_gvr_check_result_t check = {0};
        hu_error_t err =
            hu_gvr_check(alloc, provider, verify_model, verify_model_len, original_prompt,
                         original_prompt_len, current, current_len, &check);
        if (err != HU_OK) {
            if (owned_current)
                alloc->free(alloc->ctx, owned_current, current_len + 1);
            return err;
        }

        if (check.verdict == HU_GVR_PASS) {
            hu_gvr_check_result_free(alloc, &check);
            out->final_verdict = HU_GVR_PASS;
            out->revisions_performed = rev;
            if (owned_current) {
                out->final_content = owned_current;
                out->final_content_len = current_len;
            } else {
                out->final_content = gvr_dup(alloc, current, current_len);
                out->final_content_len = current_len;
            }
            return HU_OK;
        }

        /* FAIL — attempt revision if we haven't exhausted attempts */
        if (rev < max_rev) {
            char *revised = NULL;
            size_t revised_len = 0;
            err = hu_gvr_revise(alloc, provider, generator_model, generator_model_len,
                                original_prompt, original_prompt_len, current, current_len,
                                check.critique, check.critique_len, &revised, &revised_len);
            hu_gvr_check_result_free(alloc, &check);
            if (err != HU_OK) {
                if (owned_current)
                    alloc->free(alloc->ctx, owned_current, current_len + 1);
                return err;
            }

            if (owned_current)
                alloc->free(alloc->ctx, owned_current, current_len + 1);
            owned_current = revised;
            current = revised;
            current_len = revised_len;
        } else {
            /* Exhausted revisions — return last attempt */
            hu_gvr_check_result_free(alloc, &check);
            out->final_verdict = HU_GVR_FAIL;
            out->revisions_performed = rev;
            if (owned_current) {
                out->final_content = owned_current;
                out->final_content_len = current_len;
            } else {
                out->final_content = gvr_dup(alloc, current, current_len);
                out->final_content_len = current_len;
            }
            return HU_OK;
        }
    }

    /* Should not reach here, but handle gracefully */
    out->final_verdict = HU_GVR_FAIL;
    if (owned_current) {
        out->final_content = owned_current;
        out->final_content_len = current_len;
    }
    return HU_OK;
}

void hu_gvr_check_result_free(hu_allocator_t *alloc, hu_gvr_check_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->critique) {
        alloc->free(alloc->ctx, result->critique, result->critique_len + 1);
        result->critique = NULL;
        result->critique_len = 0;
    }
}

void hu_gvr_pipeline_result_free(hu_allocator_t *alloc, hu_gvr_pipeline_result_t *result) {
    if (!alloc || !result)
        return;
    if (result->final_content) {
        alloc->free(alloc->ctx, result->final_content, result->final_content_len + 1);
        result->final_content = NULL;
        result->final_content_len = 0;
    }
}
