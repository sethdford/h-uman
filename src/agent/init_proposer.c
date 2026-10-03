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
#include "human/core/llm_purpose.h"
#include "human/core/local_only_guard.h"
#include "human/core/log.h"
#include "human/core/log_redact.h"
#include "human/memory.h"
#include "human/memory/proactive_decisions_repo.h" /* C5 Part A: decision log */
#include "human/provider.h"
#include "human/providers/chat_oneshot.h"
#include "human/reflection.h" /* T8: pull unsurfaced patterns into bundle */
#include <ctype.h>
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

static const char *g_last_llm_caller = NULL;
const char *hu_init_proposer_last_llm_caller_for_test(void) {
    return g_last_llm_caller;
}

void hu_init_proposer_reset_warn_guards_for_test(void) {
#if HU_IS_TEST
    atomic_store(&g_warned_disabled, false);
    atomic_store(&g_warned_enabled, false);
    atomic_store(&g_warned_no_dnd_config, false);
#endif
}

/* Contract C5, Part A — log every proactive PROPOSAL decision so
 * scripts/eval_when_to_speak.py has ground truth for the daemon's OWN
 * decisions (MIR/FIR), not just what eventually got sent. Best-effort: a
 * logging failure (no memory backend, no db, insert error) must never
 * fail the tick itself — this is telemetry, not a gate. Test builds
 * (HU_IS_TEST) skip the write entirely so unit tests never touch a real
 * db path by accident; tests exercise the repo directly instead (see
 * tests/test_proactive_decisions_repo.c). */
/* Did this contact already get a check-in on the same topic in the last 14
 * days? Reads the delivered proactive_send rows (message_ref prefixes). */
static bool init_proposer_repeats_recent_send(const struct hu_agent *agent, const char *contact,
                                              const char *draft, size_t draft_len,
                                              int64_t now_unix) {
#if defined(HU_ENABLE_SQLITE)
    if (!agent || !agent->memory || !contact || !contact[0])
        return false;
    struct sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return false;
    char recent[8][HU_PROACTIVE_REF_MAX];
    size_t n = 0;
    if (hu_proactive_decisions_repo_recent_sent_refs(db, contact, now_unix - (int64_t)14 * 86400,
                                                     recent, 8, &n) != HU_OK)
        return false;
    return hu_init_proposer_repeats_recent(draft, draft_len, (const char (*)[160])recent, n);
#else
    (void)agent;
    (void)contact;
    (void)draft;
    (void)draft_len;
    (void)now_unix;
    return false;
#endif
}

static void init_proposer_record_decision(const struct hu_agent *agent, const char *contact,
                                          const char *trigger, const char *decision,
                                          const char *reason, const char *message_ref,
                                          int64_t now_unix) {
#if defined(HU_ENABLE_SQLITE) && !HU_IS_TEST
    if (!agent || !agent->memory)
        return;
    struct sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
    if (!db)
        return;
    /* sent=0: this row is the PROPOSAL decision. The actual send outcome
     * (whether a channel accepted delivery) is a separate later event
     * logged by hu_daemon_proactive_send_and_record in daemon_proactive.c
     * — recording it here would claim delivery before it happened. */
    hu_error_t err = hu_proactive_decisions_repo_record(db, now_unix, contact, trigger, decision,
                                                        reason, 0, message_ref);
    if (err != HU_OK)
        hu_log_warn("init_proposer", NULL, "proactive_decisions_repo_record failed: err=%d",
                    (int)err);
#else
    (void)agent;
    (void)contact;
    (void)trigger;
    (void)decision;
    (void)reason;
    (void)message_ref;
    (void)now_unix;
#endif
}

/* decision/reason mapping for init_proposer_record_decision — pure so the
 * mapping table itself is trivially auditable (and unit-testable if a
 * future test wants to pin it, without touching sqlite). */
