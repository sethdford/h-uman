#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "seaclaw/daemon.h"
#include "seaclaw/agent.h"
#include "seaclaw/agent/anticipatory.h"
#include "seaclaw/agent/conversation_plan.h"
#include "seaclaw/agent/episodic.h"
#include "seaclaw/agent/info_asymmetry.h"
#include "seaclaw/agent/outcomes.h"
#include "seaclaw/agent/proactive.h"
#include "seaclaw/agent/theory_of_mind.h"
#include "seaclaw/config.h"
#include "seaclaw/context/conversation.h"
#include "seaclaw/context/event_extract.h"
#include "seaclaw/context/mood.h"
#include "seaclaw/context/vision.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory/consolidation.h"
#include "seaclaw/memory/deep_extract.h"
#include "seaclaw/memory/emotional_graph.h"
#include "seaclaw/memory/fast_capture.h"
#include "seaclaw/memory/graph.h"
#include "seaclaw/memory/inbox.h"
#include "seaclaw/memory/promotion.h"
#include "seaclaw/memory/retrieval.h"
#include "seaclaw/observability/bth_metrics.h"
#include "seaclaw/persona/voice_maturity.h"
#include <stdlib.h>
#ifdef SC_HAS_PERSONA
#include "seaclaw/persona.h"
#include "seaclaw/persona/auto_profile.h"
#include "seaclaw/persona/auto_tune.h"
#include "seaclaw/persona/replay.h"
#endif
#ifdef SC_HAS_CRON
#include "seaclaw/cron.h"
#include "seaclaw/crontab.h"
#endif
#if defined(SC_ENABLE_IMESSAGE) && !defined(SC_IS_TEST)
#include "seaclaw/channels/imessage.h"
#endif
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* sqlite3.h no longer needed in daemon — moved to channel vtable implementations */

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SC_DAEMON_PID_DIR  ".seaclaw"
#define SC_DAEMON_PID_FILE "seaclaw.pid"
#define SC_MAX_PATH        1024

/* GCC's warn_unused_result is not suppressed by (void) casts */
#define SC_IGNORE_RESULT(expr)   \
    do {                         \
        if ((expr)) { /* noop */ \
        }                        \
    } while (0)

#ifndef SC_IS_TEST
/* Build conversation callback context from memory.
 * Queries memory for past topics relevant to the current message.
 * Returns an allocated string (caller frees) or NULL. */
static char *build_callback_context(sc_allocator_t *alloc, sc_memory_t *memory,
                                    const char *session_id, size_t session_id_len, const char *msg,
                                    size_t msg_len, size_t *out_len) {
    *out_len = 0;
    if (!memory || !memory->vtable || !memory->vtable->recall || !msg || msg_len == 0)
        return NULL;

    sc_memory_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = memory->vtable->recall(memory->ctx, alloc, msg, msg_len, 3, session_id,
                                            session_id_len, &entries, &count);
    if (err != SC_OK || !entries || count == 0)
        return NULL;

    char buf[2048];
    size_t pos = 0;
    int w = snprintf(buf, sizeof(buf), "\nCONTEXT FROM YOUR SHARED HISTORY:\n");
    if (w > 0)
        pos = (size_t)w;

    size_t usable = 0;
    for (size_t i = 0; i < count && i < 3; i++) {
        if (!entries[i].content || entries[i].content_len == 0)
            continue;
        size_t show = entries[i].content_len;
        if (show > 200)
            show = 200;
        w = snprintf(buf + pos, sizeof(buf) - pos, "%.*s\n", (int)show, entries[i].content);
        if (w > 0 && pos + (size_t)w < sizeof(buf)) {
            pos += (size_t)w;
            usable++;
        }
    }

    for (size_t i = 0; i < count; i++)
        sc_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_memory_entry_t));

    if (usable == 0)
        return NULL;

    w = snprintf(buf + pos, sizeof(buf) - pos,
                 "Use this knowledge naturally. Don't reference that you \"remember\" "
                 "things — you just KNOW them, the way you know things about people "
                 "you're close to.\n");
    if (w > 0 && pos + (size_t)w < sizeof(buf))
        pos += (size_t)w;

    char *result = (char *)alloc->alloc(alloc->ctx, pos + 1);
    if (!result)
        return NULL;
    memcpy(result, buf, pos);
    result[pos] = '\0';
    *out_len = pos;
    return result;
}

/* Store a conversation summary as long-term memory.
 * Concatenates the user message and agent response, runs deep-extract
 * on the full exchange, and stores extracted facts scoped to the contact.
 * When graph is non-NULL, also upserts facts and relations into the GraphRAG knowledge graph.
 * agent may be NULL; when non-NULL and bth_metrics set, increments facts_extracted. */
static void store_conversation_summary(sc_allocator_t *alloc, sc_memory_t *memory,
                                       sc_graph_t *graph, sc_agent_t *agent, const char *session_id,
                                       size_t session_id_len, const char *user_msg,
                                       size_t user_msg_len, const char *response,
                                       size_t response_len) {
    if (!alloc || !memory || !memory->vtable || !memory->vtable->store)
        return;
    if (!user_msg || user_msg_len == 0)
        return;
#ifndef SC_ENABLE_SQLITE
    (void)graph;
#endif

    /* Build "them: ... | me: ..." for richer extraction context */
    size_t total = user_msg_len + response_len + 16;
    char *combined = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!combined)
        return;
    int n = snprintf(combined, total + 1, "them: %.*s | me: %.*s", (int)user_msg_len, user_msg,
                     (int)response_len, response);
    if (n <= 0) {
        alloc->free(alloc->ctx, combined, total + 1);
        return;
    }
    size_t combined_len = (size_t)n < total ? (size_t)n : total;

    sc_deep_extract_result_t de;
    memset(&de, 0, sizeof(de));
    sc_error_t err = sc_deep_extract_lightweight(alloc, combined, combined_len, &de);
    if (err == SC_OK && de.fact_count > 0) {
        if (agent && agent->bth_metrics)
            agent->bth_metrics->facts_extracted += de.fact_count;
        static const char cat_name[] = "conversation_summary";
        sc_memory_category_t cat = {
            .tag = SC_MEMORY_CATEGORY_CUSTOM,
            .data.custom = {.name = cat_name, .name_len = sizeof(cat_name) - 1},
        };
        for (size_t i = 0; i < de.fact_count; i++) {
            const sc_extracted_fact_t *f = &de.facts[i];
            if (!f->subject || !f->predicate || !f->object)
                continue;
            char key_buf[256];
            int kn =
                snprintf(key_buf, sizeof(key_buf), "%s:%s:%s", f->subject, f->predicate, f->object);
            if (kn > 0 && (size_t)kn < sizeof(key_buf)) {
                (void)memory->vtable->store(memory->ctx, key_buf, (size_t)kn, f->object,
                                            strlen(f->object), &cat, session_id ? session_id : "",
                                            session_id ? session_id_len : 0);
            }
#ifdef SC_ENABLE_SQLITE
            if (graph) {
                int64_t src_id = 0;
                int64_t tgt_id = 0;
                size_t subj_len = strlen(f->subject);
                size_t obj_len = strlen(f->object);
                sc_relation_type_t rel_type =
                    sc_relation_type_from_string(f->predicate, strlen(f->predicate));
                if (sc_graph_upsert_entity(graph, f->subject, subj_len, SC_ENTITY_UNKNOWN, NULL,
                                           &src_id) == SC_OK &&
                    sc_graph_upsert_entity(graph, f->object, obj_len, SC_ENTITY_UNKNOWN, NULL,
                                           &tgt_id) == SC_OK) {
                    sc_error_t rel_err = sc_graph_upsert_relation(graph, src_id, tgt_id, rel_type,
                                                                  1.0f, f->object, obj_len);
                    if (rel_err != SC_OK)
                        fprintf(stderr, "[daemon] graph: relation upsert failed: %d\n",
                                (int)rel_err);
                }
            }
#endif
        }
    }
#ifdef SC_ENABLE_SQLITE
    if (graph && err == SC_OK && de.relation_count > 0) {
        for (size_t i = 0; i < de.relation_count; i++) {
            const sc_extracted_relation_t *r = &de.relations[i];
            if (!r->entity_a || !r->relation || !r->entity_b)
                continue;
            int64_t src_id = 0;
            int64_t tgt_id = 0;
            size_t a_len = strlen(r->entity_a);
            size_t b_len = strlen(r->entity_b);
            size_t rel_len = strlen(r->relation);
            sc_relation_type_t rel_type = sc_relation_type_from_string(r->relation, rel_len);
            if (sc_graph_upsert_entity(graph, r->entity_a, a_len, SC_ENTITY_UNKNOWN, NULL,
                                       &src_id) == SC_OK &&
                sc_graph_upsert_entity(graph, r->entity_b, b_len, SC_ENTITY_UNKNOWN, NULL,
                                       &tgt_id) == SC_OK) {
                sc_error_t rel_err = sc_graph_upsert_relation(graph, src_id, tgt_id, rel_type, 1.0f,
                                                              r->entity_b, b_len);
                if (rel_err != SC_OK)
                    fprintf(stderr, "[daemon] graph: relation upsert failed: %d\n", (int)rel_err);
            }
        }
    }
#endif
    sc_deep_extract_result_deinit(&de, alloc);
    alloc->free(alloc->ctx, combined, total + 1);
}
#endif /* !SC_IS_TEST */

static sc_error_t validate_home(const char *home) {
    if (!home || !home[0]) {
        fprintf(stderr, "HOME not set\n");
        return SC_ERR_INVALID_ARGUMENT;
    }
    for (const char *p = home; *p; p++) {
        char c = *p;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '/' && c != '.' && c != '_' && c != '-' && c != ' ') {
            fprintf(stderr, "HOME contains unsafe characters\n");
            return SC_ERR_INVALID_ARGUMENT;
        }
    }
    return SC_OK;
}

static int get_pid_path(char *buf, size_t buf_size) {
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    if (validate_home(home) != SC_OK)
        return -1;
    return snprintf(buf, buf_size, "%s/%s/%s", home, SC_DAEMON_PID_DIR, SC_DAEMON_PID_FILE);
}

/* ── Cron field parsing and tick (only when SC_HAS_CRON) ────────────── */
#ifdef SC_HAS_CRON

static bool parse_cron_int(const char *s, int *out) {
    if (!s || !*s)
        return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0')
        return false;
    if (v < 0 || v > 999)
        return false;
    *out = (int)v;
    return true;
}

bool cron_atom_matches(const char *atom, size_t len, int value) {
    if (len == 0)
        return false;

    char buf[32];
    if (len >= sizeof(buf))
        return false;
    memcpy(buf, atom, len);
    buf[len] = '\0';

    char *slash = strchr(buf, '/');
    int step = 0;
    if (slash) {
        *slash = '\0';
        if (!parse_cron_int(slash + 1, &step) || step <= 0)
            return false;
    }

    char *dash = strchr(buf, '-');
    if (buf[0] == '*' && buf[1] == '\0') {
        return step > 0 ? (value % step == 0) : true;
    }

    if (dash && dash != buf) {
        *dash = '\0';
        int lo, hi;
        if (!parse_cron_int(buf, &lo) || !parse_cron_int(dash + 1, &hi))
            return false;
        if (value < lo || value > hi)
            return false;
        return step > 0 ? ((value - lo) % step == 0) : true;
    }

    int exact;
    if (!parse_cron_int(buf, &exact))
        return false;
    return exact == value;
}

bool cron_field_matches(const char *field, int value) {
    if (!field)
        return false;
    if (field[0] == '*' && field[1] == '\0')
        return true;

    const char *p = field;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',')
            p++;
        size_t len = (size_t)(p - start);
        if (cron_atom_matches(start, len, value))
            return true;
        if (*p == ',')
            p++;
    }
    return false;
}

bool sc_cron_schedule_matches(const char *schedule, const struct tm *tm) {
    if (!schedule || !tm)
        return false;
    char buf[128];
    size_t slen = strlen(schedule);
    if (slen >= sizeof(buf))
        return false;
    memcpy(buf, schedule, slen + 1);

    char *fields[5] = {0};
    size_t fi = 0;
    char *tok = buf;
    while (fi < 5 && *tok) {
        while (*tok == ' ')
            tok++;
        if (!*tok)
            break;
        fields[fi++] = tok;
        while (*tok && *tok != ' ')
            tok++;
        if (*tok)
            *tok++ = '\0';
    }
    if (fi < 5)
        return false;

    return cron_field_matches(fields[0], tm->tm_min) &&
           cron_field_matches(fields[1], tm->tm_hour) &&
           cron_field_matches(fields[2], tm->tm_mday) &&
           cron_field_matches(fields[3], tm->tm_mon + 1) &&
           cron_field_matches(fields[4], tm->tm_wday);
}

/* ── Cron tick execution ─────────────────────────────────────────────── */

static void run_cron_tick(sc_allocator_t *alloc) {
    char *cron_path = NULL;
    size_t cron_path_len = 0;
    if (sc_crontab_get_path(alloc, &cron_path, &cron_path_len) != SC_OK)
        return;

    sc_crontab_entry_t *entries = NULL;
    size_t count = 0;
    if (sc_crontab_load(alloc, cron_path, &entries, &count) != SC_OK || count == 0) {
        alloc->free(alloc->ctx, cron_path, cron_path_len + 1);
        if (entries)
            sc_crontab_entries_free(alloc, entries, count);
        return;
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    for (size_t i = 0; i < count; i++) {
        if (!entries[i].enabled || !entries[i].command)
            continue;
        if (!sc_cron_schedule_matches(entries[i].schedule, &tm))
            continue;

#ifndef SC_IS_TEST
        const char *argv[] = {"/bin/sh", "-c", entries[i].command, NULL};
        sc_run_result_t result = {0};
        sc_error_t run_err = sc_process_run(alloc, argv, NULL, 65536, &result);
        if (run_err != SC_OK)
            fprintf(stderr, "[seaclaw] cron job failed: %s (err=%d)\n", entries[i].command,
                    (int)run_err);
        sc_run_result_free(alloc, &result);
#endif
    }

    sc_crontab_entries_free(alloc, entries, count);
    alloc->free(alloc->ctx, cron_path, cron_path_len + 1);
}

sc_error_t sc_service_run_agent_cron(sc_allocator_t *alloc, sc_agent_t *agent,
                                     sc_service_channel_t *channels, size_t channel_count) {
    if (!alloc || !agent || !agent->scheduler)
        return SC_ERR_INVALID_ARGUMENT;

    size_t job_count = 0;
    const sc_cron_job_t *jobs = sc_cron_list_jobs(agent->scheduler, &job_count);
    if (!jobs || job_count == 0)
        return SC_OK;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    for (size_t i = 0; i < job_count; i++) {
        if (jobs[i].type != SC_CRON_JOB_AGENT || !jobs[i].enabled || jobs[i].paused)
            continue;
        if (!jobs[i].command || !jobs[i].expression)
            continue;
        if (!sc_cron_schedule_matches(jobs[i].expression, &tm))
            continue;

        const char *prompt = jobs[i].command;
        size_t prompt_len = strlen(prompt);
        const char *target_channel = jobs[i].channel;

        if (target_channel && channels) {
            agent->active_channel = target_channel;
            agent->active_channel_len = strlen(target_channel);
        }
        agent->active_job_id = jobs[i].id;

        char *response = NULL;
        size_t response_len = 0;
#ifndef SC_IS_TEST
        sc_error_t err = sc_agent_turn(agent, prompt, prompt_len, &response, &response_len);
#else
        (void)prompt_len;
        sc_error_t err = SC_OK;
        response = sc_strndup(alloc, "[agent-cron-test]", 17);
        response_len = 17;
#endif

        agent->active_job_id = 0;
        if (err == SC_OK && response && response_len > 0 && target_channel && channels) {
            /* If response is exactly "SKIP", the agent decided not to engage */
            bool skip = (response_len == 4 && memcmp(response, "SKIP", 4) == 0);
            if (!skip) {
                /* Parse "channel:target" format for directed messages */
                const char *ch_part = target_channel;
                const char *target_part = NULL;
                size_t target_part_len = 0;
                const char *colon = strchr(target_channel, ':');
                char ch_buf[64] = {0};
                if (colon) {
                    size_t ch_len = (size_t)(colon - target_channel);
                    if (ch_len < sizeof(ch_buf)) {
                        memcpy(ch_buf, target_channel, ch_len);
                        ch_buf[ch_len] = '\0';
                        ch_part = ch_buf;
                        target_part = colon + 1;
                        target_part_len = strlen(target_part);
                    }
                }
                for (size_t c = 0; c < channel_count; c++) {
                    if (!channels[c].channel || !channels[c].channel->vtable ||
                        !channels[c].channel->vtable->name)
                        continue;
                    const char *ch_name =
                        channels[c].channel->vtable->name(channels[c].channel->ctx);
                    if (ch_name && strcmp(ch_name, ch_part) == 0) {
                        if (channels[c].channel->vtable->send) {
                            (void)channels[c].channel->vtable->send(
                                channels[c].channel->ctx, target_part, target_part_len, response,
                                response_len, NULL, 0);
                        }
                        break;
                    }
                }
            }
        }

        (void)sc_cron_add_run(agent->scheduler, alloc, jobs[i].id, (int64_t)now,
                              err == SC_OK ? "success" : "failed", response);

        if (response)
            agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
        sc_agent_clear_history(agent);
    }
    return SC_OK;
}

#endif /* SC_HAS_CRON */

/* ── Proactive check-in ──────────────────────────────────────────────── */

#if defined(SC_HAS_PERSONA) && !defined(SC_IS_TEST)

static char *proactive_prompt_for_contact(sc_allocator_t *alloc, sc_memory_t *memory,
                                          const sc_contact_profile_t *cp, size_t *out_len) {
    char *starter = NULL;
    size_t starter_len = 0;
    if (memory && cp->contact_id) {
        (void)sc_proactive_build_starter(alloc, memory, cp->contact_id, strlen(cp->contact_id),
                                         &starter, &starter_len);
    }

    /* Memory-informed topics: recall recent memories about the contact */
    char *mem_ctx = NULL;
    size_t mem_ctx_len = 0;
    if (memory && cp->contact_id) {
        mem_ctx = build_callback_context(alloc, memory, cp->contact_id, strlen(cp->contact_id),
                                         "recent conversation topics", 24, &mem_ctx_len);
    }

    static const char RULES[] =
        "\nRules: "
        "1. One short natural message (not 'hey how are you' — too generic). "
        "2. Reference something specific you know about them or ask about "
        "something from a previous conversation. "
        "3. Keep it under 10 words. "
        "4. If you have nothing specific, share something you saw/did "
        "that made you think of them. "
        "5. Reply SKIP if you genuinely have nothing natural to say.";
    size_t rules_len = sizeof(RULES) - 1;

    char base_buf[256];
    int w = snprintf(base_buf, sizeof(base_buf), "You're initiating a casual check-in text to %s. ",
                     cp->name ? cp->name : "this person");
    size_t base_len = (w > 0 && (size_t)w < sizeof(base_buf)) ? (size_t)w : 0;

    size_t total = base_len + rules_len;
    if (starter && starter_len > 0)
        total += 2 + starter_len; /* "\n\n" + starter */
    if (mem_ctx && mem_ctx_len > 0)
        total += 2 + mem_ctx_len; /* "\n\n" + mem_ctx */

    char *result = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!result) {
        if (starter)
            alloc->free(alloc->ctx, starter, starter_len + 1);
        if (mem_ctx)
            alloc->free(alloc->ctx, mem_ctx, mem_ctx_len + 1);
        *out_len = 0;
        return NULL;
    }

    size_t pos = 0;
    memcpy(result, base_buf, base_len);
    pos = base_len;

    if (starter && starter_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, starter, starter_len);
        pos += starter_len;
        alloc->free(alloc->ctx, starter, starter_len + 1);
    }
    if (mem_ctx && mem_ctx_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, mem_ctx, mem_ctx_len);
        pos += mem_ctx_len;
        alloc->free(alloc->ctx, mem_ctx, mem_ctx_len + 1);
    }

    memcpy(result + pos, RULES, rules_len);
    pos += rules_len;
    result[pos] = '\0';
    *out_len = pos;
    return result;
}

