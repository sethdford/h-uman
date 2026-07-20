/* src/agent/init_proposer.c
 *
 * Initiative Layer — T1 skeleton (governor-only, always SKIP).
 *
 * See docs/plans/2026-05-25-initiative-layer/{requirements,design,tasks}.md.
 * This file implements AC-1 (scheduler ticks), AC-6 (loud failure on silent
 * gating), and AC-7 (reversible kill switch). AC-2 (context bundle), AC-3
 * (governor with confidence), AC-4 (SKIP-default fast path), and AC-5
 * (delivery via existing channels) land in T2/T3/T4.
 */

#include "human/agent/init_proposer.h"
#include "human/agent.h"
#include "human/agent/governor.h"
#include "human/agent/response_guard.h"
#include "human/agent/response_guard_dpo.h"
#include "human/autoresponder.h"
#include "human/config.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/memory.h"
#include "human/provider.h"
#include "human/reflection.h" /* T8: pull unsurfaced patterns into bundle */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per ~/.claude/rules/silent-config-gated-subsystems.md: emit ONE
 * operator-visible log line per process when the subsystem is disabled or
 * enabled. Guards are process-scoped via atomic_bool. */
static atomic_bool g_warned_disabled = false;
static atomic_bool g_warned_enabled = false;
/* Guard for DND-gate diagnostic: alert when no config but gate fires */
static atomic_bool g_warned_no_dnd_config = false;

void hu_init_proposer_reset_warn_guards_for_test(void) {
#if HU_IS_TEST
    atomic_store(&g_warned_disabled, false);
    atomic_store(&g_warned_enabled, false);
    atomic_store(&g_warned_no_dnd_config, false);
#endif
}

hu_init_proposer_result_t
hu_init_proposer_governor_check_only(const struct hu_initiative_config *cfg,
                                     const struct hu_autoresponder_config *ar_cfg,
                                     int32_t tz_offset_seconds, struct hu_proactive_budget *budget,
                                     int64_t last_inbound_unix, int64_t now_unix) {
    /* Quiet hours (NULL ar_cfg = operator opted out, NO configured schedule = no DND).
     * Per silent-config-gated-subsystems.md: alert once when ar_cfg exists with
     * schedule_count=0 but somehow DND gate fires (diagnostic signal of a logic bug). */
    if (ar_cfg && hu_autoresponder_in_dnd_window(ar_cfg, now_unix, tz_offset_seconds)) {
        /* Sanity check: log once if we gate QUIET despite no configured DND.
         * Normal path: ar_cfg with schedule_count=0 returns false, so this
         * block doesn't execute. If it does, the DND check has a bug. */
        if (ar_cfg->schedule_count == 0) {
            hu_log_info_once(&g_warned_no_dnd_config, "init_proposer", NULL,
                             "DIAGNOSTIC: GATED_QUIET fired despite schedule_count=0 — "
                             "this indicates a bug in hu_autoresponder_in_dnd_window; "
                             "proactive outreach is gated despite no DND config. "
                             "Create ~/.human/autoresponder.json with "
                             "\"schedules\":[{\"start\":\"22:00\",\"end\":\"08:00\"},...] "
                             "to configure quiet hours explicitly.");
        }
        return HU_INIT_RESULT_GATED_QUIET;
    }

    /* Daily proactive budget (NULL budget = operator opted out). */
    if (budget && !hu_governor_has_budget(budget, (uint64_t)now_unix * 1000ULL))
        return HU_INIT_RESULT_GATED_BUDGET;

    /* Per-contact recency. NULL cfg means "defaults" (600s floor). */
    int recency_floor =
        (cfg && cfg->per_contact_min_seconds > 0) ? cfg->per_contact_min_seconds : 600;
    if (last_inbound_unix > 0 && now_unix - last_inbound_unix < recency_floor)
        return HU_INIT_RESULT_GATED_RECENCY;

    return HU_INIT_RESULT_SKIP;
}