static const char *init_proposer_decision_for_result(hu_init_proposer_result_t r) {
    switch (r) {
    case HU_INIT_RESULT_FIRED:
        return HU_PROACTIVE_DECISION_SEND;
    case HU_INIT_RESULT_GATED_BUDGET:
    case HU_INIT_RESULT_GATED_RECENCY:
    case HU_INIT_RESULT_GATED_INTERVAL:
        return HU_PROACTIVE_DECISION_DEFER;
    default:
        return HU_PROACTIVE_DECISION_DECLINE;
    }
}

static const char *init_proposer_reason_for_result(hu_init_proposer_result_t r) {
    switch (r) {
    case HU_INIT_RESULT_GATED_QUIET:
        return "quiet_hours";
    case HU_INIT_RESULT_GATED_BUDGET:
        return "budget_exhausted";
    case HU_INIT_RESULT_GATED_RECENCY:
        return "recent_inbound";
    case HU_INIT_RESULT_GATED_INTERVAL:
        return "interval_not_elapsed";
    case HU_INIT_RESULT_LLM_ERROR:
        return "llm_error";
    case HU_INIT_RESULT_PARSE_ERROR:
        return "parse_error";
    case HU_INIT_RESULT_LOW_CONFIDENCE:
        return "low_confidence";
    case HU_INIT_RESULT_NEGATIVE:
        return "llm_negative";
    case HU_INIT_RESULT_GUARD_REJECT:
        return "guard_reject";
    case HU_INIT_RESULT_DISABLED:
        return "disabled";
    case HU_INIT_RESULT_SKIP:
        return "no_provider_or_skip";
    case HU_INIT_RESULT_FIRED:
    default:
        return NULL;
    }
}

/* Bounded, always-NUL-terminated copy of inputs->contact_id (which is a
 * pointer+len pair, not guaranteed NUL-terminated) for the decision-log
 * `contact` column. */
static void init_proposer_copy_contact(const hu_proactive_compose_inputs_t *inputs, char *buf,
                                       size_t buf_cap) {
    buf[0] = '\0';
    if (!inputs || !inputs->contact_id || inputs->contact_id_len == 0 || buf_cap == 0)
        return;
    size_t n = inputs->contact_id_len < buf_cap - 1 ? inputs->contact_id_len : buf_cap - 1;
    memcpy(buf, inputs->contact_id, n);
    buf[n] = '\0';
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
        *out_result = HU_INIT_RESULT_DISABLED;
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
                                                                   : "gemini-3.1-pro-preview");

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

/* Contact check-ins (the _ex path with inputs->contact_id set) decide whether
 * SETH should text a specific person, and draft that text in his voice. They
 * used to reuse s_system_prompt, which asks whether h-uman should message
 * Seth, while the daemon's situation brief says "You're initiating a check-in
 * text to <contact>". Given two opposite tasks, the model declined ~49 of 57
 * contact check-ins as "confused" (2026-09-26 decline audit). */
static const char *const s_contact_system_prompt =
    "You are the Initiative Layer of h-uman, a private assistant that texts on "
    "Seth's behalf from his own phone. Decide whether Seth should send a text to "
    "the contact identified in the user message right now, and if so, draft it. "
    "The draft is a text from Seth to that person, written in his voice. Nobody "
    "is messaging Seth.\n"
    "\n"
    "The situation section is Seth's brief for this text (who they are to him, "
    "tone, length). Treat its instructions as drafting guidance. Where it says to "
    "reply SKIP, set should_propose=false instead. Bias toward silence: no text "
    "beats a generic or awkward one. Use only the context provided and do NOT "
    "invent facts.\n"
    "\n"
    "Return ONLY a single JSON object on a single line: no prose, no preamble, no "
    "markdown code fences. The first character MUST be `{` and the last `}`. "
    "Shape:\n"
    "{\n"
    "  \"should_propose\": <true|false>,\n"
    "  \"confidence\": <0.0..1.0>,\n"
    "  \"draft\": \"<the text from Seth to this contact, only when should_propose=true>\",\n"
    "  \"reason\": \"<one short sentence why, when should_propose=false>\"\n"
    "}\n"
    "\n"
    "Score confidence honestly (0.0-1.0): how certain are you that this text "
    "would be welcome and natural coming from Seth? A threshold gates the send.";

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
/* Also called by hu_init_proposer_decide_once in every build, so it is not
 * guarded by HU_IS_TEST (the tick paths still never reach it in tests). */