void sc_service_run_proactive_checkins(sc_allocator_t *alloc, sc_agent_t *agent,
                                       sc_service_channel_t *channels, size_t channel_count) {
    if (!alloc || !agent || !agent->persona)
        return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int hour = tm_now.tm_hour;

    /* Only check in during social hours (9am-9pm) */
    if (hour < 9 || hour > 21)
        return;

    for (size_t i = 0; i < agent->persona->contacts_count; i++) {
        const sc_contact_profile_t *cp = &agent->persona->contacts[i];
        if (!cp->proactive_checkin || !cp->proactive_channel || !cp->contact_id)
            continue;

        /* Parse channel:target format */
        const char *ch_part = cp->proactive_channel;
        const char *target_part = cp->contact_id;
        size_t target_len = strlen(target_part);
        const char *colon = strchr(cp->proactive_channel, ':');
        char ch_buf[64] = {0};
        if (colon) {
            size_t ch_len = (size_t)(colon - cp->proactive_channel);
            if (ch_len < sizeof(ch_buf)) {
                memcpy(ch_buf, cp->proactive_channel, ch_len);
                ch_buf[ch_len] = '\0';
                ch_part = ch_buf;
                target_part = colon + 1;
                target_len = strlen(target_part);
            }
        }

        /* Check last interaction time via conversation history */
        for (size_t c = 0; c < channel_count; c++) {
            char *silence_ctx = NULL;
            size_t silence_ctx_len = 0;

            if (!channels[c].channel || !channels[c].channel->vtable ||
                !channels[c].channel->vtable->name)
                continue;
            const char *ch_name = channels[c].channel->vtable->name(channels[c].channel->ctx);
            if (!ch_name || strcmp(ch_name, ch_part) != 0)
                continue;

            if (!channels[c].channel->vtable->load_conversation_history)
                break;

            sc_channel_history_entry_t *entries = NULL;
            size_t entry_count = 0;
            sc_error_t hist_err = channels[c].channel->vtable->load_conversation_history(
                channels[c].channel->ctx, alloc, cp->contact_id, strlen(cp->contact_id), 15,
                &entries, &entry_count);
            if (hist_err != SC_OK)
                fprintf(stderr, "[daemon] proactive: history load failed for %s: %d\n",
                        cp->contact_id, (int)hist_err);

            uint64_t last_contact_ms = 0;
            bool should_checkin = true;
            if (hist_err == SC_OK && entries && entry_count > 0) {
                struct tm last_tm = {0};
                if (strptime(entries[entry_count - 1].timestamp, "%Y-%m-%d %H:%M", &last_tm)) {
                    time_t last_time = mktime(&last_tm);
                    last_contact_ms = (uint64_t)last_time * 1000ULL;
                    double hours_since = difftime(now, last_time) / 3600.0;
                    if (hours_since < 24.0)
                        should_checkin = false;
                }
            }

            /* Build combined text from user messages for event extraction */
            char combined[4096];
            size_t combined_len = 0;
            if (entries && entry_count > 0) {
                for (size_t e = 0; e < entry_count; e++) {
                    if (entries[e].from_me)
                        continue;
                    size_t tlen = strlen(entries[e].text);
                    if (tlen == 0)
                        continue;
                    if (combined_len + tlen + 2 >= sizeof(combined))
                        break;
                    if (combined_len > 0)
                        combined[combined_len++] = '\n';
                    memcpy(combined + combined_len, entries[e].text, tlen);
                    combined_len += tlen;
                }
                combined[combined_len] = '\0';
            }
            if (entries)
                alloc->free(alloc->ctx, entries, entry_count * sizeof(*entries));

#ifndef SC_IS_TEST
            /* Silence check-in: per-contact last activity from conversation history */
            {
                sc_silence_config_t silence_cfg = SC_SILENCE_DEFAULTS;
                sc_proactive_result_t silence_result;
                memset(&silence_result, 0, sizeof(silence_result));
                uint64_t now_ms = (uint64_t)now * 1000ULL;
                if (sc_proactive_check_silence(alloc, last_contact_ms, now_ms, &silence_cfg,
                                               &silence_result) == SC_OK &&
                    silence_result.count > 0) {
                    should_checkin = true;
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->silence_checkins++;
                    sc_proactive_build_context(&silence_result, alloc, 3, &silence_ctx,
                                               &silence_ctx_len);
                }
                sc_proactive_result_deinit(&silence_result, alloc);
            }
#endif

            if (!should_checkin)
                break;

#ifndef SC_IS_TEST
            /* Event-triggered follow-ups from recent messages */
            char *event_ctx = NULL;
            size_t event_ctx_len = 0;
            if (combined_len > 0) {
                sc_event_extract_result_t extract_result;
                memset(&extract_result, 0, sizeof(extract_result));
                if (sc_event_extract(alloc, combined, combined_len, &extract_result) == SC_OK &&
                    extract_result.event_count > 0) {
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->events_extracted += extract_result.event_count;
                    sc_proactive_result_t event_result;
                    memset(&event_result, 0, sizeof(event_result));
                    if (sc_proactive_check_events(alloc, extract_result.events,
                                                  extract_result.event_count,
                                                  &event_result) == SC_OK &&
                        event_result.count > 0) {
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->event_followups++;
                        sc_proactive_build_context(&event_result, alloc, 3, &event_ctx,
                                                   &event_ctx_len);
                    }
                    sc_proactive_result_deinit(&event_result, alloc);
                }
                sc_event_extract_result_deinit(&extract_result, alloc);
            }

            /* Build and send the check-in */
            size_t prompt_len = 0;
            char *prompt = proactive_prompt_for_contact(alloc, agent->memory, cp, &prompt_len);
            if (prompt && event_ctx && event_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + event_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, event_ctx, event_ctx_len);
                    merged[prompt_len + 1 + event_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = prompt_len + 1 + event_ctx_len;
                }
            }
            if (prompt && silence_ctx && silence_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + silence_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, silence_ctx, silence_ctx_len);
                    merged[prompt_len + 1 + silence_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = prompt_len + 1 + silence_ctx_len;
                }
            }
            if (prompt) {
                sc_agent_clear_history(agent);
                agent->active_channel = ch_part;
                agent->active_channel_len = strlen(ch_part);

                char *response = NULL;
                size_t response_len = 0;
                sc_error_t err = sc_agent_turn(agent, prompt, prompt_len, &response, &response_len);

                if (err == SC_OK && response && response_len > 0) {
                    bool skip = (response_len == 4 && memcmp(response, "SKIP", 4) == 0);
                    if (!skip && channels[c].channel->vtable->send) {
                        channels[c].channel->vtable->send(channels[c].channel->ctx, target_part,
                                                          target_len, response, response_len, NULL,
                                                          0);
                        fprintf(stderr, "[seaclaw] proactive check-in sent to %s: %.*s\n",
                                cp->name ? cp->name : cp->contact_id, (int)response_len, response);
                    }
                }
                if (response)
                    agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                alloc->free(alloc->ctx, prompt, prompt_len + 1);
                sc_agent_clear_history(agent);
            }
            if (event_ctx)
                alloc->free(alloc->ctx, event_ctx, event_ctx_len + 1);
            if (silence_ctx)
                alloc->free(alloc->ctx, silence_ctx, silence_ctx_len + 1);
#endif
            break;
        }
    }
}

#endif /* SC_HAS_PERSONA && !SC_IS_TEST */

#ifndef SC_IS_TEST
/* Tapback-worthy: message properties suggest acknowledgment, not invitation to respond.
 * Uses structural checks (length, question mark, word count) instead of word lists. */
static bool is_tapback_worthy(const char *msg, size_t len) {
    if (!msg || len == 0)
        return false;
    /* Question invites response — never tapback */
    if (memchr(msg, '?', len))
        return false;
    /* Very short (<=6 chars), no question: likely acknowledgment */
    if (len <= 6)
        return true;
    /* Single token (no space): emoji, "k", "lol", etc. */
    if (len <= 12) {
        for (size_t i = 0; i < len; i++) {
            if (msg[i] == ' ')
                return false;
        }
        return true;
    }
    /* Substantive message (>20 chars) invites response */
    if (len > 20)
        return false;
    /* Short multi-word but no question: borderline — treat as tapback if very brief */
    return len <= 12;
}
#endif

/* ── Signal handling (non-test only) ─────────────────────────────────── */

#if !defined(SC_IS_TEST) && !defined(_WIN32) && !defined(__CYGWIN__)
static volatile sig_atomic_t g_stop_flag = 0;

static void service_signal_handler(int sig) {
    (void)sig;
    g_stop_flag = 1;
}
#endif

/* ── Streaming callback for channels with send_event ─────────────────────── */

/* ── Service loop ──────────────────────────────────────────────────────── */

