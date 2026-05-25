#include "human/agent/mar.h"
#include "human/core/log.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef HU_IS_TEST
#define HU_IS_TEST 0
#endif

#if !defined(HU_IS_TEST) || !HU_IS_TEST
static const char *MAR_SYSTEM_PROMPTS[] = {
    [HU_MAR_ACTOR] = "You are the ACTOR. Generate a thorough response to the given task. "
                     "Be specific, cite evidence, and address all aspects of the task.",
    [HU_MAR_DIAGNOSER] = "You are the DIAGNOSER. Given the task and an actor's response, identify "
                         "weaknesses, errors, missing information, and logical gaps. Be specific.",
    [HU_MAR_CRITIC] =
        "You are the CRITIC. Given the task, the actor's response, and the diagnoser's "
        "analysis, critique the diagnosis. Are the identified issues valid? Are there "
        "issues the diagnoser missed? Provide a balanced assessment.",
    [HU_MAR_JUDGE] = "You are the JUDGE. Given the task, all previous responses, diagnoses, and "
                     "critiques, produce the definitive final answer. Synthesize the best elements "
                     "and correct any remaining errors.",
};
#endif

const char *hu_mar_role_name(hu_mar_role_t role) {
    switch (role) {
    case HU_MAR_ACTOR:
        return "actor";
    case HU_MAR_DIAGNOSER:
        return "diagnoser";
    case HU_MAR_CRITIC:
        return "critic";
    case HU_MAR_JUDGE:
        return "judge";
    default:
        return "unknown";
    }
}

static char *mar_dup(hu_allocator_t *alloc, const char *s, size_t len) {
    if (!s || len == 0)
        return NULL;
    char *d = (char *)alloc->alloc(alloc->ctx, len + 1);
    if (d) {
        memcpy(d, s, len);
        d[len] = '\0';
    }
    return d;
}

void hu_mar_phase_result_free(hu_allocator_t *alloc, hu_mar_phase_result_t *phase) {
    if (!alloc || !phase)
        return;
    if (phase->output) {
        alloc->free(alloc->ctx, phase->output, phase->output_len + 1);
        phase->output = NULL;
    }
}

void hu_mar_result_free(hu_allocator_t *alloc, hu_mar_result_t *result) {
    if (!alloc || !result)
        return;
    for (size_t i = 0; i < result->phase_count; i++)
        hu_mar_phase_result_free(alloc, &result->phases[i]);
    if (result->final_output) {
        alloc->free(alloc->ctx, result->final_output, result->final_output_len + 1);
        result->final_output = NULL;
    }
}