static hu_error_t init_proposer_call_llm(hu_allocator_t *alloc, struct hu_provider *provider,
                                         const char *sys_prompt, const char *user_msg,
                                         const char *model, char **out_response,
                                         size_t *out_response_len) {
    /* max_tokens 512, thinking_budget 0, json_object: see the comment above. */
    const hu_chat_oneshot_opts_t opts = {
        .temperature = 0.2, .max_tokens = 512, .json_object = true};
    /* Drafts are background work: X-HU-Priority: batch lets a reply jump them. */
    hu_llm_purpose_t prev_purpose = hu_llm_purpose_set(HU_LLM_PURPOSE_PROACTIVE);
    hu_error_t err = hu_provider_chat_oneshot(alloc, provider, model, strlen(model), sys_prompt,
                                              strlen(sys_prompt), user_msg, strlen(user_msg), &opts,
                                              out_response, out_response_len);
    (void)hu_llm_purpose_set(prev_purpose);
    return err;
}

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

static hu_error_t init_proposer_tick_run(
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

    /* Log what the model will actually see. A 100%-decline verdict stream
     * is unattributable without this: "empty context, correctly silent"
     * and "rich context, over-conservative prompt" look identical in the
     * verdict log alone. */
    {
        char ctx_summary[512];
        if (hu_init_proposer_format_context_summary(&bundle, ctx_summary, sizeof(ctx_summary)) > 0)
            hu_log_info("init_proposer", NULL, "context bundle: %s", ctx_summary);
    }

    /* Build prompt (T3 pure). */
    static char sys_prompt[1536];
    static char user_msg[16384];
    hu_init_proposer_build_propose_prompt(&bundle, sys_prompt, sizeof(sys_prompt), user_msg,
                                          sizeof(user_msg));
    g_last_llm_caller = hu_local_only_current_caller(); /* the tag this request carries */

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
    const char *model = (cfg->propose_model && cfg->propose_model[0]) ? cfg->propose_model
                                                                      : "gemini-3.1-pro-preview";
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
                    "response parse failed: err=%d response_len=%zu preview=%s", (int)perr,
                    response_len, HU_LOG_TEXT(preview, copy, 120));
        alloc->free(alloc->ctx, response, response_len + 1);
        if (out_result)
            *out_result = HU_INIT_RESULT_PARSE_ERROR;
        return HU_OK;
    }
    alloc->free(alloc->ctx, response, response_len + 1);

    double threshold = cfg->confidence_threshold > 0.0 ? cfg->confidence_threshold : 0.85;
    hu_init_proposer_result_t verdict = hu_init_proposer_evaluate_decision(&decision, threshold);

    hu_log_info("init_proposer", NULL,
                "LLM verdict: should_propose=%d confidence=%.3f draft_len=%zu result=%d "
                "reason=%s",
                decision.should_propose ? 1 : 0, decision.confidence, decision.draft_len,
                (int)verdict, HU_LOG_TEXT(decision.skip_reason, decision.skip_reason_len, 120));

    if (out_result)
        *out_result = verdict;
    if (out_decision && verdict == HU_INIT_RESULT_FIRED)
        memcpy(out_decision, &decision, sizeof(decision));
    return HU_OK;
#endif
}

/* Tags the owner-initiative propose-model request "initiative" for local_only
 * (the daemon calls this entry point directly; the _ex one tags "proactive"). */