sc_error_t sc_service_run(sc_allocator_t *alloc, uint32_t tick_interval_ms,
                          sc_service_channel_t *channels, size_t channel_count, sc_agent_t *agent,
                          const sc_config_t *config) {
    if (!alloc)
        return SC_ERR_INVALID_ARGUMENT;
    if (tick_interval_ms == 0)
        tick_interval_ms = 1000;

#ifdef SC_IS_TEST
    (void)tick_interval_ms;
    (void)channels;
    (void)channel_count;
    (void)agent;
    (void)config;
#ifdef SC_HAS_CRON
    run_cron_tick(alloc);
    sc_service_run_agent_cron(alloc, agent, channels, channel_count);
#endif
    return SC_OK;
#else

#if !defined(_WIN32) && !defined(__CYGWIN__)
    g_stop_flag = 0;
    signal(SIGTERM, service_signal_handler);
    signal(SIGINT, service_signal_handler);
#endif

    /* Enable outcome tracking so persona feedback loop works in daemon */
    sc_outcome_tracker_t daemon_outcomes;
    sc_outcome_tracker_init(&daemon_outcomes, true);
    if (agent && !agent->outcomes)
        sc_agent_set_outcomes(agent, &daemon_outcomes);

#ifdef SC_HAS_CRON
    time_t last_cron_minute = 0;
#endif
#if defined(SC_HAS_PERSONA) && defined(SC_HAS_CRON)
    time_t proactive_due_at = 0;
#endif

    sc_graph_t *graph = NULL;
#ifdef SC_ENABLE_SQLITE
    {
        const char *home = getenv("HOME");
        if (home) {
            char graph_path[SC_MAX_PATH];
            int np = snprintf(graph_path, sizeof(graph_path), "%s/.seaclaw/graph.db", home);
            if (np > 0 && (size_t)np < sizeof(graph_path)) {
                if (sc_graph_open(alloc, graph_path, (size_t)np, &graph) != SC_OK)
                    fprintf(stderr, "[seaclaw] graph open failed: %.*s\n", np, graph_path);
            }
        }
    }
    if (graph && agent && agent->retrieval_engine)
        sc_retrieval_set_graph(agent->retrieval_engine, graph);
#endif

    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t start = (int64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        for (size_t i = 0; i < channel_count; i++) {
            channels[i].last_poll_ms = start;
            channels[i].last_contact_ms = 0;
        }
    }

#if !defined(_WIN32) && !defined(__CYGWIN__)
#define SC_STOP_FLAG g_stop_flag
#else
    volatile int local_stop = 0;
#define SC_STOP_FLAG local_stop
#endif

#ifndef SC_IS_TEST
    static char replay_insights[2048] = {0};
    static size_t replay_insights_len = 0;
    static size_t promotion_counter = 0;
    static char community_insights[2048] = {0};
    static size_t community_insights_len = 0;

    /* Per-contact Theory of Mind belief states */
#define SC_TOM_MAX_CONTACTS 16
    static sc_belief_state_t tom_states[SC_TOM_MAX_CONTACTS];
    static char tom_contact_keys[SC_TOM_MAX_CONTACTS][64];
    static size_t tom_contact_count = 0;
    (void)tom_states;
    (void)tom_contact_keys;
    (void)tom_contact_count;

    /* Per-contact voice maturity profiles */
    static sc_voice_profile_t voice_profiles[SC_TOM_MAX_CONTACTS];
    static char voice_contact_keys[SC_TOM_MAX_CONTACTS][64];
    static size_t voice_contact_count = 0;
    (void)voice_profiles;
    (void)voice_contact_keys;
    (void)voice_contact_count;
    static sc_bth_metrics_t bth_metrics;
    sc_bth_metrics_init(&bth_metrics);
    if (agent)
        agent->bth_metrics = &bth_metrics;

    sc_inbox_watcher_t inbox_watcher = {0};
    static int64_t last_inbox_poll_ms = 0;
    if (agent && agent->memory) {
        (void)sc_inbox_init(&inbox_watcher, alloc, agent->memory, NULL, 0);
        inbox_watcher.provider = &agent->provider;
        inbox_watcher.model = agent->model_name;
        inbox_watcher.model_len = agent->model_name_len;
    }
#endif

    while (!SC_STOP_FLAG) {
#ifdef SC_HAS_CRON
        {
            time_t t = time(NULL);
            time_t current_minute = t / 60;
            if (current_minute > last_cron_minute) {
                run_cron_tick(alloc);
                sc_service_run_agent_cron(alloc, agent, channels, channel_count);
#ifdef SC_HAS_PERSONA
                /* Run proactive check-ins at the top of each hour.
                 * Add jitter (0-30 min) via deferred scheduling to avoid
                 * blocking the daemon loop. */
                if (current_minute % 60 == 0 && proactive_due_at == 0) {
                    unsigned int jitter_sec = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                    jitter_sec = (unsigned int)arc4random_uniform(1801);
#else
                    {
                        uint32_t s = (uint32_t)time(NULL);
                        s = s * 1103515245u + 12345u;
                        jitter_sec = (unsigned int)((s >> 16u) & 0x7fffu) % 1801u;
                    }
#endif
                    proactive_due_at = t + (time_t)jitter_sec;
                }
                if (proactive_due_at > 0 && t >= proactive_due_at) {
                    proactive_due_at = 0;
                    sc_service_run_proactive_checkins(alloc, agent, channels, channel_count);
                    if (agent && agent->bth_metrics)
                        sc_bth_metrics_log(agent->bth_metrics);
                }
#endif
#ifndef SC_IS_TEST
                /* Daily memory consolidation at 3 AM */
                {
                    static bool consolidated_today = false;
                    struct tm tm_buf;
#if defined(_WIN32) && !defined(__CYGWIN__)
                    struct tm *lt = (localtime_s(&tm_buf, &t) == 0) ? &tm_buf : NULL;
#else
                    struct tm *lt = localtime_r(&t, &tm_buf);
#endif
                    if (lt && lt->tm_hour == 4) {
                        consolidated_today = false;
                    }
                    if (lt && lt->tm_hour == 3 && lt->tm_min == 0 && agent && agent->memory &&
                        !consolidated_today) {
                        sc_consolidation_config_t cons_cfg = {
                            .decay_days = 30,
                            .decay_factor = 0.5,
                            .dedup_threshold = 70,
                            .max_entries = 5000,
                            .provider = &agent->provider,
                            .model = agent->model_name,
                            .model_len = agent->model_name_len,
                        };
                        if (sc_memory_consolidate(alloc, agent->memory, &cons_cfg) == SC_OK) {
                            consolidated_today = true;
                            fprintf(stderr, "[seaclaw] daily memory consolidation completed\n");
                        }
                    }
                }
#ifdef SC_HAS_PERSONA
                {
                    static bool tuned_today = false;
                    struct tm tm_tune;
#if defined(_WIN32) && !defined(__CYGWIN__)
                    struct tm *lt_tune = (localtime_s(&tm_tune, &t) == 0) ? &tm_tune : NULL;
#else
                    struct tm *lt_tune = localtime_r(&t, &tm_tune);
#endif
                    if (lt_tune && lt_tune->tm_hour == 5)
                        tuned_today = false;
                    if (lt_tune && lt_tune->tm_hour == 4 && lt_tune->tm_min == 0 && agent &&
                        agent->memory && !tuned_today) {
                        char *tune_summary = NULL;
                        size_t tune_len = 0;
                        if (sc_replay_auto_tune(alloc, agent->memory, NULL, 0, &tune_summary,
                                                &tune_len) == SC_OK &&
                            tune_summary && tune_len > 0) {
                            size_t copy_len = tune_len < sizeof(replay_insights) - 1
                                                  ? tune_len
                                                  : sizeof(replay_insights) - 1;
                            memcpy(replay_insights, tune_summary, copy_len);
                            replay_insights[copy_len] = '\0';
                            replay_insights_len = copy_len;
                            fprintf(stderr, "[seaclaw] daily replay auto-tune completed\n");
                        }
                        if (tune_summary)
                            alloc->free(alloc->ctx, tune_summary, tune_len + 1);
                        tuned_today = true;
                    }
                }
#endif
                /* Weekly GraphRAG community detection at Sunday 2 AM */
#ifdef SC_ENABLE_SQLITE
                {
                    static bool communities_built_this_week = false;
                    struct tm tm_buf2;
                    struct tm *lt = localtime_r(&t, &tm_buf2);
                    if (lt && lt->tm_wday == 1 && lt->tm_hour == 9) {
                        communities_built_this_week = false;
                    }
                    if (lt && lt->tm_wday == 0 && lt->tm_hour == 2 && lt->tm_min == 0 && graph &&
                        !communities_built_this_week) {
                        char *comm_ctx = NULL;
                        size_t comm_len = 0;
                        if (sc_graph_build_communities(graph, alloc, 20, 2047, &comm_ctx,
                                                       &comm_len) == SC_OK &&
                            comm_ctx && comm_len > 0) {
                            size_t copy_len = comm_len < sizeof(community_insights) - 1
                                                  ? comm_len
                                                  : sizeof(community_insights) - 1;
                            memcpy(community_insights, comm_ctx, copy_len);
                            community_insights[copy_len] = '\0';
                            community_insights_len = copy_len;
                            communities_built_this_week = true;
                            fprintf(stderr,
                                    "[seaclaw] weekly GraphRAG community detection completed\n");
                        }
                        if (comm_ctx)
                            alloc->free(alloc->ctx, comm_ctx, comm_len + 1);
                    }
                }
#endif
#endif
                last_cron_minute = current_minute;
            }
        }
#endif

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t tick_now = (int64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;

        for (size_t i = 0; i < channel_count; i++) {
            sc_service_channel_t *ch = &channels[i];
            if (!ch->poll_fn || !ch->channel_ctx)
                continue;
            if (tick_now - ch->last_poll_ms < (int64_t)ch->interval_ms)
                continue;

            sc_channel_loop_msg_t msgs[16];
            memset(msgs, 0, sizeof(msgs));
            size_t count = 0;
            sc_error_t poll_err = ch->poll_fn(ch->channel_ctx, alloc, msgs, 16, &count);
            ch->last_poll_ms = tick_now;
            if (count > 0)
                ch->last_contact_ms = (uint64_t)time(NULL) * 1000ULL;
            if (poll_err != SC_OK && getenv("SC_DEBUG"))
                fprintf(stderr, "[seaclaw] poll error on channel %zu: %d\n", i, (int)poll_err);

            if (!agent || !ch->channel || !ch->channel->vtable || !ch->channel->vtable->send ||
                count == 0) {
                /* Log inbound messages from read-only channels (e.g. Gmail) */
                if (count > 0 && ch->channel && ch->channel->vtable && !ch->channel->vtable->send) {
                    const char *ch_name = ch->channel->vtable->name
                                              ? ch->channel->vtable->name(ch->channel->ctx)
                                              : "?";
                    for (size_t m = 0; m < count; m++) {
                        size_t clen = strlen(msgs[m].content);
                        fprintf(stderr, "[%s] ingest: %.60s%s (from %s)\n", ch_name,
                                msgs[m].content, clen > 60 ? "..." : "", msgs[m].session_key);
                    }
                }
                continue;
            }

            /*
             * Batch consecutive messages from the same sender into one prompt.
             * A real person reads all pending messages, then replies once.
             */
            for (size_t m = 0; m < count; /* advanced inside */) {
                size_t content_len = strlen(msgs[m].content);
                if (content_len == 0) {
                    m++;
                    continue;
                }

                const char *batch_key = msgs[m].session_key;
                size_t key_len = strlen(batch_key);

                /* Gather consecutive messages from the same sender */
                char combined[4096];
                size_t combined_len = 0;
                size_t batch_start = m;
                size_t batch_end = m;

                while (m < count && strcmp(msgs[m].session_key, batch_key) == 0) {
                    size_t mlen = strlen(msgs[m].content);
                    if (mlen == 0) {
                        m++;
                        continue;
                    }
                    if (combined_len + mlen + 2 >= sizeof(combined))
                        break;
                    if (combined_len > 0)
                        combined[combined_len++] = '\n';
                    memcpy(combined + combined_len, msgs[m].content, mlen);
                    combined_len += mlen;
                    batch_end = m;
                    m++;
                }
                combined[combined_len] = '\0';

                if (combined_len == 0)
                    continue;

                /* Clear STM before each contact batch to avoid cross-contact emotion contamination
                 */
                sc_stm_clear(&agent->stm);

#ifndef SC_IS_TEST
                /* Only respond to contacts explicitly listed in persona_contacts.
                 * When a persona is loaded, ALL inbound messages from contacts not in
                 * the allowlist are silently dropped — even if the list is empty. */
#ifdef SC_HAS_PERSONA
                if (agent->persona) {
                    const sc_contact_profile_t *cp_gate =
                        sc_persona_find_contact(agent->persona, batch_key, key_len);
                    if (!cp_gate) {
                        if (getenv("SC_DEBUG"))
                            fprintf(stderr,
                                    "[seaclaw] ignoring message from unknown contact: %.*s\n",
                                    (int)(key_len > 20 ? 20 : key_len), batch_key);
                        continue;
                    }
                }
#endif

                /* Group chat gating: use group classifier to decide engagement */
                if (msgs[batch_start].is_group) {
                    const char *persona_name = NULL;
#ifdef SC_HAS_PERSONA
                    if (agent->persona && agent->persona->identity)
                        persona_name = agent->persona->identity;
#endif
                    sc_group_response_t gr = sc_conversation_classify_group(
                        combined, combined_len, persona_name,
                        persona_name ? strlen(persona_name) : 0, NULL, 0);
                    if (gr == SC_GROUP_SKIP) {
                        fprintf(stderr, "[seaclaw] group: skipping (not addressed): %.*s\n",
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                        continue;
                    }
                }

                /* Response decision using conversation-aware classifier */
                uint32_t extra_delay_ms = 0;

                /* Preload channel history early so the classifier can use it */
                sc_channel_history_entry_t *early_history = NULL;
                size_t early_history_count = 0;
                if (ch->channel->vtable->load_conversation_history) {
                    ch->channel->vtable->load_conversation_history(
                        ch->channel->ctx, alloc, batch_key, key_len, 10, &early_history,
                        &early_history_count);
                }

                sc_response_action_t action = sc_conversation_classify_response(
                    combined, combined_len, early_history, early_history_count, &extra_delay_ms);

                if (early_history)
                    alloc->free(alloc->ctx, early_history,
                                early_history_count * sizeof(sc_channel_history_entry_t));

                /* Tapback-skip: for tapback-worthy messages, 40% chance to not respond */
                bool tapback_skip = false;
#ifndef SC_IS_TEST
                if (action != SC_RESPONSE_SKIP && is_tapback_worthy(combined, combined_len)) {
                    uint32_t r = (uint32_t)time(NULL);
                    r = r * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    r = (r >> 16u) & 0x7fffu;
                    if ((r % 100u) < 40u)
                        tapback_skip = true;
                }
#endif
                if (action == SC_RESPONSE_SKIP || tapback_skip) {
                    if (tapback_skip)
                        fprintf(stderr, "[seaclaw] tapback-skip (no response): %.*s\n",
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                    else
                        fprintf(stderr, "[seaclaw] skipping message (no response needed): %.*s\n",
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                    if (agent->session_store && agent->session_store->vtable &&
                        agent->session_store->vtable->save_message) {
                        for (size_t b = batch_start; b <= batch_end; b++) {
                            size_t blen = strlen(msgs[b].content);
                            if (blen > 0)
                                agent->session_store->vtable->save_message(
                                    agent->session_store->ctx, batch_key, key_len, "user", 4,
                                    msgs[b].content, blen);
                        }
                    }
                    continue;
                }

                /* ── BTH: Late-night mode (b1c) ────────────────────────────── */
                int bth_hour = -1;
#ifndef SC_IS_TEST
                {
                    time_t night_t = time(NULL);
                    struct tm night_buf;
                    struct tm *night_lt = localtime_r(&night_t, &night_buf);
                    if (night_lt)
                        bth_hour = night_lt->tm_hour;
                }
                if (bth_hour >= 0) {
                    if (bth_hour >= 1 && bth_hour < 7) {
                        /* 1AM-7AM: very high SKIP chance (sleeping) */
                        uint32_t night_r = (uint32_t)time(NULL) * 1103515245u + 12345u;
                        if (((night_r >> 16u) % 100u) < 85u) {
                            fprintf(stderr, "[seaclaw] late-night skip (1AM-7AM, hour=%d)\n",
                                    bth_hour);
                            action = SC_RESPONSE_SKIP;
                            continue;
                        }
                        /* If not skipped, force very delayed + brief */
                        extra_delay_ms += 30000 + ((night_r >> 8u) % 60000u);
                        action = SC_RESPONSE_BRIEF;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        /* 7-9AM: brief "just woke up" energy */
                        if (action == SC_RESPONSE_FULL)
                            action = SC_RESPONSE_BRIEF;
                    }
                }
#endif

                /* ── BTH: Media-type awareness (b3c) ──────────────────────── */
                if (action != SC_RESPONSE_SKIP &&
                    sc_conversation_is_media_message(combined, combined_len, NULL, 0)) {
                    action = SC_RESPONSE_BRIEF;
                    fprintf(stderr, "[seaclaw] media message detected, forcing brief response\n");
                }

                /* For BRIEF actions, override max_response_chars to force ultra-short */
                bool brief_mode = (action == SC_RESPONSE_BRIEF);

#ifndef SC_IS_TEST
                if (agent && agent->bth_metrics)
                    agent->bth_metrics->total_turns++;
#endif

                /* Adaptive timing based on message type. Variable delay with jitter,
                 * time-of-day awareness, and rare "busy" delay.
                 * This sleep also serves as the burst accumulation window. */
#ifndef SC_IS_TEST
                {
                    size_t msg_count = batch_end - batch_start + 1;
                    uint32_t base_delay = 2500 + (uint32_t)(msg_count * 1500);
                    if (base_delay > 8000)
                        base_delay = 8000;
                    base_delay += extra_delay_ms;
                    if (base_delay > 15000)
                        base_delay = 15000;

                    /* Add +/- 30% random jitter */
                    uint32_t jitter_range = base_delay * 30 / 100;
                    uint32_t jitter = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                    jitter = (uint32_t)arc4random_uniform(jitter_range * 2 + 1);
#else
                    {
                        uint32_t s = (uint32_t)time(NULL);
                        s = s * 1103515245u + 12345u;
                        jitter = (s >> 16u) & 0x7fffu;
                        jitter = jitter % (jitter_range * 2 + 1);
                    }
#endif
                    int32_t adjusted =
                        (int32_t)base_delay - (int32_t)jitter_range + (int32_t)jitter;
                    if (adjusted < 1000)
                        adjusted = 1000;

                    /* Late-night mode: graduated delay multiplier by time band */
                    if (bth_hour >= 22 || (bth_hour >= 0 && bth_hour < 1))
                        adjusted = adjusted * 2;
                    else if (bth_hour >= 1 && bth_hour < 7)
                        adjusted = adjusted * 3;
                    else if (bth_hour >= 7 && bth_hour < 9)
                        adjusted = adjusted * 3 / 2;
                    else if (bth_hour >= 21)
                        adjusted = adjusted * 3 / 2;

                    /* Rare "busy" delay: 1% chance of 15-45 second delay */
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                    if (arc4random_uniform(100) < 1)
#else
                    if ((((uint32_t)time(NULL) * 1103515245u + 12345u) >> 16u) % 100u < 1u)
#endif
                        adjusted += (int32_t)(15000 + (uint32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                          arc4random_uniform(30001)
#else
                                                          (((uint32_t)time(NULL) * 1103515245u +
                                                            12345u) >>
                                                           16u) %
                                                          30001u
#endif
                                                              ));

                    usleep((useconds_t)((unsigned int)adjusted * 1000));
                }
#else
                {
                    size_t msg_count = batch_end - batch_start + 1;
                    unsigned int read_ms = 2500 + (unsigned int)(msg_count * 1500);
                    if (read_ms > 8000)
                        read_ms = 8000;
                    read_ms += extra_delay_ms;
                    if (read_ms > 15000)
                        read_ms = 15000;
                    usleep(read_ms * 1000);
                }
#endif

                /* Burst accumulation: re-poll for messages that arrived during
                 * the read delay. Humans finish their thought in 2-3 messages,
                 * so we pick up any follow-ups before responding. */
                {
                    sc_channel_loop_msg_t burst[16];
                    size_t burst_count = 0;
                    ch->poll_fn(ch->channel_ctx, alloc, burst, 16, &burst_count);
                    for (size_t bi = 0; bi < burst_count; bi++) {
                        if (strcmp(burst[bi].session_key, batch_key) != 0)
                            continue;
                        size_t mlen = strlen(burst[bi].content);
                        if (mlen == 0)
                            continue;
                        if (combined_len + mlen + 2 >= sizeof(combined))
                            break;
                        combined[combined_len++] = '\n';
                        memcpy(combined + combined_len, burst[bi].content, mlen);
                        combined_len += mlen;
                        if (agent->session_store && agent->session_store->vtable &&
                            agent->session_store->vtable->save_message) {
                            agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                                       batch_key, key_len, "user",
                                                                       4, burst[bi].content, mlen);
                        }
                    }
                    combined[combined_len] = '\0';
                }
#else
                bool brief_mode = false;
#endif

                sc_agent_clear_history(agent);

                /* Set active channel for per-channel persona overlays */
                if (ch->channel->vtable->name) {
                    agent->active_channel = ch->channel->vtable->name(ch->channel->ctx);
                    agent->active_channel_len =
                        agent->active_channel ? strlen(agent->active_channel) : 0;
                } else {
                    agent->active_channel = NULL;
                    agent->active_channel_len = 0;
                }

#ifdef SC_HAS_PERSONA
                /* Apply persona override: per-contact takes priority, then per-channel */
                if (config) {
                    const char *persona_override = NULL;
                    if (batch_key && key_len > 0)
                        persona_override = sc_config_persona_for_contact(config, batch_key);
                    if (!persona_override && agent->active_channel)
                        persona_override =
                            sc_config_persona_for_channel(config, agent->active_channel);
                    if (persona_override && persona_override[0]) {
                        const char *current = agent->persona_name ? agent->persona_name : "";
                        if (strcmp(persona_override, current) != 0) {
                            sc_error_t perr = sc_agent_set_persona(agent, persona_override,
                                                                   strlen(persona_override));
                            if (perr != SC_OK) {
#ifndef SC_IS_TEST
                                fprintf(stderr,
                                        "[seaclaw] warning: failed to switch persona to '%s'\n",
                                        persona_override);
#endif
                            }
                        }
                    }
                }
#else
                (void)config;
#endif

                /* Restore prior conversation for this sender */
                if (agent->session_store && agent->session_store->vtable &&
                    agent->session_store->vtable->load_messages) {
                    sc_message_entry_t *entries = NULL;
                    size_t entry_count = 0;
                    if (agent->session_store->vtable->load_messages(
                            agent->session_store->ctx, alloc, batch_key, key_len, &entries,
                            &entry_count) == SC_OK &&
                        entries && entry_count > 0) {
                        for (size_t e = 0; e < entry_count; e++) {
                            if (!entries[e].content || entries[e].content_len == 0)
                                continue;
                            sc_role_t role = SC_ROLE_USER;
                            if (entries[e].role) {
                                if (strcmp(entries[e].role, "assistant") == 0)
                                    role = SC_ROLE_ASSISTANT;
                                else if (strcmp(entries[e].role, "system") == 0)
                                    role = SC_ROLE_SYSTEM;
                            }
                            if (agent->history_count >= agent->history_cap) {
                                size_t new_cap = agent->history_cap ? agent->history_cap * 2 : 8;
                                sc_owned_message_t *arr = (sc_owned_message_t *)alloc->realloc(
                                    alloc->ctx, agent->history,
                                    agent->history_cap * sizeof(sc_owned_message_t),
                                    new_cap * sizeof(sc_owned_message_t));
                                if (!arr)
                                    break;
                                agent->history = arr;
                                agent->history_cap = new_cap;
                            }
                            sc_owned_message_t *hm = &agent->history[agent->history_count];
                            memset(hm, 0, sizeof(*hm));
                            hm->role = role;
                            hm->content = sc_strndup(agent->alloc, entries[e].content,
                                                     entries[e].content_len);
                            hm->content_len = entries[e].content_len;
                            if (hm->content)
                                agent->history_count++;
                        }
                        for (size_t e = 0; e < entry_count; e++) {
                            if (entries[e].role)
                                alloc->free(alloc->ctx, (void *)entries[e].role,
                                            entries[e].role_len + 1);
                            if (entries[e].content)
                                alloc->free(alloc->ctx, (void *)entries[e].content,
                                            entries[e].content_len + 1);
                        }
                        alloc->free(alloc->ctx, entries, entry_count * sizeof(sc_message_entry_t));
                    }
                }

                char *response = NULL;
                size_t response_len = 0;
                /* sc_agent_turn allocates response via agent->alloc; use agent->alloc for
                 * free/realloc to match. (agent->alloc == alloc at creation time.) */

                /* Build per-turn context via proper architecture:
                 * 1. Contact profile from persona (sc_persona_find_contact)
                 * 2. Conversation history from channel vtable (load_conversation_history)
                 * 3. Awareness from shared analyzer (sc_conversation_build_awareness)
                 * 4. Response constraints from channel vtable (get_response_constraints)
                 */
                char *contact_ctx = NULL;
                size_t contact_ctx_len = 0;
                char *convo_ctx = NULL;
                size_t convo_ctx_len = 0;
                sc_channel_history_entry_t *history_entries = NULL;
                size_t history_count = 0;

#ifndef SC_IS_TEST
                /* 1. Per-contact profile via persona struct */
#ifdef SC_HAS_PERSONA
                if (agent->persona) {
                    const sc_contact_profile_t *cp =
                        sc_persona_find_contact(agent->persona, batch_key, key_len);
                    if (cp) {
                        sc_contact_profile_build_context(alloc, cp, &contact_ctx, &contact_ctx_len);

                        size_t iw_len = 0;
                        char *iw_ctx = sc_persona_build_inner_world_context(
                            alloc, agent->persona, cp->relationship_stage, &iw_len);
                        if (iw_ctx && iw_len > 0 && contact_ctx) {
                            size_t total = contact_ctx_len + iw_len + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, contact_ctx, contact_ctx_len);
                                memcpy(merged + contact_ctx_len, iw_ctx, iw_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, contact_ctx, contact_ctx_len + 1);
                                contact_ctx = merged;
                                contact_ctx_len = total - 1;
                            }
                            alloc->free(alloc->ctx, iw_ctx, iw_len + 1);
                        } else if (iw_ctx) {
                            alloc->free(alloc->ctx, iw_ctx, iw_len + 1);
                        }
                    }
                }
#endif

#if defined(SC_HAS_PERSONA) && !defined(SC_IS_TEST)
                /* BTH: Ongoing per-contact style learning (b2c) — re-run every 10 convos,
                 * use all overlay fields, LRU eviction at cap */
                {
#define SC_STYLE_CACHE_CAP        16
#define SC_STYLE_RELEARN_INTERVAL 10
                    static struct {
                        char key[64];
                        uint32_t convo_count;
                        uint64_t last_used;
                    } style_cache[SC_STYLE_CACHE_CAP];
                    static size_t style_cache_count = 0;
                    static uint64_t style_seq = 0;
                    style_seq++;

                    size_t slot = (size_t)-1;
                    for (size_t sc_i = 0; sc_i < style_cache_count; sc_i++) {
                        if (strncmp(style_cache[sc_i].key, batch_key, key_len) == 0 &&
                            style_cache[sc_i].key[key_len] == '\0') {
                            slot = sc_i;
                            style_cache[sc_i].convo_count++;
                            style_cache[sc_i].last_used = style_seq;
                            break;
                        }
                    }

                    if (slot == (size_t)-1 && key_len < 64) {
                        if (style_cache_count < SC_STYLE_CACHE_CAP) {
                            slot = style_cache_count++;
                        } else {
                            size_t lru = 0;
                            for (size_t sc_i = 1; sc_i < SC_STYLE_CACHE_CAP; sc_i++) {
                                if (style_cache[sc_i].last_used < style_cache[lru].last_used)
                                    lru = sc_i;
                            }
                            slot = lru;
                        }
                        memcpy(style_cache[slot].key, batch_key, key_len);
                        style_cache[slot].key[key_len] = '\0';
                        style_cache[slot].convo_count = 1;
                        style_cache[slot].last_used = style_seq;
                    }

                    bool should_profile = false;
                    if (slot != (size_t)-1) {
                        uint32_t cc = style_cache[slot].convo_count;
                        if (cc == 1 || (cc % SC_STYLE_RELEARN_INTERVAL) == 0)
                            should_profile = true;
                    }

                    if (should_profile) {
                        sc_persona_overlay_t auto_ov;
                        memset(&auto_ov, 0, sizeof(auto_ov));
                        if (sc_persona_auto_profile(alloc, batch_key, key_len, &auto_ov) == SC_OK) {
                            char profile_buf[1024];
                            int pb_n = 0;
                            profile_buf[0] = '\0';

                            if (auto_ov.formality) {
                                pb_n +=
                                    snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                             "Contact formality: %s. ", auto_ov.formality);
                            }
                            if (auto_ov.avg_length) {
                                pb_n +=
                                    snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                             "Avg message length: %s. ", auto_ov.avg_length);
                            }
                            if (auto_ov.emoji_usage) {
                                pb_n +=
                                    snprintf(profile_buf + pb_n, sizeof(profile_buf) - (size_t)pb_n,
                                             "Emoji usage: %s. ", auto_ov.emoji_usage);
                            }
                            if (auto_ov.style_notes) {
                                for (size_t sn = 0;
                                     sn < auto_ov.style_notes_count && (size_t)pb_n < 900; sn++) {
                                    if (auto_ov.style_notes[sn]) {
                                        pb_n += snprintf(profile_buf + pb_n,
                                                         sizeof(profile_buf) - (size_t)pb_n, "%s ",
                                                         auto_ov.style_notes[sn]);
                                    }
                                }
                            }

                            if (pb_n > 0) {
                                char *note = sc_strndup(alloc, profile_buf, (size_t)pb_n);
                                if (note) {
                                    if (contact_ctx) {
                                        size_t total = contact_ctx_len + (size_t)pb_n + 2;
                                        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                                        if (merged) {
                                            memcpy(merged, contact_ctx, contact_ctx_len);
                                            merged[contact_ctx_len] = '\n';
                                            memcpy(merged + contact_ctx_len + 1, note,
                                                   (size_t)pb_n);
                                            merged[total] = '\0';
                                            alloc->free(alloc->ctx, contact_ctx,
                                                        contact_ctx_len + 1);
                                            contact_ctx = merged;
                                            contact_ctx_len = total;
                                        }
                                        alloc->free(alloc->ctx, note, (size_t)pb_n + 1);
                                    } else {
                                        contact_ctx = note;
                                        contact_ctx_len = (size_t)pb_n;
                                    }
                                }
                            }

                            if (auto_ov.formality)
                                alloc->free(alloc->ctx, (char *)auto_ov.formality,
                                            strlen(auto_ov.formality) + 1);
                            if (auto_ov.avg_length)
                                alloc->free(alloc->ctx, (char *)auto_ov.avg_length,
                                            strlen(auto_ov.avg_length) + 1);
                            if (auto_ov.emoji_usage)
                                alloc->free(alloc->ctx, (char *)auto_ov.emoji_usage,
                                            strlen(auto_ov.emoji_usage) + 1);
                            if (auto_ov.style_notes) {
                                for (size_t sn = 0; sn < auto_ov.style_notes_count; sn++) {
                                    if (auto_ov.style_notes[sn])
                                        alloc->free(alloc->ctx, auto_ov.style_notes[sn],
                                                    strlen(auto_ov.style_notes[sn]) + 1);
                                }
                                alloc->free(alloc->ctx, auto_ov.style_notes,
                                            auto_ov.style_notes_count * sizeof(char *));
                            }
                            if (auto_ov.typing_quirks) {
                                for (size_t tq = 0; tq < auto_ov.typing_quirks_count; tq++) {
                                    if (auto_ov.typing_quirks[tq])
                                        alloc->free(alloc->ctx, (char *)auto_ov.typing_quirks[tq],
                                                    strlen(auto_ov.typing_quirks[tq]) + 1);
                                }
                                alloc->free(alloc->ctx, auto_ov.typing_quirks,
                                            auto_ov.typing_quirks_count * sizeof(char *));
                            }
                        }
                    }
                }
#endif

                /* 2. Conversation history via channel vtable */
                if (ch->channel->vtable->load_conversation_history) {
                    ch->channel->vtable->load_conversation_history(
                        ch->channel->ctx, alloc, batch_key, key_len, 25, &history_entries,
                        &history_count);
                }

                /* 2b. Cross-channel awareness: load history from OTHER channels
                 * for the same contact (e.g. Gmail history when texting via iMessage) */
                sc_channel_history_entry_t *cross_entries = NULL;
                size_t cross_count = 0;
#ifdef SC_HAS_PERSONA
                {
                    const sc_contact_profile_t *cp_cross = NULL;
                    if (agent->persona)
                        cp_cross = sc_persona_find_contact(agent->persona, batch_key, key_len);
                    if (cp_cross && cp_cross->email) {
                        size_t email_len = strlen(cp_cross->email);
                        for (size_t ci = 0; ci < channel_count; ci++) {
                            if (&channels[ci] == ch)
                                continue;
                            if (!channels[ci].channel->vtable->load_conversation_history)
                                continue;
                            channels[ci].channel->vtable->load_conversation_history(
                                channels[ci].channel->ctx, alloc, cp_cross->email, email_len, 5,
                                &cross_entries, &cross_count);
                            if (cross_entries && cross_count > 0)
                                break;
                        }
                    }
                }
#endif

                /* 3. Build awareness context from history via shared analyzer.
                 * Merge primary + cross-channel history into one array. */
                if (history_count > 0 || cross_count > 0) {
                    size_t total = history_count + cross_count;
                    sc_channel_history_entry_t *merged_history = NULL;
                    if (cross_count > 0 && history_count > 0) {
                        merged_history = (sc_channel_history_entry_t *)alloc->alloc(
                            alloc->ctx, total * sizeof(sc_channel_history_entry_t));
                        if (merged_history) {
                            if (cross_entries)
                                memcpy(merged_history, cross_entries,
                                       cross_count * sizeof(sc_channel_history_entry_t));
                            memcpy(merged_history + cross_count, history_entries,
                                   history_count * sizeof(sc_channel_history_entry_t));
                        }
                    }
                    const sc_channel_history_entry_t *ctx_entries = merged_history ? merged_history
                                                                    : history_entries
                                                                        ? history_entries
                                                                        : cross_entries;
                    size_t ctx_count = merged_history    ? total
                                       : history_entries ? history_count
                                                         : cross_count;
                    if (ctx_entries && ctx_count > 0) {
                        convo_ctx = sc_conversation_build_awareness(alloc, ctx_entries, ctx_count,
                                                                    &convo_ctx_len);
                    }
                    if (merged_history)
                        alloc->free(alloc->ctx, merged_history,
                                    total * sizeof(sc_channel_history_entry_t));
                }

                /* 2c. Length calibration fallback for channels without history.
                 * When history exists, calibration runs inside build_awareness.
                 * When it doesn't, we still want message-type guidance. */
                if (!convo_ctx && combined_len > 0) {
                    char cal_buf[1024];
                    size_t cal_len = sc_conversation_calibrate_length(combined, combined_len, NULL,
                                                                      0, cal_buf, sizeof(cal_buf));
                    if (cal_len > 0) {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, cal_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, cal_buf, cal_len);
                            convo_ctx[cal_len] = '\0';
                            convo_ctx_len = cal_len;
                        }
                    }
                }

                /* 2d. Style analysis: analyze their texting patterns for mirroring */
                char *style_ctx = NULL;
                size_t style_ctx_len = 0;
                if (history_entries && history_count > 0) {
                    style_ctx = sc_conversation_analyze_style(alloc, history_entries, history_count,
                                                              &style_ctx_len);
                }

                /* Merge style context into conversation context */
                if (style_ctx && convo_ctx) {
                    size_t merged_len = convo_ctx_len + style_ctx_len + 2;
                    char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                    if (merged) {
                        memcpy(merged, convo_ctx, convo_ctx_len);
                        merged[convo_ctx_len] = '\n';
                        memcpy(merged + convo_ctx_len + 1, style_ctx, style_ctx_len);
                        merged[merged_len - 1] = '\n';
                        merged[merged_len] = '\0';
                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                        convo_ctx = merged;
                        convo_ctx_len = merged_len;
                    }
                    alloc->free(alloc->ctx, style_ctx, style_ctx_len + 1);
                    style_ctx = NULL;
                } else if (style_ctx && !convo_ctx) {
                    convo_ctx = style_ctx;
                    convo_ctx_len = style_ctx_len;
                    style_ctx = NULL;
                }
                if (style_ctx)
                    alloc->free(alloc->ctx, style_ctx, style_ctx_len + 1);

                /* Narrative, engagement, emotion: inject when meaningful */
                if (history_entries && history_count > 0) {
                    sc_narrative_phase_t narr =
                        sc_conversation_detect_narrative(history_entries, history_count);
                    sc_engagement_level_t eng =
                        sc_conversation_detect_engagement(history_entries, history_count);
                    sc_emotional_state_t emo =
                        sc_conversation_detect_emotion(history_entries, history_count);

                    bool narr_meaningful =
                        (narr != SC_NARRATIVE_OPENING && narr != SC_NARRATIVE_BUILDING);
                    bool eng_meaningful = (eng != SC_ENGAGEMENT_MODERATE);
                    bool emo_meaningful =
                        (emo.dominant_emotion && strcmp(emo.dominant_emotion, "neutral") != 0) ||
                        emo.concerning;

                    if (narr_meaningful || eng_meaningful || emo_meaningful) {
                        char insights_buf[512];
                        size_t insights_pos = 0;
                        int w = snprintf(insights_buf, sizeof(insights_buf),
                                         "\n--- Narrative/engagement/emotion ---\n");
                        if (w > 0 && (size_t)w < sizeof(insights_buf))
                            insights_pos = (size_t)w;

                        if (narr_meaningful) {
                            const char *phase_name = (narr == SC_NARRATIVE_CLOSING)   ? "closing"
                                                     : (narr == SC_NARRATIVE_PEAK)    ? "peak"
                                                     : (narr == SC_NARRATIVE_RELEASE) ? "release"
                                                     : (narr == SC_NARRATIVE_APPROACHING_CLIMAX)
                                                         ? "approaching climax"
                                                         : "building";
                            w = snprintf(insights_buf + insights_pos,
                                         sizeof(insights_buf) - insights_pos,
                                         "- Narrative phase: %s\n", phase_name);
                            if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                                insights_pos += (size_t)w;
                        }
                        if (eng_meaningful) {
                            const char *eng_name = (eng == SC_ENGAGEMENT_HIGH)  ? "high"
                                                   : (eng == SC_ENGAGEMENT_LOW) ? "low"
                                                   : (eng == SC_ENGAGEMENT_DISTRACTED)
                                                       ? "distracted"
                                                       : "moderate";
                            w = snprintf(insights_buf + insights_pos,
                                         sizeof(insights_buf) - insights_pos, "- Engagement: %s\n",
                                         eng_name);
                            if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                                insights_pos += (size_t)w;
                        }
                        if (emo_meaningful) {
                            if (emo.dominant_emotion &&
                                strcmp(emo.dominant_emotion, "neutral") != 0) {
                                w = snprintf(insights_buf + insights_pos,
                                             sizeof(insights_buf) - insights_pos,
                                             "- Emotional tone: %s%s\n", emo.dominant_emotion,
                                             emo.concerning ? " (concerning)" : "");
                            } else {
                                w = snprintf(insights_buf + insights_pos,
                                             sizeof(insights_buf) - insights_pos,
                                             "- Emotional tone: concerning\n");
                            }
                            if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                                insights_pos += (size_t)w;
                        }

                        w = snprintf(insights_buf + insights_pos,
                                     sizeof(insights_buf) - insights_pos,
                                     "--- End narrative/engagement/emotion ---\n");
                        if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                            insights_pos += (size_t)w;

                        if (convo_ctx) {
                            size_t total = convo_ctx_len + insights_pos + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                memcpy(merged + convo_ctx_len, insights_buf, insights_pos);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, insights_pos + 1);
                            if (convo_ctx) {
                                memcpy(convo_ctx, insights_buf, insights_pos);
                                convo_ctx[insights_pos] = '\0';
                                convo_ctx_len = insights_pos;
                            }
                        }
                    }
                }

                /* GraphRAG: inject knowledge graph context (cross-contact synthesis via batch_key)
                 */