hu_error_t hu_init_proposer_tick(const struct hu_initiative_config *cfg,
                                 const struct hu_autoresponder_config *ar_cfg,
                                 int32_t tz_offset_seconds, struct hu_proactive_budget *budget,
                                 int64_t last_inbound_unix, int64_t now_unix,
                                 int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
                                 hu_init_proposer_result_t *out_result) {
    if (!cfg || !last_tick_unix_inout || !tick_id_inout || !out_result)
        return HU_ERR_INVALID_ARGUMENT;

    /* AC-7: reversible kill switch. */
    if (!cfg->enabled) {
        hu_log_info_once(&g_warned_disabled, "init_proposer", NULL,
                         "initiative subsystem disabled by config "
                         "(cfg->initiative.enabled=false); set initiative.enabled=true "
                         "in config.json to activate");
        *out_result = HU_INIT_RESULT_SKIP;
        return HU_OK;
    }

    /* AC-6: announce activation exactly once so operators can see it's alive. */
    hu_log_info_once(&g_warned_enabled, "init_proposer", NULL,
                     "initiative subsystem activated by config "
                     "(cfg->initiative.enabled=true; tick_interval_sec=%d, threshold=%.2f, "
                     "model=%s)",
                     cfg->tick_interval_sec > 0 ? cfg->tick_interval_sec : 1800,
                     cfg->confidence_threshold > 0.0 ? cfg->confidence_threshold : 0.85,
                     (cfg->propose_model && cfg->propose_model[0]) ? cfg->propose_model
                                                                   : "gemini-3.5-flash");

    /* Interval gate (cheap — runs every outer loop). */
    int interval = cfg->tick_interval_sec > 0 ? cfg->tick_interval_sec : 1800;
    if (*last_tick_unix_inout > 0 && now_unix - *last_tick_unix_inout < interval) {
        *out_result = HU_INIT_RESULT_GATED_INTERVAL;
        return HU_OK;
    }

    /* From here on, this is a real "tick" — bump the id so the log line carries
     * a stable handle and so SKIP rate can be computed from log telemetry. */
    (*tick_id_inout)++;
    uint64_t tid = *tick_id_inout;

    /* AC-1/AC-3 governor gates — delegated to the shared arbiter so
     * daemon_proactive, follow-up watcher, and scheduled cron all
     * consult the same gate stack. */
    hu_init_proposer_result_t gov_result = hu_init_proposer_governor_check_only(
        cfg, ar_cfg, tz_offset_seconds, budget, last_inbound_unix, now_unix);
    if (gov_result != HU_INIT_RESULT_SKIP) {
        const char *reason = (gov_result == HU_INIT_RESULT_GATED_QUIET)    ? "GATED_QUIET"
                             : (gov_result == HU_INIT_RESULT_GATED_BUDGET) ? "GATED_BUDGET"
                                                                           : "GATED_RECENCY";
        if (gov_result == HU_INIT_RESULT_GATED_RECENCY) {
            hu_log_info("init_proposer", NULL,
                        "tick id=%llu phase=governor result=%s (last_inbound=%llds_ago)",
                        (unsigned long long)tid, reason, (long long)(now_unix - last_inbound_unix));
        } else {
            hu_log_info("init_proposer", NULL, "tick id=%llu phase=governor result=%s",
                        (unsigned long long)tid, reason);
        }
        *last_tick_unix_inout = now_unix;
        *out_result = gov_result;
        return HU_OK;
    }

    /* Governor passed → SKIP means "no gating fired" for the wrapper to
     * promote to a real T3+ LLM call. We DON'T log here because the
     * wrapper (hu_init_proposer_tick_with_provider) logs the actual
     * outcome (FIRED / NEGATIVE / LOW_CONFIDENCE / PARSE_ERROR) after
     * the LLM round-trip. Until 2026-05 this site logged "T1 stub; LLM
     * call lands in T3" — historically accurate when T3 didn't exist,
     * but misleading now that the wrapper always promotes past this
     * point. The unused `tid` is kept in the signature for caller
     * compatibility. */
    (void)tid;
    *last_tick_unix_inout = now_unix;
    *out_result = HU_INIT_RESULT_SKIP;
    return HU_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * T2 — Context bundle assembly + summary formatting.
 *
 * The bundle is a thin observation view over the agent's cached per-turn
 * context strings (memory, conversation, contact, etc.). It owns nothing —
 * pointers are tied to agent lifetime. The companion format function is a
 * pure predicate so the per-tick log line can be unit-tested without
 * spinning a real agent. */

/* Stable display names matching hu_init_field_t indices. Used by both
 * assemble + format, kept here so a single source of truth controls the
 * log-line schema. */
static const char *const s_field_names[HU_INIT_FIELD_COUNT] = {
    [HU_INIT_FIELD_PERSONA] = "persona",
    [HU_INIT_FIELD_CONTACT] = "contact",
    [HU_INIT_FIELD_CONVERSATION] = "conversation",
    [HU_INIT_FIELD_MEMORY] = "memory",
    [HU_INIT_FIELD_PERSONAL_MODEL] = "personal_model",
    [HU_INIT_FIELD_AWARENESS] = "awareness",
    [HU_INIT_FIELD_INSTRUCTION] = "instruction",
    [HU_INIT_FIELD_STM] = "stm",
    [HU_INIT_FIELD_REFLECTION] = "reflection",
};

hu_error_t hu_init_proposer_assemble_context(const struct hu_agent *agent, int64_t now_unix,
                                             int64_t last_inbound_unix,
                                             hu_init_context_bundle_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->now_unix = now_unix;
    out->last_inbound_unix = last_inbound_unix;

    if (!agent)
        return HU_OK; /* empty bundle is valid — caller (T3) decides SKIP. */

    /* Per agent.h: these cached strings are set by the daemon before
     * hu_agent_turn; lifetime is tied to the agent. We borrow pointers. */
    out->content[HU_INIT_FIELD_CONTACT] = agent->contact_context;
    out->bytes[HU_INIT_FIELD_CONTACT] = agent->contact_context_len;
    out->content[HU_INIT_FIELD_CONVERSATION] = agent->conversation_context;
    out->bytes[HU_INIT_FIELD_CONVERSATION] = agent->conversation_context_len;
    out->content[HU_INIT_FIELD_INSTRUCTION] = agent->custom_instructions;
    out->bytes[HU_INIT_FIELD_INSTRUCTION] = agent->custom_instructions_len;

    /* PERSONA (2026-06-06 un-stub) — a compact "name + identity" descriptor.
     * Previously this field stayed zero, so combined with the empty
     * memory/awareness/stm slots the proposer's user message was nearly
     * empty; the silence-biased system prompt ("if context is thin, propose
     * nothing") then returned should_propose=false with confidence 0.000 on
     * every tick (317 NEGATIVE proposals observed in production). Grounding
     * the model in who it is + who it's addressing is the cheapest real
     * signal and unblocks a non-trivial decision. */
    if (agent->persona && agent->persona->name && agent->persona->name_len > 0) {
        const char *ident = agent->persona->identity      ? agent->persona->identity
                            : agent->persona->core_anchor ? agent->persona->core_anchor
                                                          : "";
        int pn = snprintf(out->persona_buf, sizeof(out->persona_buf), "%.*s%s%s",
                          (int)agent->persona->name_len, agent->persona->name,
                          ident[0] ? " — " : "", ident);
        if (pn > 0) {
            size_t plen =
                (size_t)pn < sizeof(out->persona_buf) ? (size_t)pn : sizeof(out->persona_buf) - 1;
            out->content[HU_INIT_FIELD_PERSONA] = out->persona_buf;
            out->bytes[HU_INIT_FIELD_PERSONA] = plen;
        }
    }

    /* T2 stub (remaining): memory/personal_model/awareness/stm fields land
     * when their extractor outputs are wired into a stable agent-cached
     * location. The send path (hu_init_proposer_tick_with_provider_ex)
     * already receives memory_context directly; those slots staying zero
     * here is meaningful telemetry (the proposer can see what's unwired). */

    /* T8 of docs/plans/2026-05-26-reflection-loop: pull unsurfaced
     * reflection patterns into the inline reflection_buf when the agent
     * has a SQLite memory backend. The query is cheap (single indexed
     * scan capped at 8 rows) so we do it unconditionally — gating is
     * the daemon's responsibility, not the proposer's. */
#ifdef HU_ENABLE_SQLITE
    if (agent->memory) {
        struct sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
        if (db) {
            hu_reflection_pattern_t *patterns = NULL;
            int n = 0;
            if (hu_reflection_query_unsurfaced(db, /*min_confidence=*/0.6, &patterns, &n) ==
                    HU_OK &&
                n > 0 && patterns) {
                /* Internal cap so we don't blow the inline buffer or pull
                 * too many candidates into one proposer tick. */
                if (n > 8)
                    n = 8;
                size_t pos = 0;
                for (int i = 0; i < n && pos + 1 < sizeof out->reflection_buf; i++) {
                    int w = snprintf(out->reflection_buf + pos, sizeof out->reflection_buf - pos,
                                     "- %s (id=%s, confidence %.2f)\n", patterns[i].observation,
                                     patterns[i].id, patterns[i].confidence);
                    if (w <= 0)
                        break;
                    if ((size_t)w >= sizeof out->reflection_buf - pos) {
                        /* Truncated mid-row — leave buffer NUL-terminated
                         * by snprintf and stop appending. */
                        pos = sizeof out->reflection_buf - 1;
                        break;
                    }
                    pos += (size_t)w;
                }
                if (pos > 0) {
                    out->content[HU_INIT_FIELD_REFLECTION] = out->reflection_buf;
                    out->bytes[HU_INIT_FIELD_REFLECTION] = pos;
                }
            }
            free(patterns); /* free(NULL) is fine */
        }
    }
#endif

    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT; i++) {
        out->total_bytes += out->bytes[i];
    }
    return HU_OK;
}