hu_error_t hu_init_proposer_tick_with_provider(
    const struct hu_initiative_config *cfg, const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds, struct hu_proactive_budget *budget, const struct hu_agent *agent,
    struct hu_provider *provider, hu_allocator_t *alloc, int64_t last_inbound_unix,
    int64_t now_unix, int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
    hu_init_proposer_result_t *out_result, hu_init_decision_t *out_decision) {
    const char *lo_prev = hu_local_only_enter("initiative"); /* keeps an outer "proactive" */
    hu_error_t err = init_proposer_tick_run(
        cfg, ar_cfg, tz_offset_seconds, budget, agent, provider, alloc, last_inbound_unix, now_unix,
        last_tick_unix_inout, tick_id_inout, out_result, out_decision);
    (void)hu_local_only_set_caller(lo_prev);
    return err;
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

const char *hu_init_proposer_system_prompt_for(const hu_proactive_compose_inputs_t *inputs) {
    if (inputs && inputs->contact_id && inputs->contact_id_len > 0)
        return s_contact_system_prompt;
    return s_system_prompt;
}

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

    /* HU_PROPOSER_CONTEXT block (contact profile, recent thread, insights):
     * pre-rendered with its own headers; absent on every call not pinned to
     * a local provider, so today's prompt is unchanged byte for byte. */
    if (inputs->proposer_context && inputs->proposer_context_len > 0 && pos + 1 < out_cap) {
        size_t avail = out_cap - pos - 1;
        size_t copy = inputs->proposer_context_len < avail ? inputs->proposer_context_len : avail;
        memcpy(out + pos, inputs->proposer_context, copy);
        pos += copy;
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
    bool memory_rendered = false;
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
        if (strcmp(fields[i].label, "memory") == 0)
            memory_rendered = true;
    }

    /* Whose news (2026-09-30): memory says "The user is navigating a recent
     * relocation to Florida" (Seth's move) and the proposer asked his sister
     * and mother how THEIR Florida move was going, six times in eight days. */
    if (memory_rendered && pos + 1 < out_cap) {
        int n = snprintf(out + pos, out_cap - pos,
                         "\n\n--- whose news ---\nMemory lines about \"the user\" describe "
                         "Seth's own life (his move, job, trips). Never ask the contact about "
                         "Seth's news as if it were theirs.");
        if (n > 0 && (size_t)n < out_cap - pos)
            pos += (size_t)n;
    }

    /* Final question — same wording as the bundle-based path. */
    if (pos + 1 < out_cap) {
        const char *question = (inputs->contact_id && inputs->contact_id_len > 0)
                                   ? "\n\nShould Seth text this contact right now?"
                                   : "\n\nShould h-uman send Seth a message right now?";
        int n = snprintf(out + pos, out_cap - pos, "%s", question);
        if (n > 0 && (size_t)n < out_cap - pos)
            pos += (size_t)n;
    }
    out[pos] = '\0';
    return pos;
}

#if !HU_IS_TEST /* its only caller is the _ex LLM tail, compiled out under test */
static const char *init_proposer_guard_detector(const hu_guard_report_t *r) {
    if (r->detected_naked_discourse_opener)
        return "naked_discourse_opener";
    if (r->detected_persona_identity_echo)
        return "persona_identity_echo";
    if (r->detected_persona_pii_echo)
        return "persona_pii_echo";
    if (r->detected_director_echo)
        return "director_echo";
    if (r->detected_length_anomaly)
        return "length_anomaly";
    if (r->detected_semantic_leak)
        return "semantic_leak";
    if (r->detected_degenerate_repetition)
        return "degenerate_repetition";
    return "unknown";
}
#endif

/* After the confidence threshold: the response guard on a FIRED draft (M3
 * Dispatch T2 — the same G1–G9 gate reactive replies get; a rewrite replaces
 * the draft, a reject downgrades to GUARD_REJECT), then the 14-day repeat
 * guard (2026-09-30: Mindy got "how are things settling in down there" three
 * times in a week). No logging, no DPO capture, no decision row — callers
 * decide what to record. */