#ifdef SC_ENABLE_SQLITE
                if (graph) {
                    char *graph_ctx = NULL;
                    size_t graph_ctx_len = 0;
                    sc_error_t gerr = sc_graph_build_contact_context(
                        graph, alloc, combined, combined_len, batch_key, key_len, 2, 1024,
                        &graph_ctx, &graph_ctx_len);
                    if (gerr == SC_OK && graph_ctx && graph_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + graph_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, graph_ctx, graph_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = graph_ctx;
                            convo_ctx_len = graph_ctx_len;
                            graph_ctx = NULL;
                        }
                        if (graph_ctx)
                            alloc->free(alloc->ctx, graph_ctx, graph_ctx_len + 1);
                    }
                }
#endif

                /* 6. Attachment context: guidance when attachments detected in history.
                 * When provider supports vision and channel is iMessage, try to get image
                 * path and describe it for richer context. */
                if (history_entries && history_count > 0) {
                    size_t attach_ctx_len = 0;
                    char *attach_ctx = sc_conversation_attachment_context(
                        alloc, history_entries, history_count, &attach_ctx_len);
#ifndef SC_IS_TEST
#if defined(SC_ENABLE_IMESSAGE)
                    /* Vision: if provider supports vision and we have iMessage, try to
                     * get the latest attachment path and describe it. */
                    if (attach_ctx && attach_ctx_len > 0 && agent->provider.vtable &&
                        agent->provider.vtable->supports_vision &&
                        agent->provider.vtable->supports_vision(agent->provider.ctx)) {
                        const char *ch_name = ch->channel->vtable->name
                                                  ? ch->channel->vtable->name(ch->channel->ctx)
                                                  : NULL;
                        if (ch_name && strcmp(ch_name, "imessage") == 0 && batch_key &&
                            key_len > 0) {
                            char *img_path =
                                sc_imessage_get_latest_attachment_path(alloc, batch_key, key_len);
                            if (img_path) {
                                char *desc = NULL;
                                size_t desc_len = 0;
                                const char *model =
                                    agent->model_name ? agent->model_name : "gpt-4o";
                                size_t model_len = agent->model_name_len > 0 ? agent->model_name_len
                                                                             : strlen(model);
                                sc_error_t verr = sc_vision_describe_image(
                                    alloc, &agent->provider, img_path, strlen(img_path), model,
                                    model_len, &desc, &desc_len);
                                alloc->free(alloc->ctx, img_path, strlen(img_path) + 1);
                                if (verr == SC_OK && desc && desc_len > 0) {
                                    size_t vision_ctx_len = 0;
                                    char *vision_ctx = sc_vision_build_context(
                                        alloc, desc, desc_len, &vision_ctx_len);
                                    alloc->free(alloc->ctx, desc, desc_len + 1);
                                    if (vision_ctx && vision_ctx_len > 0) {
                                        alloc->free(alloc->ctx, attach_ctx, attach_ctx_len + 1);
                                        attach_ctx = vision_ctx;
                                        attach_ctx_len = vision_ctx_len;
#ifndef SC_IS_TEST
                                        if (agent && agent->bth_metrics)
                                            agent->bth_metrics->vision_descriptions++;
#endif
                                    }
                                } else if (desc) {
                                    alloc->free(alloc->ctx, desc, desc_len + 1);
                                }
                            }
                        }
                    }
#endif
#endif
                    if (attach_ctx && attach_ctx_len > 0) {
#ifndef SC_IS_TEST
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->attachment_contexts++;
#endif
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + attach_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, attach_ctx, attach_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = attach_ctx;
                            convo_ctx_len = attach_ctx_len;
                            attach_ctx = NULL;
                        }
                        if (attach_ctx)
                            alloc->free(alloc->ctx, attach_ctx, attach_ctx_len + 1);
                    } else if (attach_ctx) {
                        alloc->free(alloc->ctx, attach_ctx, attach_ctx_len + 1);
                    }
                }

                /* 3b. Conversation callbacks: thread callback (history) + memory-based */
                {
                    char *thread_cb = NULL;
                    size_t thread_cb_len = 0;
                    char *mem_cb = NULL;
                    size_t mem_cb_len = 0;
                    if (history_entries && history_count > 0) {
                        thread_cb = sc_conversation_build_callback(alloc, history_entries,
                                                                   history_count, &thread_cb_len);
                    }
                    if (agent->memory && agent->memory->vtable && agent->memory->vtable->recall) {
                        mem_cb = build_callback_context(alloc, agent->memory, batch_key, key_len,
                                                        combined, combined_len, &mem_cb_len);
                    }
                    char *cb_ctx = NULL;
                    size_t cb_len = 0;
                    if (thread_cb && mem_cb) {
                        size_t merged_len = thread_cb_len + mem_cb_len + 2;
                        cb_ctx = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (cb_ctx) {
                            memcpy(cb_ctx, thread_cb, thread_cb_len);
                            cb_ctx[thread_cb_len] = '\n';
                            memcpy(cb_ctx + thread_cb_len + 1, mem_cb, mem_cb_len);
                            cb_ctx[merged_len] = '\0';
                            cb_len = merged_len;
                        }
                        alloc->free(alloc->ctx, thread_cb, thread_cb_len + 1);
                        alloc->free(alloc->ctx, mem_cb, mem_cb_len + 1);
                    } else if (thread_cb) {
                        cb_ctx = thread_cb;
                        cb_len = thread_cb_len;
                    } else if (mem_cb) {
                        cb_ctx = mem_cb;
                        cb_len = mem_cb_len;
                    }
                    if (cb_ctx && convo_ctx) {
                        size_t merged_len = convo_ctx_len + cb_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, cb_ctx, cb_len);
                            merged[merged_len] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = merged_len;
                        }
                        alloc->free(alloc->ctx, cb_ctx, cb_len + 1);
                    } else if (cb_ctx && !convo_ctx) {
                        convo_ctx = cb_ctx;
                        convo_ctx_len = cb_len;
                    }
