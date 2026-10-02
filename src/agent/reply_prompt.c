/* Offline reply-prompt rendering. Contract: include/human/agent/reply_prompt.h. */
#include "human/agent/reply_prompt.h"
#include "human/agent.h"
#include "human/agent/hard_moment.h"
#include "human/agent/learned_style_turn.h"
#include "human/agent/length_policy.h"
#include "human/agent/prompt.h"
#include "human/agent/prompt_budget.h"
#include "human/context/conversation.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The daemon's 1:1 length decision (hu_length_policy_turn, not brief). */
static void reply_prompt_length(const hu_reply_prompt_request_t *req,
                                hu_length_turn_result_t *out) {
    const hu_contact_profile_t *cp =
        (req->persona && req->contact)
            ? hu_persona_find_contact(req->persona, req->contact, strlen(req->contact))
            : NULL;
    hu_length_turn_t t = {.inbound = req->incoming,
                          .inbound_len = req->incoming_len,
                          .contact = cp,
                          .stage = req->stage,
                          .channel_max = req->channel_max_chars};
    hu_length_policy_turn(&t, hu_length_policy_mode(), out);
}

uint32_t hu_reply_prompt_max_chars(const hu_reply_prompt_request_t *req) {
    if (!req)
        return 0;
    hu_length_turn_result_t r;
    reply_prompt_length(req, &r);
    return r.cap;
}

/* Conversation context the daemon builds from the inbound text alone:
 * length calibration first, then the honesty check, joined like daemon.c. */
static char *offline_conversation_context(hu_allocator_t *alloc,
                                          const hu_reply_prompt_request_t *req,
                                          const hu_contact_profile_t *cp,
                                          const hu_length_turn_result_t *lim, size_t *out_len) {
    *out_len = 0;
    char cal[1024];
    size_t cal_len = hu_conversation_calibrate_length_capped(
        req->incoming, req->incoming_len, false, cp, req->stage, lim->tight ? lim->cap : 0, cal,
        sizeof(cal));
    char *honesty = req->incoming
                        ? hu_conversation_honesty_check(alloc, req->incoming, req->incoming_len)
                        : NULL;
    size_t h_len = honesty ? strlen(honesty) : 0;
    size_t total = cal_len + (cal_len && h_len ? 1 : 0) + h_len;
    if (total == 0) {
        if (honesty)
            alloc->free(alloc->ctx, honesty, h_len + 1);
        return NULL;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (buf) {
        size_t o = 0;
        memcpy(buf, cal, cal_len);
        o += cal_len;
        if (cal_len && h_len)
            buf[o++] = '\n';
        if (h_len)
            memcpy(buf + o, honesty, h_len);
        buf[total] = '\0';
        *out_len = total;
    }
    if (honesty)
        alloc->free(alloc->ctx, honesty, h_len + 1);
    return buf;
}

hu_error_t hu_reply_prompt_render(hu_allocator_t *alloc, const hu_reply_prompt_request_t *req,
                                  char **out, size_t *out_len) {
    if (!alloc || !req || !req->persona || !req->channel || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = alloc;
    agent.persona = req->persona;
    agent.lean_prompt = true;
    agent.active_channel = req->channel;
    agent.active_channel_len = strlen(req->channel);
    if (req->contact) {
        agent.memory_session_id = req->contact;
        agent.memory_session_id_len = strlen(req->contact);
    }
    agent.relationship.stage = req->stage;

    char *head = NULL;
    size_t head_len = 0;
    /* Lean head + HU_LEARNED_STYLE, the same helper the turn paths use. */
    hu_learned_style_turn_t ls_turn;
    hu_error_t err = hu_agent_build_head_learned(&agent, true, NULL, 0, req->incoming,
                                                 req->incoming_len, &head, &head_len, &ls_turn);
    if (err != HU_OK)
        return err;
    hu_agent_apply_relationship_tone(&agent, &head, &head_len);
    if (head)
        (void)hu_hard_moment_apply(alloc, hu_hard_moment_mode(), req->incoming, req->incoming_len,
                                   &head, &head_len);

    const hu_contact_profile_t *cp =
        req->contact ? hu_persona_find_contact(req->persona, req->contact, strlen(req->contact))
                     : NULL;
    char *contact_ctx = NULL;
    size_t contact_ctx_len = 0;
    if (cp)
        (void)hu_contact_profile_build_context(alloc, cp, &contact_ctx, &contact_ctx_len);
    hu_length_turn_result_t lim;
    reply_prompt_length(req, &lim);
    size_t convo_len = 0;
    char *convo = offline_conversation_context(alloc, req, cp, &lim, &convo_len);
    hu_prompt_config_t cfg = {
        .persona_prompt = head,
        .persona_prompt_len = head_len,
        .persona_immersive = (head && head_len > 0),
        .persona = NULL, /* lean path, as agent_stream.c */
        .contact_context = contact_ctx,
        .contact_context_len = contact_ctx_len,
        .learned_style_live = ls_turn.live,
        .conversation_context = convo,
        .conversation_context_len = convo_len,
        .max_response_chars = lim.cap,
        .response_limit_tight = (uint8_t)lim.tight,
    };
    hu_prompt_field_stat_t stats[HU_PROMPT_FIELD_COUNT];
    memset(stats, 0, sizeof(stats));
    err = hu_prompt_build_system(alloc, &cfg, stats, NULL, out, out_len);
    if (err == HU_OK && *out)
        (void)hu_agent_finalize_system_prompt(&agent, out, out_len,
                                              stats[HU_PROMPT_FIELD_GUARD_TAIL].bytes_contributed);

    if (head)
        alloc->free(alloc->ctx, head, head_len + 1);
    if (contact_ctx)
        alloc->free(alloc->ctx, contact_ctx, contact_ctx_len + 1);
    if (convo)
        alloc->free(alloc->ctx, convo, convo_len + 1);
    return err;
}

/* ── CLI: human reply-prompt ─────────────────────────────────────────── */

static int parse_stage(const char *s, hu_relationship_stage_t *out) {
    static const struct {
        const char *name;
        hu_relationship_stage_t stage;
    } k[] = {{"new", HU_REL_NEW},
             {"familiar", HU_REL_FAMILIAR},
             {"trusted", HU_REL_TRUSTED},
             {"deep", HU_REL_DEEP}};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(s, k[i].name) == 0) {
            *out = k[i].stage;
            return 1;
        }
    }
    return 0;
}