static hu_init_proposer_result_t
init_proposer_finalize(const struct hu_agent *agent, hu_allocator_t *alloc,
                       const hu_proactive_compose_inputs_t *inputs, const char *contact,
                       int64_t now_unix, hu_init_decision_t *d, hu_init_proposer_result_t verdict,
                       hu_guard_report_t *report, bool *guard_rejected, bool *repeat_rejected) {
    memset(report, 0, sizeof(*report));
    *guard_rejected = false;
    *repeat_rejected = false;
    if (verdict == HU_INIT_RESULT_FIRED && d->draft_len > 0) {
        hu_guard_context_t gctx;
        memset(&gctx, 0, sizeof(gctx));
        if (agent && agent->persona) {
            if (agent->persona->name && agent->persona->name_len > 1) {
                gctx.persona_name = agent->persona->name;
                gctx.persona_name_len = agent->persona->name_len;
            }
            const char *id =
                agent->persona->identity ? agent->persona->identity : agent->persona->core_anchor;
            if (id) {
                gctx.persona_identity = id;
                gctx.persona_identity_len = strlen(id);
            }
            if (agent->persona->biography) {
                gctx.persona_biography = agent->persona->biography;
                gctx.persona_biography_len = strlen(agent->persona->biography);
            }
        }
        /* Per-channel G9 disable (Sprint 41 follow-up #4). */
        if (inputs->channel_name && inputs->channel_name_len > 0)
            gctx.naked_opener_disabled = hu_response_guard_g9_disabled_for_channel(
                inputs->channel_name, inputs->channel_name_len);
        char *gout = NULL;
        size_t gout_len = 0;
        hu_guard_outcome_t outcome = HU_GUARD_OK;
        if (hu_response_guard_check_ex(alloc, d->draft, d->draft_len, &gctx, &gout, &gout_len,
                                       &outcome, report) == HU_OK) {
            verdict = hu_init_proposer_evaluate_guard_outcome((int)outcome);
            if (outcome == HU_GUARD_REWROTE && gout && gout_len > 0) {
                size_t copy = gout_len < sizeof(d->draft) - 1 ? gout_len : sizeof(d->draft) - 1;
                memcpy(d->draft, gout, copy);
                d->draft[copy] = '\0';
                d->draft_len = copy;
                alloc->free(alloc->ctx, gout, gout_len + 1);
            } else if (outcome == HU_GUARD_REJECT) {
                *guard_rejected = true;
            }
        }
    }
    if (verdict == HU_INIT_RESULT_FIRED && d->draft_len > 0 &&
        init_proposer_repeats_recent_send(agent, contact, d->draft, d->draft_len, now_unix)) {
        verdict = HU_INIT_RESULT_GUARD_REJECT;
        *repeat_rejected = true;
    }
    return verdict;
}

hu_init_proposer_result_t
hu_init_proposer_final_verdict(const struct hu_initiative_config *cfg, const struct hu_agent *agent,
                               hu_allocator_t *alloc, const hu_proactive_compose_inputs_t *inputs,
                               int64_t now_unix, hu_init_decision_t *decision) {
    if (!alloc || !inputs || !decision)
        return HU_INIT_RESULT_PARSE_ERROR;
    double threshold = cfg && cfg->confidence_threshold > 0.0 ? cfg->confidence_threshold : 0.85;
    char contact[128];
    init_proposer_copy_contact(inputs, contact, sizeof(contact));
    hu_guard_report_t report;
    bool g = false, r = false;
    return init_proposer_finalize(agent, alloc, inputs, contact, now_unix, decision,
                                  hu_init_proposer_evaluate_decision(decision, threshold), &report,
                                  &g, &r);
}

size_t hu_init_proposer_format_ex_verdict(const hu_proactive_compose_inputs_t *inputs,
                                          const hu_init_decision_t *d, int verdict,
                                          size_t user_msg_bytes, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!inputs || !d)
        return 0;
    int n;
    if (inputs->proposer_context_len > 0)
        n = snprintf(out, cap,
                     "LLM verdict (ex, channel=%.*s): should_propose=%d confidence=%.3f "
                     "draft_len=%zu result=%d user_msg_bytes=%zu reason_len=%zu (context-enriched; "
                     "text not logged)",
                     (int)inputs->channel_name_len,
                     inputs->channel_name ? inputs->channel_name : "", d->should_propose ? 1 : 0,
                     d->confidence, d->draft_len, verdict, user_msg_bytes, d->skip_reason_len);
    else
        n = snprintf(out, cap,
                     "LLM verdict (ex, channel=%.*s): should_propose=%d confidence=%.3f "
                     "draft_len=%zu result=%d user_msg_bytes=%zu reason=%s",
                     (int)inputs->channel_name_len,
                     inputs->channel_name ? inputs->channel_name : "", d->should_propose ? 1 : 0,
                     d->confidence, d->draft_len, verdict, user_msg_bytes,
                     HU_LOG_TEXT(d->skip_reason, d->skip_reason_len, 120));
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