#ifndef SC_IS_TEST
                    if (cb_len > 0 && agent && agent->bth_metrics)
                        agent->bth_metrics->callbacks_triggered++;
#endif
                }

#ifndef SC_IS_TEST
                /* Episodic: load recent sessions for context */
                char *episodic_ctx = NULL;
                size_t episodic_ctx_len = 0;
                if (agent->memory) {
                    sc_episodic_load(agent->memory, alloc, &episodic_ctx, &episodic_ctx_len);
                }
                if (episodic_ctx && episodic_ctx_len > 0) {
                    if (convo_ctx) {
                        size_t merged_len = convo_ctx_len + episodic_ctx_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, episodic_ctx, episodic_ctx_len);
                            merged[merged_len - 1] = '\n';
                            merged[merged_len] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = merged_len;
                        }
                        alloc->free(alloc->ctx, episodic_ctx, episodic_ctx_len + 1);
                    } else {
                        convo_ctx = episodic_ctx;
                        convo_ctx_len = episodic_ctx_len;
                    }
                }
#endif

#ifdef SC_HAS_PERSONA
#ifndef SC_IS_TEST
                /* Replay insights: inject stored insights from previous conversation */
                if (replay_insights_len > 0) {
                    if (convo_ctx) {
                        size_t merged_len = convo_ctx_len + replay_insights_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, replay_insights,
                                   replay_insights_len);
                            merged[merged_len - 1] = '\n';
                            merged[merged_len] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = merged_len;
                        }
                    } else {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, replay_insights_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, replay_insights, replay_insights_len);
                            convo_ctx[replay_insights_len] = '\0';
                            convo_ctx_len = replay_insights_len;
                        }
                    }
                }
                /* GraphRAG community insights: inject topic clusters from weekly detection */
                if (community_insights_len > 0) {
                    if (convo_ctx) {
                        size_t merged_len = convo_ctx_len + community_insights_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, community_insights,
                                   community_insights_len);
                            merged[merged_len - 1] = '\n';
                            merged[merged_len] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = merged_len;
                        }
                    } else {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, community_insights_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, community_insights, community_insights_len);
                            convo_ctx[community_insights_len] = '\0';
                            convo_ctx_len = community_insights_len;
                        }
                    }
                }
#endif
#endif

                /* 4. Response constraints via channel vtable */
                uint32_t max_chars = 0;
                if (ch->channel->vtable->get_response_constraints) {
                    sc_channel_response_constraints_t constraints = {0};
                    if (ch->channel->vtable->get_response_constraints(ch->channel->ctx,
                                                                      &constraints) == SC_OK) {
                        max_chars = constraints.max_chars;
                    }
                }

                /* Brief mode: force ultra-short response */
                if (brief_mode && max_chars > 50)
                    max_chars = 50;

                /* Honesty guardrail: inject if they asked "did you do X?" */
                {
                    char *honesty = sc_conversation_honesty_check(alloc, combined, combined_len);
                    if (honesty && convo_ctx) {
                        size_t h_len = strlen(honesty);
                        size_t total = convo_ctx_len + h_len + 2;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, honesty, h_len);
                            merged[total - 1] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total - 1;
                        }
                        alloc->free(alloc->ctx, honesty, h_len + 1);
                    } else if (honesty && !convo_ctx) {
                        convo_ctx = honesty;
                        convo_ctx_len = strlen(honesty);
                    } else if (honesty) {
                        alloc->free(alloc->ctx, honesty, strlen(honesty) + 1);
                    }
                }

                /* 5. Anti-repetition: detect patterns in our recent messages */
                if (history_entries && history_count >= 4) {
                    char rep_buf[1024];
                    size_t rep_len = sc_conversation_detect_repetition(
                        history_entries, history_count, rep_buf, sizeof(rep_buf));
                    if (rep_len > 0 && convo_ctx) {
                        size_t total = convo_ctx_len + rep_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            merged[convo_ctx_len] = '\n';
                            memcpy(merged + convo_ctx_len + 1, rep_buf, rep_len);
                            merged[total] = '\0';
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total;
                        }
                    } else if (rep_len > 0 && !convo_ctx) {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, rep_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, rep_buf, rep_len);
                            convo_ctx[rep_len] = '\0';
                            convo_ctx_len = rep_len;
                        }
                    }
                }

                /* 6. Relationship-tier calibration from contact profile */
#ifdef SC_HAS_PERSONA
                if (agent->persona) {
                    const sc_contact_profile_t *cp_rel =
                        sc_persona_find_contact(agent->persona, batch_key, key_len);
                    if (cp_rel && (cp_rel->relationship_stage || cp_rel->warmth_level ||
                                   cp_rel->vulnerability_level)) {
                        char rel_buf[512];
                        size_t rel_len = sc_conversation_calibrate_relationship(
                            cp_rel->relationship_stage, cp_rel->warmth_level,
                            cp_rel->vulnerability_level, rel_buf, sizeof(rel_buf));
                        if (rel_len > 0 && convo_ctx) {
                            size_t total = convo_ctx_len + rel_len + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, rel_buf, rel_len);
                                merged[total] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total;
                            }
                        } else if (rel_len > 0 && !convo_ctx) {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, rel_len + 1);
                            if (convo_ctx) {
                                memcpy(convo_ctx, rel_buf, rel_len);
                                convo_ctx[rel_len] = '\0';
                                convo_ctx_len = rel_len;
                            }
                        }
                    }
                }
#endif

                /* 6b. Link sharing context: when conversation calls for sharing a link */
                if (sc_conversation_should_share_link(combined, combined_len, history_entries,
                                                      history_count)) {
                    static const char LINK_CTX[] =
                        "\n### Link Sharing\nThe conversation naturally calls for sharing a link "
                        "or recommendation. If you have a relevant URL, include it in your "
                        "response.\n";
                    size_t link_len = sizeof(LINK_CTX) - 1;
                    if (convo_ctx) {
                        size_t total = convo_ctx_len + link_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                        if (merged) {
                            memcpy(merged, convo_ctx, convo_ctx_len);
                            memcpy(merged + convo_ctx_len, LINK_CTX, link_len + 1);
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                            convo_ctx = merged;
                            convo_ctx_len = total;
                        }
                    } else {
                        convo_ctx = (char *)alloc->alloc(alloc->ctx, link_len + 1);
                        if (convo_ctx) {
                            memcpy(convo_ctx, LINK_CTX, link_len + 1);
                            convo_ctx_len = link_len;
                        }
                    }
#ifndef SC_IS_TEST
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->link_contexts++;
#endif
                }

                /* Pre-populate STM from current message for egraph */
                {
                    sc_fc_result_t fc_pre;
                    memset(&fc_pre, 0, sizeof(fc_pre));
                    (void)sc_fast_capture(alloc, combined, combined_len, &fc_pre);
                    uint64_t ts_ms = (uint64_t)time(NULL) * 1000;
                    sc_error_t fc_err =
                        sc_stm_record_turn(&agent->stm, "user", 4, combined, combined_len, ts_ms);
                    if (fc_err == SC_OK) {
                        size_t last_idx = sc_stm_count(&agent->stm) - 1;
                        if (fc_pre.primary_topic && fc_pre.primary_topic[0]) {
                            (void)sc_stm_turn_set_primary_topic(&agent->stm, last_idx,
                                                                fc_pre.primary_topic,
                                                                strlen(fc_pre.primary_topic));
                        }
                        for (size_t ei = 0; ei < fc_pre.emotion_count; ei++) {
                            (void)sc_stm_turn_add_emotion(&agent->stm, last_idx,
                                                          fc_pre.emotions[ei].tag,
                                                          fc_pre.emotions[ei].intensity);
                        }
                    }
                    sc_fc_result_deinit(&fc_pre, alloc);
                }

                /* 7. Emotional topic map: topics → dominant emotions from STM */
                {
                    sc_emotional_graph_t egraph;
                    size_t egraph_len = 0;
                    sc_egraph_init(&egraph, *alloc);
                    (void)sc_egraph_populate_from_stm(&egraph, &agent->stm);
                    char *egraph_ctx = sc_egraph_build_context(alloc, &egraph, &egraph_len);
                    sc_egraph_deinit(&egraph);
                    if (egraph_ctx && egraph_len > 0) {
#ifndef SC_IS_TEST
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->egraph_contexts++;
#endif
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + egraph_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, egraph_ctx, egraph_len + 1);
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = egraph_ctx;
                            convo_ctx_len = egraph_len;
                        }
                        if (convo_ctx != egraph_ctx)
                            alloc->free(alloc->ctx, egraph_ctx, egraph_len + 1);
                    } else if (egraph_ctx) {
                        alloc->free(alloc->ctx, egraph_ctx, egraph_len + 1);
                    }
                }

                /* 7b. Mood context: recent emotional state from memory */
                {
                    char *mood_ctx = NULL;
                    size_t mood_ctx_len = 0;
                    if (agent->memory &&
                        sc_mood_build_context(alloc, agent->memory, batch_key, key_len, &mood_ctx,
                                              &mood_ctx_len) == SC_OK &&
                        mood_ctx && mood_ctx_len > 0) {
#ifndef SC_IS_TEST
                        if (agent->bth_metrics)
                            agent->bth_metrics->mood_contexts_built++;
#endif
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + mood_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, mood_ctx, mood_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = mood_ctx;
                            convo_ctx_len = mood_ctx_len;
                            mood_ctx = NULL;
                        }
                        if (mood_ctx)
                            alloc->free(alloc->ctx, mood_ctx, mood_ctx_len + 1);
                    }
                }

                /* ── BTH Tier 1: Wire conversation planning (t1a) ──────────── */