hu_error_t cmd_reply_prompt(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    const char *persona_name = "seth";
    hu_reply_prompt_request_t req = {.channel = "imessage",
                                     .stage = HU_REL_TRUSTED,
                                     /* iMessage's response constraint
                                      * (imessage_get_response_constraints). */
                                     .channel_max_chars = 200};
    bool print_max = false;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--persona") == 0 && i + 1 < argc) {
            persona_name = argv[++i];
        } else if (strcmp(a, "--channel") == 0 && i + 1 < argc) {
            req.channel = argv[++i];
        } else if (strcmp(a, "--contact") == 0 && i + 1 < argc) {
            req.contact = argv[++i];
        } else if (strcmp(a, "--incoming") == 0 && i + 1 < argc) {
            req.incoming = argv[++i];
            req.incoming_len = strlen(req.incoming);
        } else if (strcmp(a, "--stage") == 0 && i + 1 < argc) {
            if (!parse_stage(argv[++i], &req.stage)) {
                fprintf(stderr, "reply-prompt: --stage must be new|familiar|trusted|deep\n");
                return HU_ERR_INVALID_ARGUMENT;
            }
        } else if (strcmp(a, "--max-chars") == 0 && i + 1 < argc) {
            req.channel_max_chars = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "--print-max-chars") == 0) {
            print_max = true;
        } else {
            fprintf(stderr,
                    "Usage: human reply-prompt --incoming <text> [--contact <handle>] "
                    "[--persona seth] [--channel imessage] [--stage trusted] [--max-chars 200] "
                    "[--print-max-chars]\n"
                    "Prints the system prompt the daemon would send for this 1:1 reply "
                    "(offline approximation: no memory/graph/trust/tapback context).\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
    }
    if (!req.incoming) {
        fprintf(stderr, "reply-prompt: --incoming is required\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    hu_error_t err = hu_persona_load(alloc, persona_name, strlen(persona_name), &persona);
    if (err != HU_OK) {
        fprintf(stderr, "reply-prompt: persona not found: %s\n", persona_name);
        return err;
    }
    req.persona = &persona;
    if (print_max) {
        fprintf(stdout, "%u\n", (unsigned)hu_reply_prompt_max_chars(&req));
    } else {
        char *out = NULL;
        size_t out_len = 0;
        err = hu_reply_prompt_render(alloc, &req, &out, &out_len);
        if (err == HU_OK && out) {
            fwrite(out, 1, out_len, stdout);
            alloc->free(alloc->ctx, out, out_len + 1);
        }
    }
    hu_persona_deinit(alloc, &persona);
    return err;
}
