/* src/agent/turn/turn_silence.c — S8 silence gate, carved verbatim out of
 * hu_agent_turn (src/agent/agent_turn.c) by the phase-1 carve
 * (docs/superpowers/plans/2026-09-30-agent-turn-carve-phase1.md). The only
 * edit: the early-return tail (free 13 turn-body buffers, clear the current
 * agent, return HU_OK) became `return hu_turn_step_return(HU_OK);` — those
 * buffers are hu_agent_turn locals, so the frees and the clear stay at its
 * call site. Carries one borrowed SQLite handle (hu_sqlite_memory_get_db) for
 * the experience record; the type comes through human/memory.h, never a
 * direct <sqlite3.h> include (plan gap G2). */
#include "../agent_internal.h"
#include "human/agent/turn.h"
#include "human/core/string.h"
#include "human/humanness.h"
#ifdef HU_ENABLE_SQLITE
#include "human/experience.h"
#endif
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

hu_turn_step_t hu_turn_silence(hu_turn_ctx_t *turn_ctx) {
    if (!turn_ctx || !turn_ctx->in.agent || !turn_ctx->in.msg || !turn_ctx->in.response_out)
        return hu_turn_step_return(HU_ERR_INVALID_ARGUMENT);
    const char *msg = turn_ctx->in.msg;
    size_t msg_len = turn_ctx->in.msg_len;
    hu_agent_t *agent = turn_ctx->in.agent;
    char **response_out = turn_ctx->in.response_out;
    size_t *response_len_out = turn_ctx->in.response_len_out;
    /* Silence intuition: decide if we should skip the LLM call entirely */
    {
        hu_emotional_weight_t ew = hu_emotional_weight_classify(msg, msg_len);
        /* Detect explicit questions AND imperative requests (help me, can you, etc.) */
        bool user_asked = (msg_len > 0 && memchr(msg, '?', msg_len) != NULL);
        if (!user_asked && msg && msg_len >= 4) {
            static const char *request_phrases[] = {
                "help",    "can you", "could you", "please", "how do", "what is",
                "show me", "tell me", "explain",   "write",  "create", "fix",
                "find",    "search",  "give me",   "build",  "make",   "do ",
            };
            for (size_t ri = 0; ri < sizeof(request_phrases) / sizeof(request_phrases[0]); ri++) {
                size_t rlen = strlen(request_phrases[ri]);
                if (msg_len >= rlen) {
                    for (size_t p = 0; p + rlen <= msg_len; p++) {
                        bool match = true;
                        for (size_t c = 0; c < rlen && match; c++) {
                            char lc = msg[p + c];
                            if (lc >= 'A' && lc <= 'Z')
                                lc += 32;
                            if (lc != request_phrases[ri][c])
                                match = false;
                        }
                        if (match) {
                            user_asked = true;
                            goto silence_check;
                        }
                    }
                }
            }
        }
    silence_check:;
        hu_silence_response_t silence =
            hu_silence_intuit(msg, msg_len, ew, (uint32_t)agent->history_count, user_asked);
        if (silence != HU_SILENCE_FULL_RESPONSE) {
            const char *silence_resp = NULL;
            size_t silence_resp_len = 0;
            if (silence == HU_SILENCE_ACTUAL_SILENCE) {
                silence_resp = "";
                silence_resp_len = 0;
            } else {
                char *ack =
                    hu_silence_build_acknowledgment(agent->alloc, silence, &silence_resp_len);
                silence_resp = ack;
            }
            if (silence_resp || silence == HU_SILENCE_ACTUAL_SILENCE) {
                *response_out = silence_resp
                                    ? hu_strndup(agent->alloc, silence_resp, silence_resp_len)
                                    : hu_strndup(agent->alloc, "", 0);
                if (response_len_out)
                    *response_len_out = silence_resp_len;
                /* Record this silent turn for learning (don't drop from training) */
#ifdef HU_ENABLE_SQLITE
                if (agent->memory) {
                    hu_experience_store_t sil_exp;
                    if (hu_agent_internal_experience_init(agent, &sil_exp) == HU_OK) {
                        sqlite3 *sil_db = hu_sqlite_memory_get_db(agent->memory);
                        if (sil_db)
                            sil_exp.db = sil_db;
                        (void)hu_experience_record(&sil_exp, msg, msg_len, "silence_intuit", 14,
                                                   silence_resp ? silence_resp : "",
                                                   silence_resp_len, 0.5);
                        hu_experience_store_deinit(&sil_exp);
                    }
                }
#endif
                /* Free silence acknowledgment if allocated */
                if (silence_resp && silence != HU_SILENCE_ACTUAL_SILENCE)
                    agent->alloc->free(agent->alloc->ctx, (void *)silence_resp,
                                       silence_resp_len + 1);
                return hu_turn_step_return(HU_OK);
            }
            /* If acknowledgment build failed, fall through to full response */
        }
    }

    return hu_turn_step_continue();
}