#ifndef SC_IS_TEST
                {
                    sc_conversation_plan_t plan;
                    memset(&plan, 0, sizeof(plan));
                    const char *emotional_ctx = convo_ctx ? convo_ctx : "";
                    size_t emotional_len = convo_ctx ? convo_ctx_len : 0;
                    const char *hist_text = "";
                    size_t hist_len = 0;
                    char hist_buf[2048];
                    if (history_entries && history_count > 0) {
                        size_t hpos = 0;
                        for (size_t hi = 0; hi < history_count && hpos < sizeof(hist_buf) - 1;
                             hi++) {
                            int hw = snprintf(hist_buf + hpos, sizeof(hist_buf) - hpos, "%s: %s\n",
                                              history_entries[hi].from_me ? "me" : "them",
                                              history_entries[hi].text);
                            if (hw > 0 && hpos + (size_t)hw < sizeof(hist_buf))
                                hpos += (size_t)hw;
                        }
                        hist_buf[hpos] = '\0';
                        hist_text = hist_buf;
                        hist_len = hpos;
                    }
                    if (sc_plan_conversation(alloc, combined, combined_len, hist_text, hist_len,
                                             emotional_ctx, emotional_len, &plan) == SC_OK) {
                        char *plan_ctx = NULL;
                        size_t plan_ctx_len = 0;
                        if (sc_plan_build_prompt(&plan, alloc, &plan_ctx, &plan_ctx_len) == SC_OK &&
                            plan_ctx && plan_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + plan_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, plan_ctx, plan_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = plan_ctx;
                                convo_ctx_len = plan_ctx_len;
                                plan_ctx = NULL;
                            }
                            if (plan_ctx)
                                alloc->free(alloc->ctx, plan_ctx, plan_ctx_len + 1);
                        }
                        sc_plan_deinit(&plan, alloc);
                    }
                }

                /* ── BTH Tier 1: Theory of Mind context (t1b-pre) ──────────── */
                {
                    size_t tom_idx = (size_t)-1;
                    for (size_t ti = 0; ti < tom_contact_count; ti++) {
                        if (strncmp(tom_contact_keys[ti], batch_key, key_len) == 0 &&
                            tom_contact_keys[ti][key_len] == '\0') {
                            tom_idx = ti;
                            break;
                        }
                    }
                    if (tom_idx == (size_t)-1 && tom_contact_count < SC_TOM_MAX_CONTACTS &&
                        key_len < 64) {
                        tom_idx = tom_contact_count;
                        memset(&tom_states[tom_idx], 0, sizeof(sc_belief_state_t));
                        sc_tom_init(&tom_states[tom_idx], alloc, batch_key, key_len);
                        memcpy(tom_contact_keys[tom_idx], batch_key, key_len);
                        tom_contact_keys[tom_idx][key_len] = '\0';
                        tom_contact_count++;
                    }
                    if (tom_idx != (size_t)-1 && tom_states[tom_idx].belief_count > 0) {
                        char *tom_ctx = NULL;
                        size_t tom_ctx_len = 0;
                        if (sc_tom_build_context(&tom_states[tom_idx], alloc, &tom_ctx,
                                                 &tom_ctx_len) == SC_OK &&
                            tom_ctx && tom_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + tom_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, tom_ctx, tom_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = tom_ctx;
                                convo_ctx_len = tom_ctx_len;
                                tom_ctx = NULL;
                            }
                            if (tom_ctx)
                                alloc->free(alloc->ctx, tom_ctx, tom_ctx_len + 1);
                        }
                    }
                }

                /* ── BTH Tier 1: Information asymmetry guidance (t1c) ──────── */
                {
                    sc_info_asymmetry_t asym;
                    memset(&asym, 0, sizeof(asym));
                    const char *agent_ctx = convo_ctx ? convo_ctx : "";
                    size_t agent_ctx_len = convo_ctx ? convo_ctx_len : 0;
                    const char *ct_ctx = contact_ctx ? contact_ctx : "";
                    size_t ct_ctx_len = contact_ctx ? contact_ctx_len : 0;
                    if (sc_info_asymmetry_analyze(&asym, alloc, agent_ctx, agent_ctx_len, ct_ctx,
                                                  ct_ctx_len) == SC_OK &&
                        asym.gap_count > 0) {
                        char *ia_ctx = NULL;
                        size_t ia_ctx_len = 0;
                        if (sc_info_asymmetry_build_guidance(&asym, alloc, &ia_ctx, &ia_ctx_len) ==
                                SC_OK &&
                            ia_ctx && ia_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + ia_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, ia_ctx, ia_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = ia_ctx;
                                convo_ctx_len = ia_ctx_len;
                                ia_ctx = NULL;
                            }
                            if (ia_ctx)
                                alloc->free(alloc->ctx, ia_ctx, ia_ctx_len + 1);
                        }
                        sc_info_asymmetry_deinit(&asym, alloc);
                    }
                }

                /* ── BTH Tier 1: Anticipatory actions from GraphRAG (t1d) ──── */
                if (graph) {
                    sc_anticipatory_result_t antic;
                    memset(&antic, 0, sizeof(antic));
                    int64_t now_ts = (int64_t)time(NULL);
                    if (sc_anticipatory_analyze(graph, alloc, batch_key, key_len, now_ts, &antic) ==
                            SC_OK &&
                        antic.action_count > 0) {
                        char *antic_ctx = NULL;
                        size_t antic_ctx_len = 0;
                        if (sc_anticipatory_build_context(&antic, alloc, &antic_ctx,
                                                          &antic_ctx_len) == SC_OK &&
                            antic_ctx && antic_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + antic_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, antic_ctx, antic_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = antic_ctx;
                                convo_ctx_len = antic_ctx_len;
                                antic_ctx = NULL;
                            }
                            if (antic_ctx)
                                alloc->free(alloc->ctx, antic_ctx, antic_ctx_len + 1);
                        }
                        sc_anticipatory_result_deinit(&antic, alloc);
                    }
                }

                /* ── BTH Tier 1: Voice maturity guidance (t1e) ─────────────── */
                {
                    size_t vm_idx = (size_t)-1;
                    for (size_t vi = 0; vi < voice_contact_count; vi++) {
                        if (strncmp(voice_contact_keys[vi], batch_key, key_len) == 0 &&
                            voice_contact_keys[vi][key_len] == '\0') {
                            vm_idx = vi;
                            break;
                        }
                    }
                    if (vm_idx == (size_t)-1 && voice_contact_count < SC_TOM_MAX_CONTACTS &&
                        key_len < 64) {
                        vm_idx = voice_contact_count;
                        sc_voice_profile_init(&voice_profiles[vm_idx]);
                        memcpy(voice_contact_keys[vm_idx], batch_key, key_len);
                        voice_contact_keys[vm_idx][key_len] = '\0';
                        voice_contact_count++;
                    }
                    if (vm_idx != (size_t)-1) {
                        char *vm_ctx = NULL;
                        size_t vm_ctx_len = 0;
                        if (sc_voice_build_guidance(&voice_profiles[vm_idx], alloc, &vm_ctx,
                                                    &vm_ctx_len) == SC_OK &&
                            vm_ctx && vm_ctx_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + vm_ctx_len + 2;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, vm_ctx, vm_ctx_len);
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = vm_ctx;
                                convo_ctx_len = vm_ctx_len;
                                vm_ctx = NULL;
                            }
                            if (vm_ctx)
                                alloc->free(alloc->ctx, vm_ctx, vm_ctx_len + 1);
                        }
                    }
                }

                /* ── BTH: Time-of-day persona overlay (b1c late-night + b3b vulnerability) */
                {
                    const char *tod_overlay = NULL;
                    size_t tod_len = 0;
                    static const char NIGHT_OV[] =
                        "\nIt's late at night. You can be more relaxed, introspective, "
                        "slightly more open than during the day. If it feels natural, share "
                        "something personal or vulnerable. Late-night texts are more intimate.";
                    static const char MORNING_OV[] =
                        "\nIt's early morning — you just woke up. Keep responses brief, "
                        "practical, slightly groggy. Short sentences. No deep philosophical "
                        "conversations yet.";
                    if (bth_hour >= 22 || (bth_hour >= 0 && bth_hour < 1)) {
                        tod_overlay = NIGHT_OV;
                        tod_len = sizeof(NIGHT_OV) - 1;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        tod_overlay = MORNING_OV;
                        tod_len = sizeof(MORNING_OV) - 1;
                    }
                    if (tod_overlay && tod_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + tod_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, tod_overlay, tod_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, tod_len + 1);
                            if (convo_ctx) {
                                memcpy(convo_ctx, tod_overlay, tod_len);
                                convo_ctx[tod_len] = '\0';
                                convo_ctx_len = tod_len;
                            }
                        }
                    }
                }

                /* ── BTH Tier 2: Sentiment momentum (t2b) ─────────────────── */
                if (history_entries && history_count >= 3) {
                    char *sent_ctx = NULL;
                    size_t sent_ctx_len = 0;
                    sent_ctx = sc_conversation_build_sentiment_momentum(
                        alloc, history_entries, history_count, &sent_ctx_len);
                    if (sent_ctx && sent_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + sent_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, sent_ctx, sent_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = sent_ctx;
                            convo_ctx_len = sent_ctx_len;
                            sent_ctx = NULL;
                        }
                        if (sent_ctx)
                            alloc->free(alloc->ctx, sent_ctx, sent_ctx_len + 1);
                    }
                }

                /* ── BTH Tier 2: Topic tangent callback (t2e) ─────────────── */
                if (history_entries && history_count >= 6) {
                    char *tang_ctx = NULL;
                    size_t tang_ctx_len = 0;
                    tang_ctx = sc_conversation_build_tangent_callback(
                        alloc, history_entries, history_count, (uint32_t)time(NULL), &tang_ctx_len);
                    if (tang_ctx && tang_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + tang_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, tang_ctx, tang_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = tang_ctx;
                            convo_ctx_len = tang_ctx_len;
                            tang_ctx = NULL;
                        }
                        if (tang_ctx)
                            alloc->free(alloc->ctx, tang_ctx, tang_ctx_len + 1);
                    }
                }

                /* ── BTH Tier 3: Conversation depth signal (t3b) ──────────── */
                if (history_entries && history_count >= 5) {
                    char *depth_ctx = NULL;
                    size_t depth_ctx_len = 0;
                    depth_ctx = sc_conversation_build_depth_signal(alloc, history_entries,
                                                                   history_count, &depth_ctx_len);
                    if (depth_ctx && depth_ctx_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + depth_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, depth_ctx, depth_ctx_len);
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = depth_ctx;
                            convo_ctx_len = depth_ctx_len;
                            depth_ctx = NULL;
                        }
                        if (depth_ctx)
                            alloc->free(alloc->ctx, depth_ctx, depth_ctx_len + 1);
                    }
                }
#endif

                /* Cap conversation context to avoid overflowing the provider context window.
                 * 32 KB is ~8K tokens — leaves room for system prompt, history, and response. */
                if (convo_ctx && convo_ctx_len > 32768) {
                    convo_ctx[32768] = '\0';
                    convo_ctx_len = 32768;
                }

                /* Set agent per-turn context fields (prompt builder reads these) */
                agent->contact_context = contact_ctx;
                agent->contact_context_len = contact_ctx_len;
                agent->conversation_context = convo_ctx;
                agent->conversation_context_len = convo_ctx_len;
                agent->ab_history_entries = history_entries;
                agent->ab_history_count = history_count;
                agent->max_response_chars = max_chars;

                /* Scope memory to this contact */
                agent->memory_session_id = batch_key;
                agent->memory_session_id_len = key_len;
                if (agent->memory && agent->memory->vtable) {
                    agent->memory->current_session_id = batch_key;
                    agent->memory->current_session_id_len = key_len;
                }

                /* Start typing indicator before LLM call */
                if (ch->channel->vtable->start_typing) {
                    ch->channel->vtable->start_typing(ch->channel->ctx, batch_key, key_len);
                }

                /* Thinking response: send filler first if message warrants it */
                {
                    sc_thinking_response_t thinking;
                    memset(&thinking, 0, sizeof(thinking));
                    bool needs_thinking = sc_conversation_classify_thinking(
                        combined, combined_len, history_entries, history_count, &thinking,
                        (uint32_t)time(NULL));
                    if (needs_thinking && thinking.filler_len > 0) {
                        ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len,
                                                  thinking.filler, thinking.filler_len, NULL, 0);
#ifndef SC_IS_TEST
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->thinking_responses++;
                        usleep((useconds_t)(thinking.delay_ms * 1000));
#endif
                    }
                }

                /* Tapback reactions: check if reaction is more appropriate before text */
                if (ch->channel->vtable->react) {
                    sc_reaction_type_t reaction = sc_conversation_classify_reaction(
                        combined, combined_len, false, history_entries, history_count,
                        (uint32_t)time(NULL));
                    if (reaction != SC_REACTION_NONE) {
                        int64_t msg_id = msgs[batch_end].message_id;
                        if (msg_id > 0) {
                            ch->channel->vtable->react(ch->channel->ctx, batch_key, key_len, msg_id,
                                                       reaction);
#ifndef SC_IS_TEST
                            if (agent->bth_metrics)
                                agent->bth_metrics->reactions_sent++;
#endif
                        }
                    }
                }