hu_error_t hu_mar_execute(hu_allocator_t *alloc, hu_provider_t *provider,
                          const hu_mar_config_t *config, const char *task, size_t task_len,
                          hu_mar_result_t *out) {
    if (!alloc || !provider || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    if (!config || !config->enabled || !task || task_len == 0) {
        if (config && !config->enabled) {
            static atomic_bool warned_mar_disabled = false;
            hu_log_info_once(&warned_mar_disabled, "mar", NULL,
                             "MAR (multi-agent reasoning) disabled — set "
                             "mar.enabled=true in config.json to enable "
                             "multi-round agent reasoning on complex tasks");
        }
        return HU_OK;
    }

    uint32_t max_rounds = config->max_rounds > 0 ? config->max_rounds : 2;

#if defined(HU_IS_TEST) && HU_IS_TEST
    for (uint32_t round = 0; round < max_rounds; round++) {
        for (int role = HU_MAR_ACTOR; role < HU_MAR_ROLE_COUNT; role++) {
            if (out->phase_count >= sizeof(out->phases) / sizeof(out->phases[0]))
                break;

            hu_mar_phase_result_t *phase = &out->phases[out->phase_count];
            memset(phase, 0, sizeof(*phase));
            phase->role = (hu_mar_role_t)role;
            phase->success = true;

            char mock_output[256];
            int n = snprintf(mock_output, sizeof(mock_output), "[%s round %u] processed: %.*s",
                             hu_mar_role_name((hu_mar_role_t)role), round + 1,
                             (int)(task_len < 100 ? task_len : 100), task);
            if (n > 0) {
                phase->output = mar_dup(alloc, mock_output, (size_t)n);
                phase->output_len = (size_t)n;
            }
            phase->elapsed_ms = 10;
            out->phase_count++;
        }
        out->rounds_completed = round + 1;
    }

    if (out->phase_count > 0) {
        hu_mar_phase_result_t *last = &out->phases[out->phase_count - 1];
        out->final_output = mar_dup(alloc, last->output, last->output_len);
        out->final_output_len = last->output_len;
    }
    out->consensus_reached = true;

    (void)provider;
    (void)config->actor_model;
    (void)config->judge_model;
    (void)config->critic_model;
    return HU_OK;
#else
    char context[16384];
    size_t ctx_pos = 0;
    int cn;

    cn = snprintf(context, sizeof(context), "Task: %.*s\n\n",
                  (int)(task_len < 4000 ? task_len : 4000), task);
    if (cn > 0)
        ctx_pos = (size_t)cn;

    for (uint32_t round = 0; round < max_rounds; round++) {
        for (int role = HU_MAR_ACTOR; role < HU_MAR_ROLE_COUNT; role++) {
            if (out->phase_count >= sizeof(out->phases) / sizeof(out->phases[0]))
                break;

            const char *sys_prompt = MAR_SYSTEM_PROMPTS[role];
            const char *model = config->actor_model;
            size_t model_len = config->actor_model_len;

            if (role == HU_MAR_JUDGE && config->judge_model) {
                model = config->judge_model;
                model_len = config->judge_model_len;
            } else if ((role == HU_MAR_CRITIC || role == HU_MAR_DIAGNOSER) &&
                       config->critic_model) {
                model = config->critic_model;
                model_len = config->critic_model_len;
            }

            if (!provider->vtable || !provider->vtable->chat_with_system) {
                out->phases[out->phase_count].role = (hu_mar_role_t)role;
                out->phases[out->phase_count].success = false;
                out->phase_count++;
                continue;
            }

            clock_t start = clock();
            char *resp = NULL;
            size_t resp_len = 0;
            hu_error_t err = provider->vtable->chat_with_system(
                provider->ctx, alloc, sys_prompt, strlen(sys_prompt), context, ctx_pos,
                model ? model : "", model_len, 0.5, &resp, &resp_len);

            hu_mar_phase_result_t *phase = &out->phases[out->phase_count];
            memset(phase, 0, sizeof(*phase));
            phase->role = (hu_mar_role_t)role;
            phase->elapsed_ms = (int64_t)((clock() - start) * 1000 / CLOCKS_PER_SEC);

            if (err == HU_OK && resp) {
                phase->output = resp;
                phase->output_len = resp_len;
                phase->success = true;

                size_t avail = sizeof(context) - ctx_pos;
                int an = snprintf(context + ctx_pos, avail, "\n[%s]: %.*s\n",
                                  hu_mar_role_name((hu_mar_role_t)role),
                                  (int)(resp_len < 2000 ? resp_len : 2000), resp);
                if (an > 0 && (size_t)an < avail)
                    ctx_pos += (size_t)an;
            } else {
                phase->success = false;
            }
            out->phase_count++;
        }
        out->rounds_completed = round + 1;
    }

    if (out->phase_count > 0) {
        for (size_t i = out->phase_count; i > 0; i--) {
            if (out->phases[i - 1].success && out->phases[i - 1].output) {
                out->final_output =
                    mar_dup(alloc, out->phases[i - 1].output, out->phases[i - 1].output_len);
                out->final_output_len = out->phases[i - 1].output_len;
                break;
            }
        }
    }
    out->consensus_reached = (out->final_output != NULL);

    return HU_OK;
#endif
}