size_t hu_init_proposer_format_context_summary(const hu_init_context_bundle_t *bundle, char *out,
                                               size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!bundle)
        return 0;

    /* Count populated fields (>0 bytes) for the "fields=N" leader. */
    size_t populated = 0;
    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT; i++) {
        if (bundle->bytes[i] > 0)
            populated++;
    }

    int written = snprintf(out, out_cap, "fields=%zu total=%zu", populated, bundle->total_bytes);
    if (written < 0)
        return 0;
    size_t pos = (size_t)written < out_cap ? (size_t)written : out_cap - 1;

    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT && pos + 1 < out_cap; i++) {
        int n = snprintf(out + pos, out_cap - pos, " %s=%zu", s_field_names[i], bundle->bytes[i]);
        if (n < 0)
            break;
        if ((size_t)n >= out_cap - pos) {
            pos = out_cap - 1;
            break;
        }
        pos += (size_t)n;
    }
    out[pos] = '\0';
    return pos;
}

/* ──────────────────────────────────────────────────────────────────────────
 * T3 — Prompt building, response parsing, decision evaluation.
 *
 * Three pure predicates. The integration glue (tick_with_provider) calls
 * provider->vtable->chat_with_system between predicates 1 and 2. Tests
 * exercise each predicate directly. */

/* The system prompt is small and static. We embed it as a literal so we
 * don't have to manage a config string slot for it. T6 (post-tuning) can
 * move it to config if Seth wants to A/B test alternate prompts. */
static const char *const s_system_prompt =
    "You are the Initiative Layer of h-uman, a private AI assistant that runs "
    "on Seth's hardware. Your job is to decide whether h-uman should proactively "
    "send Seth a message right now — even though he didn't ask. You bias HEAVILY "
    "toward silence: a wrong proposal during a meeting is far worse than a right "
    "proposal that never fires.\n"
    "\n"
    "Consider only the context provided in the user message below. Do NOT invent "
    "facts. If the context is empty or thin, propose nothing.\n"
    "\n"
    "Concrete triggers DO warrant a proposal: an established contact, a stored "
    "commitment or temporal event with a due time, a notable gap since last contact "
    "with real shared context, reasonable hours. A warm check-in after a significant "
    "silence with grounded context is acceptable. Focus on trigger-based outreach "
    "(commitments due, stored events arriving) over generic pondering.\n"
    "\n"
    "Return ONLY a single JSON object on a single line — no prose, no preamble, "
    "no markdown code fences (no triple-backticks, no ```json wrapper). The "
    "very first character of your output MUST be `{` and the last must be `}`. "
    "Shape:\n"
    "{\n"
    "  \"should_propose\": <true|false>,\n"
    "  \"confidence\": <0.0..1.0>,\n"
    "  \"draft\": \"<text to send to Seth — only when should_propose=true>\",\n"
    "  \"reason\": \"<one short sentence why — when should_propose=false>\"\n"
    "}\n"
    "\n"
    "Score confidence honestly on a 0.0-1.0 scale: how certain are you that "
    "this outreach would be welcome and appropriate? The configured confidence "
    "threshold (typically 0.85) gates the send; your job is to provide an honest "
    "confidence score, not to second-guess the threshold.";

size_t hu_init_proposer_build_propose_prompt(const hu_init_context_bundle_t *bundle,
                                             char *out_system_prompt, size_t system_prompt_cap,
                                             char *out_user_message, size_t user_message_cap) {
    if (!out_system_prompt || !out_user_message || system_prompt_cap == 0 || user_message_cap == 0)
        return 0;
    out_system_prompt[0] = '\0';
    out_user_message[0] = '\0';
    if (!bundle)
        return 0;

    /* Copy the static system prompt (truncating if cap is tiny). */
    size_t sys_len = strlen(s_system_prompt);
    size_t sys_copy = sys_len < system_prompt_cap - 1 ? sys_len : system_prompt_cap - 1;
    memcpy(out_system_prompt, s_system_prompt, sys_copy);
    out_system_prompt[sys_copy] = '\0';

    /* Build the user message — bundle fields in a stable order, then a
     * one-line tail asking the question. Each field gets a labeled header
     * so the LLM can see WHICH source contributed what. */
    int written =
        snprintf(out_user_message, user_message_cap, "Context as of unix=%lld; last_inbound=%lld\n",
                 (long long)bundle->now_unix, (long long)bundle->last_inbound_unix);
    if (written < 0)
        return 0;
    size_t pos = (size_t)written < user_message_cap ? (size_t)written : user_message_cap - 1;

    for (size_t i = 0; i < (size_t)HU_INIT_FIELD_COUNT && pos + 1 < user_message_cap; i++) {
        const char *body = bundle->content[i];
        size_t body_len = bundle->bytes[i];
        if (!body || body_len == 0)
            continue; /* skip empty fields — saves tokens, signals "unwired" */

        int n = snprintf(out_user_message + pos, user_message_cap - pos, "\n--- %s ---\n",
                         s_field_names[i]);
        if (n < 0)
            break;
        if ((size_t)n >= user_message_cap - pos) {
            pos = user_message_cap - 1;
            break;
        }
        pos += (size_t)n;

        size_t avail = user_message_cap - pos - 1; /* leave space for NUL */
        size_t copy = body_len < avail ? body_len : avail;
        memcpy(out_user_message + pos, body, copy);
        pos += copy;
    }

    /* Final question line. Append only if there's room — otherwise the
     * model still gets the system-prompt instructions, which include the
     * decision contract. */
    if (pos + 1 < user_message_cap) {
        int n = snprintf(out_user_message + pos, user_message_cap - pos,
                         "\n\nShould h-uman send Seth a message right now?");
        if (n > 0 && (size_t)n < user_message_cap - pos)
            pos += (size_t)n;
    }
    out_user_message[pos] = '\0';
    return pos;
}