#endif

                bool retried = false;
                sc_error_t err = SC_OK;
                do {
                    if (response) {
                        agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                        response = NULL;
                        response_len = 0;
                    }
                    err = sc_agent_turn(agent, combined, combined_len, &response, &response_len);
                    if (err != SC_OK)
                        fprintf(stderr, "[seaclaw] agent turn failed for %.*s: %d\n", (int)key_len,
                                batch_key, (int)err);

#ifndef SC_IS_TEST
                    /* Stop typing indicator after LLM call */
                    if (ch->channel->vtable->stop_typing) {
                        ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                    }

                    /* Quality gate: check response for unnatural patterns.
                     * If needs_revision, retry once with hint. */
                    if (err == SC_OK && response && response_len > 0 && history_entries) {
                        sc_quality_score_t qscore = sc_conversation_evaluate_quality(
                            response, response_len, history_entries, history_count, max_chars);
                        if (qscore.needs_revision && !retried) {
                            retried = true;
                            fprintf(stderr,
                                    "[seaclaw] quality retry: score=%d (b=%d v=%d w=%d n=%d) "
                                    "for %.40s...\n",
                                    qscore.total, qscore.brevity, qscore.validation, qscore.warmth,
                                    qscore.naturalness, response_len > 40 ? response : response);
                            agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                            response = NULL;
                            response_len = 0;
                            /* Prepend data-driven retry hint from quality score */
                            if (convo_ctx) {
                                const char *hint =
                                    qscore.guidance[0] != '\0'
                                        ? qscore.guidance
                                        : (qscore.brevity < 10
                                               ? "Your response was much longer than their "
                                                 "messages. Match their energy."
                                               : (qscore.warmth < 10
                                                      ? "Your response felt distant. Show you care."
                                                      : "Your phrasing felt formal. Drop the "
                                                        "formality."));
                                size_t hint_len = strlen(hint);
                                size_t new_len = hint_len + 1 + convo_ctx_len + 1;
                                char *new_convo = (char *)alloc->alloc(alloc->ctx, new_len);
                                if (new_convo) {
                                    memcpy(new_convo, hint, hint_len);
                                    new_convo[hint_len] = '\n';
                                    memcpy(new_convo + hint_len + 1, convo_ctx, convo_ctx_len);
                                    new_convo[new_len - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = new_convo;
                                    convo_ctx_len = new_len - 1;
                                    agent->conversation_context = convo_ctx;
                                    agent->conversation_context_len = convo_ctx_len;
                                }
                            }
                            if (ch->channel->vtable->start_typing)
                                ch->channel->vtable->start_typing(ch->channel->ctx, batch_key,
                                                                  key_len);
                            continue;
                        } else if (qscore.needs_revision) {
                            fprintf(stderr,
                                    "[seaclaw] quality warning: score=%d (b=%d v=%d w=%d n=%d) "
                                    "for %.40s...\n",
                                    qscore.total, qscore.brevity, qscore.validation, qscore.warmth,
                                    qscore.naturalness, response_len > 40 ? response : response);
                        }
                    }
                    break;
                } while (1);

#ifdef SC_HAS_PERSONA
                /* Replay learning: analyze conversation and store insights for future prompts */
                if (history_entries && history_count > 0) {
                    sc_replay_result_t replay = {0};
                    sc_error_t rerr =
                        sc_replay_analyze(alloc, history_entries, history_count, 2000, &replay);
                    if (rerr == SC_OK) {
#ifndef SC_IS_TEST
                        if (agent->bth_metrics)
                            agent->bth_metrics->replay_analyses++;
#endif
                        size_t rctx_len = 0;
                        char *rctx = sc_replay_build_context(alloc, &replay, &rctx_len);
                        if (rctx && rctx_len > 0) {
#ifndef SC_IS_TEST
                            {
                                size_t copy_len = rctx_len < sizeof(replay_insights) - 1
                                                      ? rctx_len
                                                      : sizeof(replay_insights) - 1;
                                memcpy(replay_insights, rctx, copy_len);
                                replay_insights[copy_len] = '\0';
                                replay_insights_len = copy_len;
                            }
#endif
                            if (history_count > 2 && agent->memory && agent->memory->vtable &&
                                agent->memory->vtable->store) {
                                static const char cat_name[] = "replay_insights";
                                sc_memory_category_t cat = {
                                    .tag = SC_MEMORY_CATEGORY_CUSTOM,
                                    .data.custom = {.name = cat_name,
                                                    .name_len = sizeof(cat_name) - 1},
                                };
                                agent->memory->vtable->store(agent->memory->ctx, "replay:latest",
                                                             13, rctx, rctx_len, &cat, batch_key,
                                                             key_len);
                            }
                        }
                        if (rctx)
                            alloc->free(alloc->ctx, rctx, rctx_len + 1);
                    }
                    sc_replay_result_deinit(&replay, alloc);
                }
#endif
#endif

                /* ── BTH post-turn: Theory of Mind record (t1b-post) ──────── */
#ifndef SC_IS_TEST
                if (err == SC_OK && response && response_len > 0) {
                    size_t tom_idx = (size_t)-1;
                    for (size_t ti = 0; ti < tom_contact_count; ti++) {
                        if (strncmp(tom_contact_keys[ti], batch_key, key_len) == 0 &&
                            tom_contact_keys[ti][key_len] == '\0') {
                            tom_idx = ti;
                            break;
                        }
                    }
                    if (tom_idx != (size_t)-1) {
                        sc_fc_result_t fc_tom;
                        memset(&fc_tom, 0, sizeof(fc_tom));
                        (void)sc_fast_capture(alloc, combined, combined_len, &fc_tom);
                        if (fc_tom.primary_topic && fc_tom.primary_topic[0]) {
                            (void)sc_tom_record_belief(
                                &tom_states[tom_idx], alloc, fc_tom.primary_topic,
                                strlen(fc_tom.primary_topic), SC_BELIEF_KNOWS, 0.8f);
                        }
                        for (size_t ei = 0; ei < fc_tom.entity_count && ei < 3; ei++) {
                            if (fc_tom.entities[ei].name[0]) {
                                (void)sc_tom_record_belief(
                                    &tom_states[tom_idx], alloc, fc_tom.entities[ei].name,
                                    strlen(fc_tom.entities[ei].name), SC_BELIEF_KNOWS, 0.6f);
                            }
                        }
                        sc_fc_result_deinit(&fc_tom, alloc);
                    }
                }

                /* ── BTH post-turn: Voice maturity update (t1e-post) ──────── */
                if (err == SC_OK && response && response_len > 0) {
                    size_t vm_idx = (size_t)-1;
                    for (size_t vi = 0; vi < voice_contact_count; vi++) {
                        if (strncmp(voice_contact_keys[vi], batch_key, key_len) == 0 &&
                            voice_contact_keys[vi][key_len] == '\0') {
                            vm_idx = vi;
                            break;
                        }
                    }
                    if (vm_idx != (size_t)-1) {
                        sc_fc_result_t fc_vm;
                        memset(&fc_vm, 0, sizeof(fc_vm));
                        (void)sc_fast_capture(alloc, combined, combined_len, &fc_vm);
                        bool had_emotion = fc_vm.emotion_count > 0;
                        bool had_topic = fc_vm.primary_topic && fc_vm.primary_topic[0];
                        bool had_humor = false;
                        if (combined_len > 0) {
                            for (size_t ci = 0; ci + 2 < combined_len; ci++) {
                                if ((combined[ci] == 'l' || combined[ci] == 'L') &&
                                    (combined[ci + 1] == 'o' || combined[ci + 1] == 'O') &&
                                    (combined[ci + 2] == 'l' || combined[ci + 2] == 'L')) {
                                    had_humor = true;
                                    break;
                                }
                                if ((combined[ci] == 'h') &&
                                    (ci + 3 < combined_len && combined[ci + 1] == 'a' &&
                                     combined[ci + 2] == 'h' && combined[ci + 3] == 'a')) {
                                    had_humor = true;
                                    break;
                                }
                            }
                        }
                        sc_voice_profile_update(&voice_profiles[vm_idx], had_emotion, had_topic,
                                                had_humor);
                        sc_fc_result_deinit(&fc_vm, alloc);
                    }
                }

                /* ── BTH post-turn: Graph recall tracking + reconsolidate (t1f) */
                if (err == SC_OK && response && response_len > 0 && graph) {
                    sc_deep_extract_result_t de_light;
                    memset(&de_light, 0, sizeof(de_light));
                    if (sc_deep_extract_lightweight(alloc, combined, combined_len, &de_light) ==
                        SC_OK) {
                        for (size_t fi = 0; fi < de_light.fact_count; fi++) {
                            if (!de_light.facts[fi].subject)
                                continue;
                            sc_graph_entity_t ent;
                            memset(&ent, 0, sizeof(ent));
                            if (sc_graph_find_entity(graph, de_light.facts[fi].subject,
                                                     strlen(de_light.facts[fi].subject),
                                                     &ent) == SC_OK &&
                                ent.id > 0) {
                                (void)sc_graph_record_recall(graph, ent.id);
                                if (de_light.facts[fi].object) {
                                    (void)sc_graph_reconsolidate(graph, alloc,
                                                                 de_light.facts[fi].subject,
                                                                 strlen(de_light.facts[fi].subject),
                                                                 de_light.facts[fi].object,
                                                                 strlen(de_light.facts[fi].object));
                                }
                            }
                        }
                        sc_deep_extract_result_deinit(&de_light, alloc);
                    }
                }

                /* ── BTH post-turn: LLM deep extraction (t1g) ─────────────── */
                if (err == SC_OK && response && response_len > 0 && agent->memory && graph) {
                    char convo_buf[4096];
                    int cb_w = snprintf(convo_buf, sizeof(convo_buf), "User: %.*s\nAssistant: %.*s",
                                        (int)combined_len, combined, (int)response_len, response);
                    if (cb_w > 0 && (size_t)cb_w < sizeof(convo_buf)) {
                        char *de_prompt = NULL;
                        size_t de_prompt_len = 0;
                        if (sc_deep_extract_build_prompt(alloc, convo_buf, (size_t)cb_w, &de_prompt,
                                                         &de_prompt_len) == SC_OK &&
                            de_prompt && de_prompt_len > 0) {
                            char *de_response = NULL;
                            size_t de_response_len = 0;
                            sc_error_t de_err = sc_agent_turn(agent, de_prompt, de_prompt_len,
                                                              &de_response, &de_response_len);
                            if (de_err == SC_OK && de_response && de_response_len > 0) {
                                sc_deep_extract_result_t de_result;
                                memset(&de_result, 0, sizeof(de_result));
                                if (sc_deep_extract_parse(alloc, de_response, de_response_len,
                                                          &de_result) == SC_OK) {
                                    for (size_t ri = 0; ri < de_result.relation_count; ri++) {
                                        if (!de_result.relations[ri].entity_a ||
                                            !de_result.relations[ri].entity_b)
                                            continue;
                                        int64_t src_id = 0, tgt_id = 0;
                                        (void)sc_graph_upsert_entity(
                                            graph, de_result.relations[ri].entity_a,
                                            strlen(de_result.relations[ri].entity_a),
                                            SC_ENTITY_UNKNOWN, NULL, &src_id);
                                        (void)sc_graph_upsert_entity(
                                            graph, de_result.relations[ri].entity_b,
                                            strlen(de_result.relations[ri].entity_b),
                                            SC_ENTITY_UNKNOWN, NULL, &tgt_id);
                                        if (src_id > 0 && tgt_id > 0) {
                                            const char *rel_str =
                                                de_result.relations[ri].relation
                                                    ? de_result.relations[ri].relation
                                                    : "";
                                            (void)sc_graph_upsert_relation(
                                                graph, src_id, tgt_id, SC_REL_RELATED_TO,
                                                (float)de_result.relations[ri].confidence, rel_str,
                                                strlen(rel_str));
                                        }
                                    }
                                    sc_deep_extract_result_deinit(&de_result, alloc);
                                }
                            }
                            if (de_response)
                                agent->alloc->free(agent->alloc->ctx, de_response,
                                                   de_response_len + 1);
                            alloc->free(alloc->ctx, de_prompt, de_prompt_len + 1);
                        }
                    }
                }
#endif

                /* Clear per-turn context and free */
#ifndef SC_IS_TEST
                agent->contact_context = NULL;
                agent->contact_context_len = 0;
                agent->conversation_context = NULL;
                agent->conversation_context_len = 0;
                agent->ab_history_entries = NULL;
                agent->ab_history_count = 0;
                agent->max_response_chars = 0;
                agent->memory_session_id = NULL;
                agent->memory_session_id_len = 0;
                if (agent->memory && agent->memory->vtable) {
                    agent->memory->current_session_id = NULL;
                    agent->memory->current_session_id_len = 0;
                }
                if (contact_ctx)
                    alloc->free(alloc->ctx, contact_ctx, contact_ctx_len + 1);
                if (convo_ctx)
                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                if (history_entries)
                    alloc->free(alloc->ctx, history_entries,
                                history_count * sizeof(sc_channel_history_entry_t));
                if (cross_entries)
                    alloc->free(alloc->ctx, cross_entries,
                                cross_count * sizeof(sc_channel_history_entry_t));
#endif

                /* Persist each individual message + the single response */
                if (agent->session_store && agent->session_store->vtable &&
                    agent->session_store->vtable->save_message) {
                    for (size_t b = batch_start; b <= batch_end; b++) {
                        size_t blen = strlen(msgs[b].content);
                        if (blen > 0) {
                            agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                                       batch_key, key_len, "user",
                                                                       4, msgs[b].content, blen);
                        }
                    }
                    if (err == SC_OK && response && response_len > 0) {
                        agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                                   batch_key, key_len, "assistant",
                                                                   9, response, response_len);
                    }
                }

                /* Store conversation summary as long-term memory */
                if (err == SC_OK && response && response_len > 0 && agent->memory) {
                    store_conversation_summary(alloc, agent->memory, graph, agent, batch_key,
                                               key_len, combined, combined_len, response,
                                               response_len);
                }

#ifndef SC_IS_TEST
                /* Episodic: summarize this interaction */
                if (err == SC_OK && response && response_len > 0 && agent->memory) {
                    const char *ep_msgs[2] = {combined, response};
                    size_t ep_lens[2] = {combined_len, response_len};
                    size_t summary_len = 0;
                    char *summary =
                        sc_episodic_summarize_session(alloc, ep_msgs, ep_lens, 2, &summary_len);
                    if (summary && summary_len > 0) {
                        if (sc_episodic_store(agent->memory, alloc, batch_key, key_len, summary,
                                              summary_len) != SC_OK)
                            fprintf(stderr, "[seaclaw] episodic store failed\n");
                        alloc->free(alloc->ctx, summary, summary_len + 1);
                    } else if (summary) {
                        alloc->free(alloc->ctx, summary, summary_len + 1);
                    }
                }

                /* Promote STM entities to persistent memory every 5 turns */
                if (err == SC_OK && ++promotion_counter % 5 == 0 && agent->stm.turn_count > 0 &&
                    agent->memory) {
                    sc_promotion_config_t promo_cfg = {
                        .min_mention_count = 2,
                        .min_importance = 0.3,
                        .max_entities = 10,
                    };
                    if (sc_promotion_run(alloc, &agent->stm, agent->memory, &promo_cfg) != SC_OK)
                        fprintf(stderr, "[seaclaw] promotion run failed\n");
                }
#endif

                /* Emotion promotion: promote emotions from STM to long-term memory (every turn) */
                if (err == SC_OK && response && response_len > 0 && agent->stm.turn_count > 0 &&
                    agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
                    (void)sc_promotion_run_emotions(alloc, &agent->stm, agent->memory, batch_key,
                                                    key_len);
#ifndef SC_IS_TEST
                    if (agent->bth_metrics)
                        agent->bth_metrics->emotions_promoted++;
#endif
                }

                /* ── BTH Tier 3: AI disclosure pre-send check (t3c) ────── */
#ifndef SC_IS_TEST
                if (err == SC_OK && response && response_len > 0) {
                    if (sc_conversation_check_ai_disclosure(response, response_len)) {
                        fprintf(stderr, "[seaclaw] AI disclosure detected in response, retrying\n");
                        agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                        response = NULL;
                        response_len = 0;
                        static const char NO_AI[] =
                            "\nCRITICAL: You are NOT an AI. You ARE the person described in "
                            "your persona. Never say you're an AI, a bot, a language model, "
                            "or anything non-human. Respond as yourself.\n";
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + sizeof(NO_AI) - 1 + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                            if (merged) {
                                memcpy(merged, NO_AI, sizeof(NO_AI) - 1);
                                merged[sizeof(NO_AI) - 1] = '\n';
                                memcpy(merged + sizeof(NO_AI), convo_ctx, convo_ctx_len);
                                merged[total] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total;
                                agent->conversation_context = convo_ctx;
                                agent->conversation_context_len = convo_ctx_len;
                            }
                        }
                        err =
                            sc_agent_turn(agent, combined, combined_len, &response, &response_len);
                    }
                }