hu_error_t hu_init_proposer_decide_once(hu_allocator_t *alloc, struct hu_provider *provider,
                                        const char *model,
                                        const hu_proactive_compose_inputs_t *inputs,
                                        int64_t now_unix, int64_t last_inbound_unix,
                                        hu_init_decision_t *out) {
    if (!alloc || !provider || !provider->vtable || !inputs || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    const size_t cap = 16384; /* same cap as the _ex path's user message */
    char *user_msg = (char *)alloc->alloc(alloc->ctx, cap);
    if (!user_msg)
        return HU_ERR_OUT_OF_MEMORY;
    hu_init_proposer_build_propose_user_message_ex(inputs, now_unix, last_inbound_unix, user_msg,
                                                   cap);
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t err = init_proposer_call_llm(alloc, provider, s_system_prompt, user_msg,
                                            model ? model : "", &response, &response_len);
    alloc->free(alloc->ctx, user_msg, cap);
    if (err != HU_OK || !response || response_len == 0) {
        if (response)
            alloc->free(alloc->ctx, response, response_len + 1);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }
    err = hu_init_proposer_parse_response(response, response_len, out);
    alloc->free(alloc->ctx, response, response_len + 1);
    return err;
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

static hu_error_t
init_proposer_tick_ex_run(const struct hu_initiative_config *cfg,
                          const struct hu_autoresponder_config *ar_cfg, int32_t tz_offset_seconds,
                          struct hu_proactive_budget *budget, const struct hu_agent *agent,
                          struct hu_provider *provider, hu_allocator_t *alloc,
                          const hu_proactive_compose_inputs_t *inputs, int64_t last_inbound_unix,
                          int64_t now_unix, int64_t *last_tick_unix_inout, uint64_t *tick_id_inout,
                          hu_init_proposer_result_t *out_result, hu_init_decision_t *out_decision) {
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
        char contact_buf[128];
        init_proposer_copy_contact(inputs, contact_buf, sizeof(contact_buf));
        init_proposer_record_decision(agent, contact_buf[0] ? contact_buf : NULL,
                                      "init_proposer_governor",
                                      init_proposer_decision_for_result(gov_result),
                                      init_proposer_reason_for_result(gov_result), NULL, now_unix);
        return HU_OK;
    }

    /* Governor passed; no provider available means no LLM call possible. */
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system || !alloc) {
        if (out_result)
            *out_result = HU_INIT_RESULT_SKIP;
        char contact_buf[128];
        init_proposer_copy_contact(inputs, contact_buf, sizeof(contact_buf));
        init_proposer_record_decision(agent, contact_buf[0] ? contact_buf : NULL,
                                      "init_proposer_governor", HU_PROACTIVE_DECISION_DECLINE,
                                      "no_provider", NULL, now_unix);
        return HU_OK;
    }

    /* Build prompt from inputs (NOT from agent's cached context). */
    static char sys_prompt[1536];
    static char user_msg[16384];
    const char *sys_src = hu_init_proposer_system_prompt_for(inputs);
    size_t sys_len = strlen(sys_src);
    size_t sys_copy = sys_len < sizeof(sys_prompt) - 1 ? sys_len : sizeof(sys_prompt) - 1;
    memcpy(sys_prompt, sys_src, sys_copy);
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
    const char *model = (cfg->propose_model && cfg->propose_model[0]) ? cfg->propose_model
                                                                      : "gemini-3.1-pro-preview";
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t lerr = init_proposer_call_llm(alloc, provider, sys_prompt, user_msg, model,
                                             &response, &response_len);
    char contact_buf[128];
    init_proposer_copy_contact(inputs, contact_buf, sizeof(contact_buf));
    if (lerr != HU_OK || !response || response_len == 0) {
        hu_log_warn("init_proposer", NULL, "LLM call (ex) failed: err=%d (response_len=%zu)",
                    (int)lerr, response_len);
        if (response)
            alloc->free(alloc->ctx, response, response_len + 1);
        if (out_result)
            *out_result = HU_INIT_RESULT_LLM_ERROR;
        if (!inputs->defer_llm_failure_row) /* the caller's fallback records the outcome */
            init_proposer_record_decision(agent, contact_buf[0] ? contact_buf : NULL,
                                          "init_proposer_llm", HU_PROACTIVE_DECISION_DECLINE,
                                          "llm_error", NULL, now_unix);
        return HU_OK;
    }

    hu_init_decision_t decision;
    hu_error_t perr = hu_init_proposer_parse_response(response, response_len, &decision);
    alloc->free(alloc->ctx, response, response_len + 1);
    if (perr != HU_OK) {
        if (out_result)
            *out_result = HU_INIT_RESULT_PARSE_ERROR;
        if (!inputs->defer_llm_failure_row)
            init_proposer_record_decision(agent, contact_buf[0] ? contact_buf : NULL,
                                          "init_proposer_llm", HU_PROACTIVE_DECISION_DECLINE,
                                          "parse_error", NULL, now_unix);
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
    bool guard_rejected = false;
    bool repeat_rejected = false;
    hu_guard_report_t guard_report;
    verdict = init_proposer_finalize(agent, alloc, inputs, contact_buf, now_unix, &decision,
                                     verdict, &guard_report, &guard_rejected, &repeat_rejected);
    /* A proposal built from the HU_PROPOSER_CONTEXT block (real message
     * text) never puts the draft or the reason in the log — lengths only. */
    const bool redact = inputs->proposer_context_len > 0;
    if (guard_rejected) {
        /* Capture the rejection as a DPO negative pair. The "prompt" for
         * proactive is the propose-or-skip USER message — WHAT context the
         * model was responding to when it produced the rejected draft. */
        const char *dpo_detector = init_proposer_guard_detector(&guard_report);
        (void)hu_response_guard_log_dpo_negative(user_msg, strlen(user_msg), decision.draft,
                                                 decision.draft_len, dpo_detector,
                                                 inputs->channel_name, (int64_t)now_unix);
        hu_log_warn("init_proposer", NULL,
                    "FIRED draft GUARD-REJECTED (channel=%.*s detector=%s len=%zu) — "
                    "skipping send, captured as DPO negative",
                    (int)inputs->channel_name_len, inputs->channel_name ? inputs->channel_name : "",
                    dpo_detector, decision.draft_len);
    }
    if (repeat_rejected) {
        if (redact)
            hu_log_info("init_proposer", NULL,
                        "FIRED draft rejected: repeats a check-in from the last 14 days "
                        "(draft_len=%zu)",
                        decision.draft_len);
        else
            hu_log_info("init_proposer", NULL,
                        "FIRED draft rejected: repeats a check-in from the last 14 days (%s)",
                        HU_LOG_TEXT(decision.draft, decision.draft_len, 60));
    }

    {
        char vline[512];
        hu_init_proposer_format_ex_verdict(inputs, &decision, (int)verdict, strlen(user_msg), vline,
                                           sizeof(vline));
        hu_log_info("init_proposer", NULL, "%s", vline);
    }

    if (out_result)
        *out_result = verdict;
    if (out_decision && verdict == HU_INIT_RESULT_FIRED)
        memcpy(out_decision, &decision, sizeof(decision));

    /* message_ref is a short, bounded PREFIX only — this table is a
     * decision log, not a message store (see header for the rationale). */
    char msg_ref_buf[65];
    msg_ref_buf[0] = '\0';
    if (verdict == HU_INIT_RESULT_FIRED && decision.draft_len > 0) {
        size_t n = decision.draft_len < sizeof(msg_ref_buf) - 1 ? decision.draft_len
                                                                : sizeof(msg_ref_buf) - 1;
        memcpy(msg_ref_buf, decision.draft, n);
        msg_ref_buf[n] = '\0';
    }
    init_proposer_record_decision(agent, contact_buf[0] ? contact_buf : NULL, "init_proposer_llm",
                                  init_proposer_decision_for_result(verdict),
                                  init_proposer_reason_for_result(verdict),
                                  msg_ref_buf[0] ? msg_ref_buf : NULL, now_unix);
    return HU_OK;
#endif
}

/* Tags every model request of a proactive tick "proactive" for local_only. */
hu_error_t hu_init_proposer_tick_with_provider_ex(
    const struct hu_initiative_config *cfg, const struct hu_autoresponder_config *ar_cfg,
    int32_t tz_offset_seconds, struct hu_proactive_budget *budget, const struct hu_agent *agent,
    struct hu_provider *provider, hu_allocator_t *alloc,
    const hu_proactive_compose_inputs_t *inputs, int64_t last_inbound_unix, int64_t now_unix,
    int64_t *last_tick_unix_inout, uint64_t *tick_id_inout, hu_init_proposer_result_t *out_result,
    hu_init_decision_t *out_decision) {
    const char *lo_prev = hu_local_only_set_caller("proactive");
    hu_error_t err = init_proposer_tick_ex_run(
        cfg, ar_cfg, tz_offset_seconds, budget, agent, provider, alloc, inputs, last_inbound_unix,
        now_unix, last_tick_unix_inout, tick_id_inout, out_result, out_decision);
    (void)hu_local_only_set_caller(lo_prev);
    return err;
}

/* Content words of a check-in: lowercased letters, apostrophes dropped,
 * 4+ letters, minus greetings and filler that every check-in shares. */
#define REPEAT_MAX_WORDS 24
static size_t repeat_words(const char *s, size_t len, char out[][24]) {
    static const char *const k_filler[] = {
        "hows",  "whats",   "have",     "been",     "going", "doing", "things", "hope", "youre",
        "your",  "morning", "evening",  "there",    "down",  "with",  "that",   "this", "just",
        "okay",  "good",    "along",    "coming",   "like",  "about", "still",  "into", "over",
        "here",  "today",   "week",     "thinking", "some",  "much",  "really", "well", "last",
        "night", "doing",   "anything", "hear",     "from",  "been",  "what",   "when", NULL};
    size_t n = 0, wl = 0;
    char w[24];
    for (size_t i = 0; i <= len && n < REPEAT_MAX_WORDS; i++) {
        char c = i < len ? s[i] : ' ';
        if (isalpha((unsigned char)c)) {
            if (wl + 1 < sizeof(w))
                w[wl++] = (char)tolower((unsigned char)c);
            continue;
        }
        if (c == '\'' || (unsigned char)c >= 0x80)
            continue; /* "how's" -> "hows"; curly quotes are multi-byte */
        if (wl >= 4) {
            w[wl] = '\0';
            bool filler = false;
            for (size_t k = 0; k_filler[k] && !filler; k++)
                filler = strcmp(w, k_filler[k]) == 0;
            if (!filler)
                memcpy(out[n++], w, wl + 1);
        }
        wl = 0;
    }
    return n;
}

bool hu_init_proposer_repeats_recent(const char *draft, size_t draft_len, const char (*recent)[160],
                                     size_t recent_count) {
    if (!draft || draft_len == 0 || !recent)
        return false;
    char a[REPEAT_MAX_WORDS][24];
    size_t na = repeat_words(draft, draft_len, a);
    if (na == 0)
        return false;
    for (size_t r = 0; r < recent_count; r++) {
        char b[REPEAT_MAX_WORDS][24];
        size_t nb = repeat_words(recent[r], strnlen(recent[r], 160), b);
        if (nb == 0)
            continue;
        size_t shared = 0;
        for (size_t i = 0; i < na; i++)
            for (size_t j = 0; j < nb; j++)
                if (strcmp(a[i], b[j]) == 0) {
                    shared++;
                    break;
                }
        size_t smaller = na < nb ? na : nb;
        if (shared > 0 && shared * 2 >= smaller)
            return true;
    }
    return false;
}