/* Locate the first balanced top-level {...} substring in `text`. Returns
 * 0/0 if none is found. Bracket-counting is naive (does not understand
 * JSON string-escaped braces) but adequate for well-formed model outputs. */
static void find_first_json_object(const char *text, size_t len, size_t *out_start,
                                   size_t *out_end) {
    *out_start = 0;
    *out_end = 0;
    if (!text || len == 0)
        return;

    /* Strip a leading markdown code fence if present — gemini-3.1-flash-lite
     * often wraps the JSON in ```json … ``` despite the prompt asking for
     * raw output. We compute a `text_offset` into the ORIGINAL buffer so
     * out_start / out_end stay caller-relative (the caller does
     * `response + js_start` against the unmodified pointer). The brace-
     * matcher below still requires a complete object inside, so a
     * truncated fenced payload still fails cleanly. */
    size_t text_offset = 0;
    if (len >= 3) {
        for (size_t i = 0; i + 2 < len; i++) {
            if (text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`') {
                size_t after = i + 3;
                while (after < len && ((text[after] >= 'a' && text[after] <= 'z') ||
                                       (text[after] >= 'A' && text[after] <= 'Z'))) {
                    after++;
                }
                while (after < len && (text[after] == ' ' || text[after] == '\n' ||
                                       text[after] == '\r' || text[after] == '\t')) {
                    after++;
                }
                text_offset = after;
                break;
            }
        }
    }
    const char *scan = text + text_offset;
    size_t scan_len = len - text_offset;

    size_t start = 0;
    bool found_start = false;
    for (size_t i = 0; i < scan_len; i++) {
        if (scan[i] == '{') {
            start = i;
            found_start = true;
            break;
        }
    }
    if (!found_start)
        return;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (size_t i = start; i < scan_len; i++) {
        char c = scan[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                *out_start = text_offset + start;
                *out_end = text_offset + i + 1;
                return;
            }
        }
    }
}

/* Call the LLM via the structured chat() vtable so we can set the three
 * controls chat_with_system() hides:
 *
 *   max_tokens = 512        — enough for the compact JSON decision
 *                             {"should_propose":bool,"confidence":num,
 *                              "draft":"...","reason":"..."} comfortably.
 *
 *   thinking_budget = 0     — this is a DETERMINISTIC binary classifier,
 *                             not a reasoning task. Gemini 3.x defaults
 *                             thinking ON with a large invisible budget
 *                             that comes out of max_tokens. Production
 *                             logs (2026-05-26) showed `err=42` (JSON
 *                             parse fail) on responses truncated to
 *                             ~57 chars at `"draft": "",` — thinking
 *                             ate the budget. CLAUDE.md "Gemini 3.x
 *                             thinking-token budget gotcha" documents
 *                             this exact failure mode.
 *
 *   response_format = json   — providers that honor JSON mode force the
 *                             output shape. Gemini-3.x respects it.
 *
 * On success, caller takes ownership of *out_response (heap, alloc) and
 * MUST free via alloc->free(*out_response, *out_response_len + 1).
 *
 * On failure, *out_response is NULL.
 */
/* Both call sites live in the #else (non-test) branches of the proposer
 * tick/run functions, so in HU_IS_TEST (human_tests) builds this helper
 * is unused and would trip -Wunused-function under -Werror. Guard the
 * definition with the same condition as its callers. */
#if !HU_IS_TEST
static hu_error_t init_proposer_call_llm(hu_allocator_t *alloc, struct hu_provider *provider,
                                         const char *sys_prompt, const char *user_msg,
                                         const char *model, char **out_response,
                                         size_t *out_response_len) {
    *out_response = NULL;
    *out_response_len = 0;

    /* Prefer the structured chat() vtable. Some test mocks only set
     * chat_with_system(); fall back so we don't break those callers. */
    if (provider->vtable && provider->vtable->chat) {
        hu_chat_message_t msgs[2];
        memset(msgs, 0, sizeof(msgs));
        msgs[0].role = HU_ROLE_SYSTEM;
        msgs[0].content = sys_prompt;
        msgs[0].content_len = strlen(sys_prompt);
        msgs[1].role = HU_ROLE_USER;
        msgs[1].content = user_msg;
        msgs[1].content_len = strlen(user_msg);

        hu_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.messages = msgs;
        req.messages_count = 2;
        req.model = model;
        req.model_len = strlen(model);
        req.temperature = 0.2;
        req.max_tokens = 512;    /* compact JSON; not a long-form generation */
        req.thinking_budget = 0; /* deterministic classifier — no thinking */
        req.response_format = "json_object";
        req.response_format_len = strlen("json_object");

        hu_chat_response_t resp;
        memset(&resp, 0, sizeof(resp));
        hu_error_t err =
            provider->vtable->chat(provider->ctx, alloc, &req, model, strlen(model), 0.2, &resp);
        if (err == HU_OK && resp.content && resp.content_len > 0) {
            char *buf = (char *)alloc->alloc(alloc->ctx, resp.content_len + 1);
            if (buf) {
                memcpy(buf, resp.content, resp.content_len);
                buf[resp.content_len] = '\0';
                *out_response = buf;
                *out_response_len = resp.content_len;
            } else {
                err = HU_ERR_OUT_OF_MEMORY;
            }
        }
        hu_chat_response_free(alloc, &resp);
        return err;
    }

    /* Fallback path for providers/mocks without structured chat(). The
     * truncation risk above DOES apply here, but it's the only path
     * available — better to attempt than to hard-fail. */
    if (!provider->vtable || !provider->vtable->chat_with_system)
        return HU_ERR_NOT_SUPPORTED;
    return provider->vtable->chat_with_system(provider->ctx, alloc, sys_prompt, strlen(sys_prompt),
                                              user_msg, strlen(user_msg), model, strlen(model), 0.2,
                                              out_response, out_response_len);
}
#endif /* !HU_IS_TEST */

/* 2026-05-26 issue-sweep — defense-in-depth fallback for truncated
 * responses. Even with gemini-3.5-flash + json_object mode, the model
 * occasionally returns a partial response that's missing the closing
 * `}`. find_first_json_object correctly fails, but if we can SEE the
 * model's intent in the partial text, we should honor it rather than
 * silently parsing-error. Today we only recognize `"should_propose":
 * false` since the safe-default is SKIP — a partial `true` with no
 * confidence + draft would FAIL the FIRED gate anyway, so there's no
 * value in trying to extract `true` from a truncated stream.
 *
 * Returns true iff a partial-but-confident SKIP decision was extracted. */
static bool try_partial_skip_parse(const char *response, size_t response_len,
                                   hu_init_decision_t *out) {
    if (!response || response_len < 20)
        return false;
    /* Look for `"should_propose":\s*false` allowing arbitrary whitespace
     * after the colon. Substring search is fine — the field name is
     * sufficiently unique that false-positives are vanishingly rare in
     * an LLM-generated response. */
    const char *key = "\"should_propose\"";
    size_t klen = strlen(key);
    if (klen > response_len)
        return false;
    for (size_t i = 0; i + klen < response_len; i++) {
        if (memcmp(response + i, key, klen) != 0)
            continue;
        size_t p = i + klen;
        /* Skip `:` and any whitespace. */
        while (p < response_len && (response[p] == ':' || response[p] == ' ' ||
                                    response[p] == '\t' || response[p] == '\n'))
            p++;
        if (p + 5 <= response_len && memcmp(response + p, "false", 5) == 0) {
            memset(out, 0, sizeof(*out));
            out->should_propose = false;
            out->confidence = 0.0;
            snprintf(out->skip_reason, sizeof(out->skip_reason),
                     "(partial-parse) model returned should_propose=false in "
                     "truncated response");
            out->skip_reason_len = strlen(out->skip_reason);
            return true;
        }
        /* Found the key but it's `true` or other — let main parser deal. */
        return false;
    }
    return false;
}

hu_error_t hu_init_proposer_parse_response(const char *response, size_t response_len,
                                           hu_init_decision_t *out) {
    if (!response || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    size_t js_start = 0, js_end = 0;
    find_first_json_object(response, response_len, &js_start, &js_end);
    if (js_end == 0 || js_end <= js_start) {
        /* Main parse failed (no complete JSON object). Try the
         * partial-skip fallback: if the model clearly said "no propose"
         * in a truncated response, honor that as a SKIP rather than
         * surfacing a parse error. Reduces operator log noise from the
         * gemini-3.5-flash truncation cases observed 2026-05-26. */
        if (try_partial_skip_parse(response, response_len, out))
            return HU_OK;
        return HU_ERR_JSON_PARSE;
    }

    /* Use a temporary system allocator for the parse — the JSON tree is
     * freed before return; only the decision struct survives. */
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, response + js_start, js_end - js_start, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(&alloc, root);
        return HU_ERR_JSON_PARSE;
    }

    out->should_propose = hu_json_get_bool(root, "should_propose", false);
    double conf = hu_json_get_number(root, "confidence", 0.0);
    if (conf < 0.0)
        conf = 0.0;
    if (conf > 1.0)
        conf = 1.0;
    out->confidence = conf;

    const char *draft = hu_json_get_string(root, "draft");
    if (draft && draft[0]) {
        size_t dlen = strlen(draft);
        size_t cap = sizeof(out->draft) - 1;
        size_t copy = dlen < cap ? dlen : cap;
        memcpy(out->draft, draft, copy);
        out->draft[copy] = '\0';
        out->draft_len = copy;
    }

    const char *reason = hu_json_get_string(root, "reason");
    if (reason && reason[0]) {
        size_t rlen = strlen(reason);
        size_t cap = sizeof(out->skip_reason) - 1;
        size_t copy = rlen < cap ? rlen : cap;
        memcpy(out->skip_reason, reason, copy);
        out->skip_reason[copy] = '\0';
        out->skip_reason_len = copy;
    }

    hu_json_free(&alloc, root);
    return HU_OK;
}

hu_init_proposer_result_t hu_init_proposer_evaluate_decision(const hu_init_decision_t *decision,
                                                             double confidence_threshold) {
    if (!decision)
        return HU_INIT_RESULT_LLM_ERROR;
    if (!decision->should_propose)
        return HU_INIT_RESULT_NEGATIVE;
    /* When should_propose=true, gate on confidence AND non-empty draft.
     * A "propose" decision with no draft is malformed → low confidence. */
    if (decision->confidence < confidence_threshold || decision->draft_len == 0)
        return HU_INIT_RESULT_LOW_CONFIDENCE;
    return HU_INIT_RESULT_FIRED;
}

hu_error_t hu_init_proposer_tick_with_provider(
    const struct hu_initiative_config *cfg, const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds, struct hu_proactive_budget *budget, const struct hu_agent *agent,
    struct hu_provider *provider, hu_allocator_t *alloc, int64_t last_inbound_unix,
    int64_t now_unix, int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
    hu_init_proposer_result_t *out_result, hu_init_decision_t *out_decision) {
    /* Run the T1/T2 governor first. If gated, return early — never spends
     * an LLM token unless the cheap gates passed. */
    hu_init_proposer_result_t gov_result = HU_INIT_RESULT_SKIP;
    hu_error_t gov_err =
        hu_init_proposer_tick(cfg, ar_cfg, tz_offset_seconds, budget, last_inbound_unix, now_unix,
                              last_tick_unix_inout, tick_id_inout, &gov_result);
    if (gov_err != HU_OK)
        return gov_err;
    if (gov_result != HU_INIT_RESULT_SKIP) {
        /* Either a governor GATE_ or the T1-stub SKIP-after-gov returned;
         * either way, no LLM call is warranted this tick. */
        if (out_result)
            *out_result = gov_result;
        return HU_OK;
    }

    /* T1 returned SKIP after passing all governor gates. Promote to a
     * full T3 LLM call when provider + alloc are both present. */
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system || !alloc) {
        if (out_result)
            *out_result = HU_INIT_RESULT_SKIP;
        return HU_OK;
    }

    /* Assemble context bundle (T2). */
    hu_init_context_bundle_t bundle;
    hu_error_t ace = hu_init_proposer_assemble_context(agent, now_unix, last_inbound_unix, &bundle);
    if (ace != HU_OK) {
        if (out_result)
            *out_result = HU_INIT_RESULT_SKIP;
        return HU_OK;
    }

    /* Build prompt (T3 pure). */
    static char sys_prompt[1536];
    static char user_msg[16384];
    hu_init_proposer_build_propose_prompt(&bundle, sys_prompt, sizeof(sys_prompt), user_msg,
                                          sizeof(user_msg));

#if HU_IS_TEST
    /* Test builds: never make a real network call. Return SKIP so unit
     * tests of the integration path can exercise the wiring without
     * needing a mock provider. The pure predicates are tested directly. */
    (void)tz_offset_seconds;
    if (out_result)
        *out_result = HU_INIT_RESULT_SKIP;
    if (out_decision)
        memset(out_decision, 0, sizeof(*out_decision));
    return HU_OK;
#else
    /* Production: structured chat() with max_tokens + thinking_budget=0
     * + response_format=json. See init_proposer_call_llm helper for the
     * full rationale on each request field. */
    const char *model =
        (cfg->propose_model && cfg->propose_model[0]) ? cfg->propose_model : "gemini-3.5-flash";
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t lerr = init_proposer_call_llm(alloc, provider, sys_prompt, user_msg, model,
                                             &response, &response_len);
    if (lerr != HU_OK || !response || response_len == 0) {
        hu_log_warn("init_proposer", NULL, "LLM call failed: err=%d (response_len=%zu)", (int)lerr,
                    response_len);
        if (response)
            alloc->free(alloc->ctx, response, response_len + 1);
        if (out_result)
            *out_result = HU_INIT_RESULT_LLM_ERROR;
        return HU_OK;
    }

    hu_init_decision_t decision;
    hu_error_t perr = hu_init_proposer_parse_response(response, response_len, &decision);
    if (perr != HU_OK) {
        /* T3 diagnostic: preview the first 200 chars of the failed
         * response so we can tell whether the model returned plain text
         * (no '{' at all) vs malformed JSON vs JSON-with-wrong-schema.
         * The preview is sanitized — newlines → spaces, NULs → '.' —
         * so a single log line is grep-friendly. */
        char preview[201];
        size_t copy = response_len < sizeof(preview) - 1 ? response_len : sizeof(preview) - 1;
        for (size_t i = 0; i < copy; i++) {
            unsigned char c = (unsigned char)response[i];
            preview[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : (c == 0 ? '.' : (char)c);
        }
        preview[copy] = '\0';
        hu_log_warn("init_proposer", NULL,
                    "response parse failed: err=%d response_len=%zu preview=%.*s%s", (int)perr,
                    response_len, (int)copy, preview, response_len > copy ? "..." : "");
        alloc->free(alloc->ctx, response, response_len + 1);
        if (out_result)
            *out_result = HU_INIT_RESULT_PARSE_ERROR;
        return HU_OK;
    }
    alloc->free(alloc->ctx, response, response_len + 1);

    double threshold = cfg->confidence_threshold > 0.0 ? cfg->confidence_threshold : 0.85;
    hu_init_proposer_result_t verdict = hu_init_proposer_evaluate_decision(&decision, threshold);

    hu_log_info("init_proposer", NULL,
                "LLM verdict: should_propose=%d confidence=%.3f draft_len=%zu result=%d",
                decision.should_propose ? 1 : 0, decision.confidence, decision.draft_len,
                (int)verdict);

    if (out_result)
        *out_result = verdict;
    if (out_decision && verdict == HU_INIT_RESULT_FIRED)
        memcpy(out_decision, &decision, sizeof(decision));
    return HU_OK;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * M3 Dispatch Unification — T1 (2026-05-26)
 *
 * Pure-addition wrapper that lets daemon_proactive's scheduler pass rich
 * per-contact context THROUGH init_proposer so the same propose-or-skip
 * machinery composes both initiative-driven AND daemon-proactive-driven
 * sends. See docs/plans/2026-05-26-m3-dispatch-unification/.
 *
 * T1 is the smallest useful step: new struct + new function. No existing
 * caller is forced to migrate; T2-T8 (separate sprint tasks) wire callers
 * over and eventually delete the legacy path. */

size_t hu_init_proposer_build_propose_user_message_ex(const hu_proactive_compose_inputs_t *inputs,
                                                      int64_t now_unix, int64_t last_inbound_unix,
                                                      char *out, size_t out_cap) {
    if (!out || out_cap == 0)
        return 0;
    out[0] = '\0';
    if (!inputs)
        return 0;

    /* Header — same shape as hu_init_proposer_build_propose_prompt so the
     * LLM sees a consistent prompt schema across initiative and proactive
     * paths. */
    int written = snprintf(out, out_cap, "Context as of unix=%lld; last_inbound=%lld\n",
                           (long long)now_unix, (long long)last_inbound_unix);
    if (written < 0)
        return 0;
    size_t pos = (size_t)written < out_cap ? (size_t)written : out_cap - 1;

    /* Identity block — channel + contact go first so the model knows
     * WHO is being addressed before it sees the content fragments.
     * Skipped cleanly when either is empty. */
    if (inputs->channel_name && inputs->channel_name_len > 0 && pos + 1 < out_cap) {
        int n = snprintf(out + pos, out_cap - pos, "\n--- channel ---\n%.*s",
                         (int)inputs->channel_name_len, inputs->channel_name);
        if (n > 0 && (size_t)n < out_cap - pos)
            pos += (size_t)n;
    }
    if (inputs->contact_id && inputs->contact_id_len > 0 && pos + 1 < out_cap) {
        int n = snprintf(out + pos, out_cap - pos, "\n--- contact ---\n%.*s",
                         (int)inputs->contact_id_len, inputs->contact_id);
        if (n > 0 && (size_t)n < out_cap - pos)
            pos += (size_t)n;
    }

    /* Content fragments. Each gets its own labeled header so the model
     * can see WHICH source contributed what. Memory is filtered through
     * the optional content_is_safe predicate if present — risk-mitigation
     * for the daemon_proactive callback path that previously leaked
     * first-person memory entries to family contacts.
     * due_followups provides concrete triggers: stored commitments with
     * due dates that the proposer can use as a triggering signal (F25). */
    struct {
        const char *label;
        const char *body;
        size_t body_len;
        bool apply_safety;
    } fields[] = {
        {"situation", inputs->situation_context, inputs->situation_context_len, false},
        {"memory", inputs->memory_context, inputs->memory_context_len, true},
        {"weather", inputs->weather_context, inputs->weather_context_len, false},
        {"calendar", inputs->calendar_context, inputs->calendar_context_len, false},
        {"feeds", inputs->feeds_context, inputs->feeds_context_len, false},
        {"due_followups", inputs->due_followups_context, inputs->due_followups_context_len, false},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (!fields[i].body || fields[i].body_len == 0)
            continue;
        /* Safety predicate gate. When the caller passes a predicate AND
         * it rejects this body, we skip the source entirely (silently;
         * the field stays unrepresented in the prompt). */
        if (fields[i].apply_safety && inputs->content_is_safe &&
            !inputs->content_is_safe(fields[i].body, fields[i].body_len))
            continue;
        if (pos + 1 >= out_cap)
            break;
        int n = snprintf(out + pos, out_cap - pos, "\n--- %s ---\n", fields[i].label);
        if (n < 0)
            break;
        if ((size_t)n >= out_cap - pos) {
            pos = out_cap - 1;
            break;
        }
        pos += (size_t)n;
        size_t avail = out_cap - pos - 1;
        size_t copy = fields[i].body_len < avail ? fields[i].body_len : avail;
        memcpy(out + pos, fields[i].body, copy);
        pos += copy;
    }

    /* Final question — same wording as the bundle-based path. */
    if (pos + 1 < out_cap) {
        int n =
            snprintf(out + pos, out_cap - pos, "\n\nShould h-uman send Seth a message right now?");
        if (n > 0 && (size_t)n < out_cap - pos)
            pos += (size_t)n;
    }
    out[pos] = '\0';
    return pos;
}

/* M3 Dispatch T2 — pure verdict mapping. Exposed in the header so the
 * post-FIRE behavior is unit-testable without spinning a provider. */
hu_init_proposer_result_t hu_init_proposer_evaluate_guard_outcome(int guard_outcome) {
    switch (guard_outcome) {
    case HU_GUARD_OK:
    case HU_GUARD_REWROTE:
        return HU_INIT_RESULT_FIRED;
    case HU_GUARD_REJECT:
        return HU_INIT_RESULT_GUARD_REJECT;
    default:
        /* Defensive: any future outcome we don't recognize is treated
         * as a reject so unknown failures never let a draft slip past. */
        return HU_INIT_RESULT_GUARD_REJECT;
    }
}

hu_error_t hu_init_proposer_tick_with_provider_ex(
    const struct hu_initiative_config *cfg, const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds, struct hu_proactive_budget *budget, const struct hu_agent *agent,
    struct hu_provider *provider, hu_allocator_t *alloc,
    const hu_proactive_compose_inputs_t *inputs, int64_t last_inbound_unix, int64_t now_unix,
    int64_t *last_tick_unix_inout, uint64_t *tick_id_inout, hu_init_proposer_result_t *out_result,
    hu_init_decision_t *out_decision) {
    /* AC-6 backwards compatibility: inputs=NULL → identical to the
     * original function. T2-T8 will land additional behavior; T1 is
     * pure addition. */
    if (!inputs) {
        return hu_init_proposer_tick_with_provider(
            cfg, ar_cfg, tz_offset_seconds, budget, agent, provider, alloc, last_inbound_unix,
            now_unix, last_tick_unix_inout, tick_id_inout, out_result, out_decision);
    }

    /* Inputs-driven path. Run the governor first (cheap, never spends an
     * LLM token unless gates pass). */
    hu_init_proposer_result_t gov_result = HU_INIT_RESULT_SKIP;
    hu_error_t gov_err =
        hu_init_proposer_tick(cfg, ar_cfg, tz_offset_seconds, budget, last_inbound_unix, now_unix,
                              last_tick_unix_inout, tick_id_inout, &gov_result);
    if (gov_err != HU_OK)
        return gov_err;
    if (gov_result != HU_INIT_RESULT_SKIP) {
        if (out_result)
            *out_result = gov_result;
        return HU_OK;
    }

    /* Governor passed; no provider available means no LLM call possible. */
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system || !alloc) {
        if (out_result)
            *out_result = HU_INIT_RESULT_SKIP;
        return HU_OK;
    }

    /* Build prompt from inputs (NOT from agent's cached context). */
    static char sys_prompt[1536];
    static char user_msg[16384];
    size_t sys_len = strlen(s_system_prompt);
    size_t sys_copy = sys_len < sizeof(sys_prompt) - 1 ? sys_len : sizeof(sys_prompt) - 1;
    memcpy(sys_prompt, s_system_prompt, sys_copy);
    sys_prompt[sys_copy] = '\0';
    hu_init_proposer_build_propose_user_message_ex(inputs, now_unix, last_inbound_unix, user_msg,
                                                   sizeof(user_msg));

#if HU_IS_TEST
    /* Test builds: never make a real network call. Unit-test the pure
     * helper directly. */
    (void)agent;
    if (out_result)
        *out_result = HU_INIT_RESULT_SKIP;
    if (out_decision)
        memset(out_decision, 0, sizeof(*out_decision));
    return HU_OK;
#else
    const char *model =
        (cfg->propose_model && cfg->propose_model[0]) ? cfg->propose_model : "gemini-3.5-flash";
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t lerr = init_proposer_call_llm(alloc, provider, sys_prompt, user_msg, model,
                                             &response, &response_len);
    if (lerr != HU_OK || !response || response_len == 0) {
        hu_log_warn("init_proposer", NULL, "LLM call (ex) failed: err=%d (response_len=%zu)",
                    (int)lerr, response_len);
        if (response)
            alloc->free(alloc->ctx, response, response_len + 1);
        if (out_result)
            *out_result = HU_INIT_RESULT_LLM_ERROR;
        return HU_OK;
    }

    hu_init_decision_t decision;
    hu_error_t perr = hu_init_proposer_parse_response(response, response_len, &decision);
    alloc->free(alloc->ctx, response, response_len + 1);
    if (perr != HU_OK) {
        if (out_result)
            *out_result = HU_INIT_RESULT_PARSE_ERROR;
        return HU_OK;
    }

    double threshold = cfg->confidence_threshold > 0.0 ? cfg->confidence_threshold : 0.85;
    hu_init_proposer_result_t verdict = hu_init_proposer_evaluate_decision(&decision, threshold);

    /* M3 Dispatch T2 — validator chain on the FIRED draft. Reactive
     * agent_turn already runs response_guard_check_ex on every outbound;
     * proactive must apply the same gate so G1–G9 detectors (semantic
     * leak, length anomaly, persona PII echo, naked discourse-marker
     * opener — the Jordan incident class) protect proactive outbounds
     * uniformly. On REJECT we capture the rejection as a DPO negative
     * pair (Sprint 41 follow-up #3) and downgrade to GUARD_REJECT; the
     * caller skips the send. Unlike reactive, proactive does NOT retry
     * — the next tick can try again, and retrying a propose-or-skip
     * prompt with a repair-style instruction is semantically odd
     * (no inbound user-msg to repair toward). */
    if (verdict == HU_INIT_RESULT_FIRED && decision.draft_len > 0) {
        hu_guard_context_t guard_ctx;
        memset(&guard_ctx, 0, sizeof(guard_ctx));
        if (agent && agent->persona) {
            if (agent->persona->name && agent->persona->name_len > 1) {
                guard_ctx.persona_name = agent->persona->name;
                guard_ctx.persona_name_len = agent->persona->name_len;
            }
            const char *id =
                agent->persona->identity ? agent->persona->identity : agent->persona->core_anchor;
            if (id) {
                guard_ctx.persona_identity = id;
                guard_ctx.persona_identity_len = strlen(id);
            }
            if (agent->persona->biography) {
                guard_ctx.persona_biography = agent->persona->biography;
                guard_ctx.persona_biography_len = strlen(agent->persona->biography);
            }
        }
        /* Per-channel G9 disable (Sprint 41 follow-up #4) — consult the
         * runtime channel list. Voice-style channels can suppress G9
         * without affecting other detectors. */
        if (inputs->channel_name && inputs->channel_name_len > 0) {
            guard_ctx.naked_opener_disabled = hu_response_guard_g9_disabled_for_channel(
                inputs->channel_name, inputs->channel_name_len);
        }

        char *guard_out = NULL;
        size_t guard_out_len = 0;
        hu_guard_outcome_t guard_outcome = HU_GUARD_OK;
        hu_guard_report_t guard_report;
        memset(&guard_report, 0, sizeof(guard_report));
        hu_error_t gerr =
            hu_response_guard_check_ex(alloc, decision.draft, decision.draft_len, &guard_ctx,
                                       &guard_out, &guard_out_len, &guard_outcome, &guard_report);
        if (gerr == HU_OK) {
            verdict = hu_init_proposer_evaluate_guard_outcome((int)guard_outcome);
            if (guard_outcome == HU_GUARD_REWROTE && guard_out && guard_out_len > 0) {
                /* Copy rewrite back into the decision's fixed buffer,
                 * truncating if the rewrite is somehow longer than
                 * HU_INIT_DRAFT_MAX (extremely rare — guard typically
                 * STRIPS bytes, never adds). */
                size_t copy = guard_out_len < sizeof(decision.draft) - 1
                                  ? guard_out_len
                                  : sizeof(decision.draft) - 1;
                memcpy(decision.draft, guard_out, copy);
                decision.draft[copy] = '\0';
                decision.draft_len = copy;
                alloc->free(alloc->ctx, guard_out, guard_out_len + 1);
            } else if (guard_outcome == HU_GUARD_REJECT) {
                /* Capture the rejection as a DPO negative pair. The
                 * "prompt" for proactive is the propose-or-skip USER
                 * message — captures WHAT context the model was trying
                 * to respond to when it produced the rejected draft. */
                const char *dpo_detector = "unknown";
                if (guard_report.detected_naked_discourse_opener)
                    dpo_detector = "naked_discourse_opener";
                else if (guard_report.detected_persona_identity_echo)
                    dpo_detector = "persona_identity_echo";
                else if (guard_report.detected_persona_pii_echo)
                    dpo_detector = "persona_pii_echo";
                else if (guard_report.detected_director_echo)
                    dpo_detector = "director_echo";
                else if (guard_report.detected_length_anomaly)
                    dpo_detector = "length_anomaly";
                else if (guard_report.detected_semantic_leak)
                    dpo_detector = "semantic_leak";
                else if (guard_report.detected_degenerate_repetition)
                    dpo_detector = "degenerate_repetition";
                (void)hu_response_guard_log_dpo_negative(user_msg, strlen(user_msg), decision.draft,
                                                         decision.draft_len, dpo_detector,
                                                         inputs->channel_name, (int64_t)now_unix);
                hu_log_warn("init_proposer", NULL,
                            "FIRED draft GUARD-REJECTED (channel=%.*s detector=%s len=%zu) — "
                            "skipping send, captured as DPO negative",
                            (int)inputs->channel_name_len,
                            inputs->channel_name ? inputs->channel_name : "", dpo_detector,
                            decision.draft_len);
            }
        }
    }

    hu_log_info("init_proposer", NULL,
                "LLM verdict (ex, channel=%.*s): should_propose=%d confidence=%.3f "
                "draft_len=%zu result=%d",
                (int)inputs->channel_name_len, inputs->channel_name ? inputs->channel_name : "",
                decision.should_propose ? 1 : 0, decision.confidence, decision.draft_len,
                (int)verdict);

    if (out_result)
        *out_result = verdict;
    if (out_decision && verdict == HU_INIT_RESULT_FIRED)
        memcpy(out_decision, &decision, sizeof(decision));
    return HU_OK;
#endif
}