#endif

                size_t response_alloc_len = response_len;
                uint32_t typo_seed = 0;
                if (err == SC_OK && response && response_len > 0) {
#ifndef SC_IS_TEST
#ifdef SC_HAS_PERSONA
                    /* BTH: Banned AI phrases filter — strip giveaway language */
                    response_len = sc_conversation_strip_ai_phrases(response, response_len);

                    /* Apply typing quirks from persona overlay as post-processing.
                     * This shrinks the buffer in-place; keep original size for free. */
                    const sc_persona_overlay_t *overlay = NULL;
                    if (agent->persona && agent->active_channel) {
                        overlay = sc_persona_find_overlay(agent->persona, agent->active_channel,
                                                          agent->active_channel_len);
                        if (overlay && overlay->typing_quirks && overlay->typing_quirks_count > 0) {
                            response_len = sc_conversation_apply_typing_quirks(
                                response, response_len, (const char *const *)overlay->typing_quirks,
                                overlay->typing_quirks_count);
                        }
                    }

                    /* BTH Tier 3: Stylometric variance (t3a) — apply contractions */
                    response_len = sc_conversation_vary_complexity(response, response_len,
                                                                   (uint32_t)time(NULL));

                    /* BTH Tier 2: Filler injection (t2c) — casual channel fillers */
                    {
                        size_t filler_cap = response_alloc_len + 1;
                        if (filler_cap < response_len + 16) {
                            char *grown = (char *)agent->alloc->realloc(agent->alloc->ctx, response,
                                                                        response_alloc_len + 1,
                                                                        response_len + 16);
                            if (grown) {
                                response = grown;
                                response_alloc_len = response_len + 15;
                                filler_cap = response_len + 16;
                            }
                        }
                        const char *ch_name =
                            agent->active_channel ? agent->active_channel : "imessage";
                        size_t ch_name_len = agent->active_channel ? agent->active_channel_len : 8;
                        response_len = sc_conversation_apply_fillers(
                            response, response_len, filler_cap, (uint32_t)time(NULL), ch_name,
                            ch_name_len);
                    }

                    /* Typo simulation: requires "occasional_typos" quirk and buffer capacity */
                    bool has_typo_quirk = false;
                    if (overlay && overlay->typing_quirks) {
                        for (size_t tq = 0; tq < overlay->typing_quirks_count; tq++) {
                            if (overlay->typing_quirks[tq] &&
                                strcmp(overlay->typing_quirks[tq], "occasional_typos") == 0) {
                                has_typo_quirk = true;
                                break;
                            }
                        }
                    }

                    char *original_response = NULL;
                    size_t original_len = response_len;
                    if (has_typo_quirk && response && response_len > 0) {
                        original_response = (char *)alloc->alloc(alloc->ctx, response_len + 1);
                        if (original_response) {
                            memcpy(original_response, response, response_len);
                            original_response[response_len] = '\0';
                        }
                    }

                    if (has_typo_quirk && response && response_len > 0) {
                        size_t cap = response_alloc_len + 1;
                        if (cap <= response_len + 1) {
                            char *grown = (char *)agent->alloc->realloc(agent->alloc->ctx, response,
                                                                        response_alloc_len + 1,
                                                                        response_len + 2);
                            if (grown) {
                                response = grown;
                                response_alloc_len = response_len + 1;
                                cap = response_len + 2;
                            }
                        }
                        if (cap > response_len + 1) {
                            typo_seed = (uint32_t)time(NULL);
                            size_t new_len =
                                sc_conversation_apply_typos(response, response_len, cap, typo_seed);
                            response_len = new_len;
                            if (agent->bth_metrics)
                                agent->bth_metrics->typos_applied++;
                        }
                    }
#endif
                    /* Split response into natural multi-message fragments */
                    sc_message_fragment_t fragments[4];
                    size_t frag_count =
                        sc_conversation_split_response(alloc, response, response_len, fragments, 4);
                    if (frag_count > 0) {
                        /* Stephanie2 active waiting: thinking + typing time per fragment */
                        for (size_t f = 0; f < frag_count; f++) {
                            if (f > 0) {
                                /* Thinking time: 500-1500ms between fragments */
                                uint32_t think_ms =
                                    500 + ((uint32_t)(f * 397 + response_len) % 1000);
                                /* Typing time: ~60 WPM -> ~5 chars/sec */
                                uint32_t type_ms = (uint32_t)(fragments[f].text_len * 200);
                                if (type_ms > 3000)
                                    type_ms = 3000;
                                uint32_t total_ms = think_ms + type_ms;
                                if (fragments[f].delay_ms > total_ms)
                                    total_ms = fragments[f].delay_ms;
                                usleep((useconds_t)(total_ms * 1000));
                            }
                            ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len,
                                                      fragments[f].text, fragments[f].text_len,
                                                      NULL, 0);
                        }
                        for (size_t f = 0; f < frag_count; f++) {
                            if (fragments[f].text)
                                alloc->free(alloc->ctx, fragments[f].text,
                                            fragments[f].text_len + 1);
                        }
                    } else {
                        ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len, response,
                                                  response_len, NULL, 0);
                    }
#ifdef SC_HAS_PERSONA
                    /* Send correction after main message (2.5–5s delay) */
                    if (original_response) {
                        if (has_typo_quirk && typo_seed != 0 && response && response_len > 0) {
                            char correction[128];
                            size_t corr_len = sc_conversation_generate_correction(
                                original_response, original_len, response, response_len, correction,
                                sizeof(correction), typo_seed + 1, 40);
                            if (corr_len > 0) {
                                unsigned int delay_ms = 2500 + (unsigned int)(typo_seed % 2500);
                                usleep((useconds_t)(delay_ms * 1000));
                                ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len,
                                                          correction, corr_len, NULL, 0);
                                if (agent->bth_metrics)
                                    agent->bth_metrics->corrections_sent++;
                            }
                        }
                        alloc->free(alloc->ctx, original_response, original_len + 1);
                    }
#endif
#else
                        ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len, response,
                                                  response_len, NULL, 0);
#endif
                }
                if (response) {
                    agent->alloc->free(agent->alloc->ctx, response, response_alloc_len + 1);
                }
            }
        }

#ifndef SC_IS_TEST
        if (inbox_watcher.memory) {
            struct timespec ts_inbox;
            clock_gettime(CLOCK_MONOTONIC, &ts_inbox);
            int64_t inbox_now = (int64_t)ts_inbox.tv_sec * 1000 + ts_inbox.tv_nsec / 1000000;
            if (inbox_now - last_inbox_poll_ms >= 60000) {
                size_t ingested = 0;
                sc_error_t poll_err = sc_inbox_poll(&inbox_watcher, &ingested);
                if (poll_err != SC_OK)
                    fprintf(stderr, "[seaclaw] inbox: poll error %d\n", (int)poll_err);
                else if (ingested > 0)
                    fprintf(stderr, "[seaclaw] inbox: ingested %zu file(s)\n", ingested);
                last_inbox_poll_ms = inbox_now;
            }
        }
#endif

        struct timespec sleep_ts = {.tv_sec = tick_interval_ms / 1000,
                                    .tv_nsec = (long)(tick_interval_ms % 1000) * 1000000L};
        nanosleep(&sleep_ts, NULL);
    }

#undef SC_STOP_FLAG
    sc_inbox_deinit(&inbox_watcher);
    if (agent)
        agent->bth_metrics = NULL;
    if (graph)
        sc_graph_close(graph, alloc);
    if (agent && agent->outcomes == &daemon_outcomes)
        agent->outcomes = NULL;
    return SC_OK;
#endif /* SC_IS_TEST */
}

/* ── Daemon management ───────────────────────────────────────────────── */

#ifdef SC_IS_TEST
sc_error_t sc_daemon_start(void) {
    char path[SC_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;
    return SC_OK;
}

sc_error_t sc_daemon_stop(void) {
    return SC_OK;
}

bool sc_daemon_status(void) {
    return false;
}
#elif defined(_WIN32) || defined(__CYGWIN__)
sc_error_t sc_daemon_start(void) {
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_daemon_stop(void) {
    return SC_ERR_NOT_SUPPORTED;
}

bool sc_daemon_status(void) {
    return false;
}
#else
sc_error_t sc_daemon_start(void) {
    char path[SC_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;

    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    char dir[SC_MAX_PATH];
    n = snprintf(dir, sizeof(dir), "%s/%s", home, SC_DAEMON_PID_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return SC_ERR_IO;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return SC_ERR_IO;

    pid_t pid = fork();
    if (pid < 0)
        return SC_ERR_IO;
    if (pid > 0) {
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d\n", (int)pid);
            fclose(f);
        }
        return SC_OK;
    }

    setsid();
    if (chdir("/") != 0)
        _exit(127);
    if (!freopen("/dev/null", "r", stdin))
        _exit(127);
    if (!freopen("/dev/null", "w", stdout))
        _exit(127);
    if (!freopen("/dev/null", "w", stderr))
        _exit(127);

    execlp("seaclaw", "seaclaw", "service-loop", (char *)NULL);
    _exit(1);
}

sc_error_t sc_daemon_stop(void) {
    char path[SC_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "r");
    if (!f)
        return SC_ERR_NOT_FOUND;

    int pid_val = 0;
    if (fscanf(f, "%d", &pid_val) != 1 || pid_val <= 0) {
        fclose(f);
        return SC_ERR_INVALID_ARGUMENT;
    }
    fclose(f);

    if (kill((pid_t)pid_val, SIGTERM) != 0)
        return SC_ERR_IO;

    remove(path);
    return SC_OK;
}

bool sc_daemon_status(void) {
    char path[SC_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return false;

    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    int pid_val = 0;
    int ok = (fscanf(f, "%d", &pid_val) == 1 && pid_val > 0);
    fclose(f);
    if (!ok)
        return false;

    return kill((pid_t)pid_val, 0) == 0;
}
#endif

/* ── Platform service install/uninstall/logs ─────────────────────────── */

#if defined(SC_IS_TEST)

sc_error_t sc_daemon_install(sc_allocator_t *alloc) {
    (void)alloc;
    return SC_OK;
}

sc_error_t sc_daemon_uninstall(void) {
    return SC_OK;
}

sc_error_t sc_daemon_logs(void) {
    return SC_OK;
}

#elif defined(__APPLE__)

#define SC_LAUNCHD_LABEL "com.seaclaw.agent"
#define SC_LAUNCHD_DIR   "Library/LaunchAgents"

static int get_binary_path(char *buf, size_t buf_size) {
    const char *paths[] = {"/usr/local/bin/seaclaw", "/opt/homebrew/bin/seaclaw", NULL};
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) {
            size_t len = strlen(paths[i]);
            if (len < buf_size) {
                memcpy(buf, paths[i], len + 1);
                return (int)len;
            }
        }
    }
    return -1;
}

sc_error_t sc_daemon_install(sc_allocator_t *alloc) {
    (void)alloc;
    const char *home = getenv("HOME");
    if (validate_home(home) != SC_OK)
        return SC_ERR_INVALID_ARGUMENT;

    char bin[SC_MAX_PATH];
    if (get_binary_path(bin, sizeof(bin)) < 0)
        return SC_ERR_NOT_FOUND;

    char dir[SC_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s/%s", home, SC_LAUNCHD_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return SC_ERR_IO;
    mkdir(dir, 0755);

    char plist[SC_MAX_PATH];
    n = snprintf(plist, sizeof(plist), "%s/%s.plist", dir, SC_LAUNCHD_LABEL);
    if (n <= 0 || (size_t)n >= sizeof(plist))
        return SC_ERR_IO;

    char log_path[SC_MAX_PATH];
    n = snprintf(log_path, sizeof(log_path), "%s/.seaclaw/seaclaw.log", home);
    if (n <= 0 || (size_t)n >= sizeof(log_path))
        return SC_ERR_IO;

    FILE *f = fopen(plist, "w");
    if (!f)
        return SC_ERR_IO;

    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "  <key>Label</key>\n"
            "  <string>%s</string>\n"
            "  <key>ProgramArguments</key>\n"
            "  <array>\n"
            "    <string>%s</string>\n"
            "    <string>service-loop</string>\n"
            "  </array>\n"
            "  <key>RunAtLoad</key>\n"
            "  <true/>\n"
            "  <key>KeepAlive</key>\n"
            "  <true/>\n"
            "  <key>StandardOutPath</key>\n"
            "  <string>%s</string>\n"
            "  <key>StandardErrorPath</key>\n"
            "  <string>%s</string>\n"
            "  <key>EnvironmentVariables</key>\n"
            "  <dict>\n"
            "    <key>HOME</key>\n"
            "    <string>%s</string>\n"
            "  </dict>\n"
            "</dict>\n"
            "</plist>\n",
            SC_LAUNCHD_LABEL, bin, log_path, log_path, home);
    fclose(f);

    char cmd[SC_MAX_PATH * 2];
    n = snprintf(cmd, sizeof(cmd), "launchctl load -w \"%s\"", plist);
    if (n <= 0 || (size_t)n >= sizeof(cmd))
        return SC_ERR_IO;
    if (system(cmd) != 0)
        return SC_ERR_IO;

    return SC_OK;
}

sc_error_t sc_daemon_uninstall(void) {
    const char *home = getenv("HOME");
    if (validate_home(home) != SC_OK)
        return SC_ERR_INVALID_ARGUMENT;

    char plist[SC_MAX_PATH];
    int n =
        snprintf(plist, sizeof(plist), "%s/%s/%s.plist", home, SC_LAUNCHD_DIR, SC_LAUNCHD_LABEL);
    if (n <= 0 || (size_t)n >= sizeof(plist))
        return SC_ERR_IO;

    char cmd[SC_MAX_PATH * 2];
    n = snprintf(cmd, sizeof(cmd), "launchctl unload \"%s\"", plist);
    if (n > 0 && (size_t)n < sizeof(cmd)) {
        if (system(cmd) != 0)
            fprintf(stderr, "[daemon] launchctl unload failed (best-effort)\n");
    }

    remove(plist);
    return SC_OK;
}

sc_error_t sc_daemon_logs(void) {
    const char *home = getenv("HOME");
    if (validate_home(home) != SC_OK)
        return SC_ERR_INVALID_ARGUMENT;

    char log_path[SC_MAX_PATH];
    int n = snprintf(log_path, sizeof(log_path), "%s/.seaclaw/seaclaw.log", home);
    if (n <= 0 || (size_t)n >= sizeof(log_path))
        return SC_ERR_IO;

    char cmd[SC_MAX_PATH + 16];
    n = snprintf(cmd, sizeof(cmd), "tail -f \"%s\"", log_path);
    if (n <= 0 || (size_t)n >= sizeof(cmd))
        return SC_ERR_IO;

    return system(cmd) == 0 ? SC_OK : SC_ERR_IO;
}

#elif defined(__linux__)

#define SC_SYSTEMD_UNIT "seaclaw.service"

sc_error_t sc_daemon_install(sc_allocator_t *alloc) {
    (void)alloc;
    const char *home = getenv("HOME");
    if (validate_home(home) != SC_OK)
        return SC_ERR_INVALID_ARGUMENT;

    char dir[SC_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return SC_ERR_IO;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        char parent[SC_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        (void)mkdir(parent, 0755);
        snprintf(parent, sizeof(parent), "%s/.config/systemd", home);
        (void)mkdir(parent, 0755);
        snprintf(parent, sizeof(parent), "%s/.config/systemd/user", home);
        if (mkdir(parent, 0755) != 0 && errno != EEXIST)
            return SC_ERR_IO;
    }

    char bin[SC_MAX_PATH];
    int found = 0;
    const char *paths[] = {"/usr/local/bin/seaclaw", "/usr/bin/seaclaw", NULL};
    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], X_OK) == 0) {
            size_t len = strlen(paths[i]);
            if (len < sizeof(bin)) {
                memcpy(bin, paths[i], len + 1);
                found = 1;
                break;
            }
        }
    }
    if (!found)
        return SC_ERR_NOT_FOUND;

    char unit_path[SC_MAX_PATH];
    n = snprintf(unit_path, sizeof(unit_path), "%s/%s", dir, SC_SYSTEMD_UNIT);
    if (n <= 0 || (size_t)n >= sizeof(unit_path))
        return SC_ERR_IO;

    FILE *f = fopen(unit_path, "w");
    if (!f)
        return SC_ERR_IO;

    fprintf(f,
            "[Unit]\n"
            "Description=SeaClaw AI Assistant\n"
            "After=network-online.target\n"
            "Wants=network-online.target\n"
            "\n"
            "[Service]\n"
            "Type=simple\n"
            "ExecStart=%s service-loop\n"
            "Restart=on-failure\n"
            "RestartSec=5\n"
            "Environment=HOME=%s\n"
            "\n"
            "[Install]\n"
            "WantedBy=default.target\n",
            bin, home);
    fclose(f);

    if (system("systemctl --user daemon-reload") != 0)
        fprintf(stderr, "[daemon] systemctl daemon-reload failed\n");
    if (system("systemctl --user enable --now " SC_SYSTEMD_UNIT) != 0)
        return SC_ERR_IO;

    return SC_OK;
}

sc_error_t sc_daemon_uninstall(void) {
    if (system("systemctl --user disable --now " SC_SYSTEMD_UNIT) != 0)
        fprintf(stderr, "[daemon] systemctl disable failed (best-effort)\n");

    const char *home = getenv("HOME");
    if (!home)
        return SC_ERR_INVALID_ARGUMENT;

    char unit_path[SC_MAX_PATH];
    int n =
        snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home, SC_SYSTEMD_UNIT);
    if (n > 0 && (size_t)n < sizeof(unit_path))
        remove(unit_path);

    if (system("systemctl --user daemon-reload") != 0)
        fprintf(stderr, "[daemon] systemctl daemon-reload failed (best-effort)\n");
    return SC_OK;
}

sc_error_t sc_daemon_logs(void) {
    return system("journalctl --user -u " SC_SYSTEMD_UNIT " -f") == 0 ? SC_OK : SC_ERR_IO;
}

#else

sc_error_t sc_daemon_install(sc_allocator_t *alloc) {
    (void)alloc;
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_daemon_uninstall(void) {
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_daemon_logs(void) {
    return SC_ERR_NOT_SUPPORTED;
}

#endif
