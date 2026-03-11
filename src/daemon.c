#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "human/daemon.h"
#include "human/agent.h"
#include "human/agent/anticipatory.h"
#include "human/agent/collab_planning.h"
#include "human/agent/conversation_plan.h"
#include "human/agent/episodic.h"
#include "human/agent/info_asymmetry.h"
#include "human/agent/outcomes.h"
#include "human/agent/proactive.h"
#include "human/agent/theory_of_mind.h"
#include "human/config.h"
#include "human/context/conversation.h"
#include "human/context/event_extract.h"
#include "human/context/style_tracker.h"
#include "human/context/mood.h"
#include "human/context/vision.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/memory/comfort_patterns.h"
#include "human/memory/consolidation.h"
#include "human/memory/consolidation_engine.h"
#include "human/memory/deep_extract.h"
#include "human/memory/episodic.h"
#include "human/memory/emotional_residue.h"
#include "human/memory/forgetting_curve.h"
#include "human/memory/prospective.h"
#include "human/memory/emotional_graph.h"
#include "human/memory/emotional_moments.h"
#include "human/memory/fast_capture.h"
#include "human/memory/graph.h"
#include "human/memory/inbox.h"
#include "human/memory/promotion.h"
#include "human/memory/retrieval.h"
#include "human/memory/superhuman.h"
#include "human/observability/bth_metrics.h"
#include "human/persona/voice_maturity.h"
#include "human/platform.h"
#include "human/platform/calendar.h"
#include <stdlib.h>
#ifdef HU_HAS_PERSONA
#include "human/persona.h"
#include "human/persona/auto_profile.h"
#include "human/persona/auto_tune.h"
#include "human/persona/replay.h"
#include "human/persona/life_sim.h"
#include "human/persona/mood.h"
#include "human/context/theory_of_mind.h"
#include "human/context/anticipatory.h"
#include "human/context/self_awareness.h"
#include "human/memory/life_chapters.h"
#include "human/context/social_graph.h"
#include "human/context/humor.h"
#include "human/memory/degradation.h"
#include "human/context/protective.h"
#endif
#ifdef HU_HAS_CRON
#include "human/cron.h"
#include "human/crontab.h"
#endif
#if defined(HU_ENABLE_IMESSAGE) && !defined(HU_IS_TEST)
#include "human/channels/imessage.h"
#endif
#if defined(HU_ENABLE_CARTESIA)
#include "human/context/voice_decision.h"
#include "human/tts/cartesia.h"
#include "human/tts/audio_pipeline.h"
#include "human/tts/emotion_map.h"
#endif
#include <ctype.h>
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

#define HU_DAEMON_PID_DIR  ".human"
#define HU_DAEMON_PID_FILE "human.pid"
#define HU_MAX_PATH        1024

/* GCC's warn_unused_result is not suppressed by (void) casts */
#define HU_IGNORE_RESULT(expr)   \
    do {                         \
        if ((expr)) { /* noop */ \
        }                        \
    } while (0)

#ifndef HU_IS_TEST
/* Build conversation callback context from memory.
 * Queries memory for past topics relevant to the current message.
 * F61: Memory content passed through hu_memory_degradation_apply before injection.
 * F68: Memory filtered via hu_protective_memory_ok before injection.
 * Returns an allocated string (caller frees) or NULL. */
static char *build_callback_context(hu_allocator_t *alloc, hu_memory_t *memory,
                                    const char *session_id, size_t session_id_len, const char *msg,
                                    size_t msg_len, size_t *out_len, hu_agent_t *agent) {
    *out_len = 0;
    if (!memory || !memory->vtable || !memory->vtable->recall || !msg || msg_len == 0)
        return NULL;

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = memory->vtable->recall(memory->ctx, alloc, msg, msg_len, 3, session_id,
                                            session_id_len, &entries, &count);
    if (err != HU_OK || !entries || count == 0)
        return NULL;

    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *lt = hu_platform_localtime_r(&now, &tm_buf);
    int hour_local = lt ? lt->tm_hour : 12;
    float deg_rate = 0.10f;
#ifdef HU_HAS_PERSONA
    if (agent && agent->persona && agent->persona->memory_degradation_rate > 0.f)
        deg_rate = agent->persona->memory_degradation_rate;
#endif

    char buf[2048];
    size_t pos = 0;
    int w = snprintf(buf, sizeof(buf), "\nCONTEXT FROM YOUR SHARED HISTORY:\n");
    if (w > 0)
        pos = (size_t)w;

    size_t usable = 0;
    for (size_t i = 0; i < count && i < 3; i++) {
        if (!entries[i].content || entries[i].content_len == 0)
            continue;
#ifdef HU_HAS_PERSONA
        if (agent && !hu_protective_memory_ok(alloc, memory, session_id, session_id_len,
                                             entries[i].content, entries[i].content_len,
                                             0.0f, hour_local))
            continue;
#endif
        const char *content = entries[i].content;
        size_t content_len = entries[i].content_len;
        char *degraded = NULL;
        size_t degraded_len = 0;
#ifdef HU_HAS_PERSONA
        uint32_t seed = (uint32_t)now * 1103515245u + 12345u + (uint32_t)i;
        degraded = hu_memory_degradation_apply(alloc, content, content_len, seed, deg_rate,
                                              &degraded_len);
        if (degraded && degraded_len > 0) {
            content = degraded;
            content_len = degraded_len;
        }
#endif
        size_t show = content_len;
        if (show > 200)
            show = 200;
        w = snprintf(buf + pos, sizeof(buf) - pos, "%.*s\n", (int)show, content);
        if (degraded)
            alloc->free(alloc->ctx, degraded, degraded_len + 1);
        if (w > 0 && pos + (size_t)w < sizeof(buf)) {
            pos += (size_t)w;
            usable++;
        }
    }

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));

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

#ifndef HU_IS_TEST
/* F27: Classify our response type for comfort pattern learning.
 * Heuristic: haha/lol/joke -> distraction; sorry/i understand/that sucks -> empathy;
 * very short (<20 chars) -> space; you should/try this/maybe -> advice; default empathy. */
static void classify_comfort_response_type(const char *response, size_t response_len,
                                           char *out_type, size_t out_cap) {
    if (!response || !out_type || out_cap < 8)
        return;
    out_type[0] = '\0';
    if (response_len < 20) {
        snprintf(out_type, out_cap, "space");
        return;
    }
    char lower[256];
    size_t copy = response_len < sizeof(lower) - 1 ? response_len : sizeof(lower) - 1;
    for (size_t i = 0; i < copy; i++) {
        char c = response[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[copy] = '\0';
    if (strstr(lower, "haha") || strstr(lower, "lol") || strstr(lower, "hah ") ||
        strstr(lower, " joke") || strstr(lower, "funny")) {
        snprintf(out_type, out_cap, "distraction");
        return;
    }
    if (strstr(lower, "you should") || strstr(lower, "try this") || strstr(lower, "maybe ") ||
        strstr(lower, "have you tried") || strstr(lower, "i'd suggest")) {
        snprintf(out_type, out_cap, "advice");
        return;
    }
    if (strstr(lower, "sorry") || strstr(lower, "i understand") || strstr(lower, "that sucks") ||
        strstr(lower, "i hear you") || strstr(lower, "that must be")) {
        snprintf(out_type, out_cap, "empathy");
        return;
    }
    snprintf(out_type, out_cap, "empathy");
}

#ifdef HU_ENABLE_SQLITE
/* F23: Extract significant topic keywords from user text and record baselines.
 * Skips stopwords, records each significant word (3–32 chars) via topic_baselines. */
#define HU_DAEMON_TOPIC_BASELINE_MAX 8
#define HU_DAEMON_TOPIC_BUF 32

static void record_topic_baselines_from_text(hu_memory_t *memory, const char *contact_id,
                                              size_t contact_id_len, const char *text,
                                              size_t text_len) {
    if (!memory || !contact_id || contact_id_len == 0 || !text || text_len == 0)
        return;
    static const char *const stop[] = {
        "i", "the", "a", "is", "was", "that", "this", "it", "to", "and", "but", "so",
        "just", "really", "what", "how", "why", "when", "where", "who", "can", "will",
        "would", "could", "should", "have", "has", "had", "do", "does", "did", "am",
        "are", "were", "be", "been", "being", "of", "in", "on", "at", "for", "with",
        "about", "from", "as", "or", "if", "not", "no", "yes", "oh", "um", "like",
        NULL,
    };
    char topics[HU_DAEMON_TOPIC_BASELINE_MAX][HU_DAEMON_TOPIC_BUF];
    size_t topic_count = 0;
    memset(topics, 0, sizeof(topics));

    const char *p = text;
    const char *end = text + text_len;
    while (p < end && topic_count < HU_DAEMON_TOPIC_BASELINE_MAX) {
        while (p < end && !isalnum((unsigned char)*p))
            p++;
        if (p >= end)
            break;
        const char *start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '\'' || *p == '-'))
            p++;
        size_t wlen = (size_t)(p - start);
        if (wlen < 3 || wlen >= HU_DAEMON_TOPIC_BUF - 1)
            continue;
        bool is_stop = false;
        for (const char *const *sw = stop; *sw; sw++) {
            size_t swlen = strlen(*sw);
            if (wlen == swlen && strncasecmp(start, *sw, wlen) == 0) {
                is_stop = true;
                break;
            }
        }
        if (is_stop)
            continue;
        /* Dedupe */
        bool dup = false;
        for (size_t k = 0; k < topic_count; k++) {
            if (strncasecmp(topics[k], start, wlen) == 0 && topics[k][wlen] == '\0') {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        memcpy(topics[topic_count], start, wlen);
        topics[topic_count][wlen] = '\0';
        for (size_t i = 0; i < wlen; i++)
            topics[topic_count][i] = (char)tolower((unsigned char)topics[topic_count][i]);
        (void)hu_superhuman_topic_baseline_record(memory, contact_id, contact_id_len,
                                                  topics[topic_count], wlen);
        topic_count++;
    }
}
#endif

/* F27: Score engagement from their reply. reply_len>20 + positive words -> 0.8;
 * brief thanks -> 0.4; very short -> 0.2. */
static float score_comfort_engagement(const char *reply, size_t reply_len) {
    if (!reply)
        return 0.2f;
    if (reply_len <= 5)
        return 0.2f;
    char lower[256];
    size_t copy = reply_len < sizeof(lower) - 1 ? reply_len : sizeof(lower) - 1;
    for (size_t i = 0; i < copy; i++) {
        char c = reply[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    lower[copy] = '\0';
    if (strstr(lower, "thanks") || strstr(lower, "thank you") || strstr(lower, "ty ") ||
        strstr(lower, "thx")) {
        if (reply_len > 20)
            return 0.8f;
        return 0.4f;
    }
    if (strstr(lower, "yes") || strstr(lower, "yeah") || strstr(lower, "that helped") ||
        strstr(lower, "good point") || strstr(lower, "makes sense")) {
        return 0.8f;
    }
    if (reply_len > 20)
        return 0.6f;
    return 0.3f;
}
#endif

/* Store a conversation summary as long-term memory.
 * Concatenates the user message and agent response, runs deep-extract
 * on the full exchange, and stores extracted facts scoped to the contact.
 * When graph is non-NULL, also upserts facts and relations into the GraphRAG knowledge graph.
 * agent may be NULL; when non-NULL and bth_metrics set, increments facts_extracted. */
static void store_conversation_summary(hu_allocator_t *alloc, hu_memory_t *memory,
                                       hu_graph_t *graph, hu_agent_t *agent, const char *session_id,
                                       size_t session_id_len, const char *user_msg,
                                       size_t user_msg_len, const char *response,
                                       size_t response_len) {
    if (!alloc || !memory || !memory->vtable || !memory->vtable->store)
        return;
    if (!user_msg || user_msg_len == 0)
        return;
#ifndef HU_ENABLE_SQLITE
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

    hu_deep_extract_result_t de;
    memset(&de, 0, sizeof(de));
    hu_error_t err = hu_deep_extract_lightweight(alloc, combined, combined_len, &de);
    if (err == HU_OK && de.fact_count > 0) {
        if (agent && agent->bth_metrics)
            agent->bth_metrics->facts_extracted += de.fact_count;
        static const char cat_name[] = "conversation_summary";
        hu_memory_category_t cat = {
            .tag = HU_MEMORY_CATEGORY_CUSTOM,
            .data.custom = {.name = cat_name, .name_len = sizeof(cat_name) - 1},
        };
        for (size_t i = 0; i < de.fact_count; i++) {
            const hu_extracted_fact_t *f = &de.facts[i];
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
#ifdef HU_ENABLE_SQLITE
            if (graph) {
                int64_t src_id = 0;
                int64_t tgt_id = 0;
                size_t subj_len = strlen(f->subject);
                size_t obj_len = strlen(f->object);
                hu_relation_type_t rel_type =
                    hu_relation_type_from_string(f->predicate, strlen(f->predicate));
                if (hu_graph_upsert_entity(graph, f->subject, subj_len, HU_ENTITY_UNKNOWN, NULL,
                                           &src_id) == HU_OK &&
                    hu_graph_upsert_entity(graph, f->object, obj_len, HU_ENTITY_UNKNOWN, NULL,
                                           &tgt_id) == HU_OK) {
                    hu_error_t rel_err = hu_graph_upsert_relation(graph, src_id, tgt_id, rel_type,
                                                                  1.0f, f->object, obj_len);
                    if (rel_err != HU_OK)
                        fprintf(stderr, "[daemon] graph: relation upsert failed: %d\n",
                                (int)rel_err);
                }
            }
#endif
        }
    }
#ifdef HU_ENABLE_SQLITE
    if (graph && err == HU_OK && de.relation_count > 0) {
        for (size_t i = 0; i < de.relation_count; i++) {
            const hu_extracted_relation_t *r = &de.relations[i];
            if (!r->entity_a || !r->relation || !r->entity_b)
                continue;
            int64_t src_id = 0;
            int64_t tgt_id = 0;
            size_t a_len = strlen(r->entity_a);
            size_t b_len = strlen(r->entity_b);
            size_t rel_len = strlen(r->relation);
            hu_relation_type_t rel_type = hu_relation_type_from_string(r->relation, rel_len);
            if (hu_graph_upsert_entity(graph, r->entity_a, a_len, HU_ENTITY_UNKNOWN, NULL,
                                       &src_id) == HU_OK &&
                hu_graph_upsert_entity(graph, r->entity_b, b_len, HU_ENTITY_UNKNOWN, NULL,
                                       &tgt_id) == HU_OK) {
                hu_error_t rel_err = hu_graph_upsert_relation(graph, src_id, tgt_id, rel_type, 1.0f,
                                                              r->entity_b, b_len);
                if (rel_err != HU_OK)
                    fprintf(stderr, "[daemon] graph: relation upsert failed: %d\n", (int)rel_err);
            }
        }
    }
#endif
    hu_deep_extract_result_deinit(&de, alloc);
    alloc->free(alloc->ctx, combined, total + 1);
}
#endif /* !HU_IS_TEST */

static hu_error_t validate_home(const char *home) {
    if (!home || !home[0]) {
        fprintf(stderr, "HOME not set\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    for (const char *p = home; *p; p++) {
        char c = *p;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '/' && c != '.' && c != '_' && c != '-' && c != ' ') {
            fprintf(stderr, "HOME contains unsafe characters\n");
            return HU_ERR_INVALID_ARGUMENT;
        }
    }
    return HU_OK;
}

static int get_pid_path(char *buf, size_t buf_size) {
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    if (validate_home(home) != HU_OK)
        return -1;
    return snprintf(buf, buf_size, "%s/%s/%s", home, HU_DAEMON_PID_DIR, HU_DAEMON_PID_FILE);
}

/* ── Cron field parsing and tick (only when HU_HAS_CRON) ────────────── */
#ifdef HU_HAS_CRON

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

bool hu_cron_schedule_matches(const char *schedule, const struct tm *tm) {
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

static void run_cron_tick(hu_allocator_t *alloc) {
    char *cron_path = NULL;
    size_t cron_path_len = 0;
    if (hu_crontab_get_path(alloc, &cron_path, &cron_path_len) != HU_OK)
        return;

    hu_crontab_entry_t *entries = NULL;
    size_t count = 0;
    if (hu_crontab_load(alloc, cron_path, &entries, &count) != HU_OK || count == 0) {
        alloc->free(alloc->ctx, cron_path, cron_path_len + 1);
        if (entries)
            hu_crontab_entries_free(alloc, entries, count);
        return;
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    for (size_t i = 0; i < count; i++) {
        if (!entries[i].enabled || !entries[i].command)
            continue;
        if (!hu_cron_schedule_matches(entries[i].schedule, &tm))
            continue;

#ifndef HU_IS_TEST
        const char *argv[] = {"/bin/sh", "-c", entries[i].command, NULL};
        hu_run_result_t result = {0};
        hu_error_t run_err = hu_process_run(alloc, argv, NULL, 65536, &result);
        if (run_err != HU_OK)
            fprintf(stderr, "[human] cron job failed: %s (err=%d)\n", entries[i].command,
                    (int)run_err);
        hu_run_result_free(alloc, &result);
#endif
    }

    hu_crontab_entries_free(alloc, entries, count);
    alloc->free(alloc->ctx, cron_path, cron_path_len + 1);
}

hu_error_t hu_service_run_agent_cron(hu_allocator_t *alloc, hu_agent_t *agent,
                                     hu_service_channel_t *channels, size_t channel_count) {
    if (!alloc || !agent || !agent->scheduler)
        return HU_ERR_INVALID_ARGUMENT;

    size_t job_count = 0;
    const hu_cron_job_t *jobs = hu_cron_list_jobs(agent->scheduler, &job_count);
    if (!jobs || job_count == 0)
        return HU_OK;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    for (size_t i = 0; i < job_count; i++) {
        if (jobs[i].type != HU_CRON_JOB_AGENT || !jobs[i].enabled || jobs[i].paused)
            continue;
        if (!jobs[i].command || !jobs[i].expression)
            continue;
        if (!hu_cron_schedule_matches(jobs[i].expression, &tm))
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
#ifndef HU_IS_TEST
        hu_error_t err = hu_agent_turn(agent, prompt, prompt_len, &response, &response_len);
#else
        (void)prompt_len;
        hu_error_t err = HU_OK;
        response = hu_strndup(alloc, "[agent-cron-test]", 17);
        response_len = 17;
#endif

        agent->active_job_id = 0;
        if (err == HU_OK && response && response_len > 0 && target_channel && channels) {
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

        (void)hu_cron_add_run(agent->scheduler, alloc, jobs[i].id, (int64_t)now,
                              err == HU_OK ? "success" : "failed", response);

        if (response)
            agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
        hu_agent_clear_history(agent);
    }
    return HU_OK;
}

#endif /* HU_HAS_CRON */

/* ── Proactive check-in ──────────────────────────────────────────────── */

#if defined(HU_HAS_PERSONA) && !defined(HU_IS_TEST)

static char *proactive_prompt_for_contact(hu_allocator_t *alloc, hu_agent_t *agent,
                                          hu_memory_t *memory, const hu_contact_profile_t *cp,
                                          size_t *out_len) {
    char *starter = NULL;
    size_t starter_len = 0;
    if (memory && cp->contact_id) {
        (void)hu_proactive_build_starter(alloc, memory, cp->contact_id, strlen(cp->contact_id),
                                         &starter, &starter_len);
    }

    /* Memory-informed topics: recall recent memories about the contact */
    char *mem_ctx = NULL;
    size_t mem_ctx_len = 0;
    if (memory && cp->contact_id) {
        mem_ctx = build_callback_context(alloc, memory, cp->contact_id, strlen(cp->contact_id),
                                         "recent conversation topics", 24, &mem_ctx_len, agent);
    }

    /* Calendar awareness: inject today's events when calendar_enabled */
    char *calendar_ctx = NULL;
    size_t calendar_ctx_len = 0;
    if (agent && agent->persona && agent->persona->context_awareness.calendar_enabled) {
#if defined(__APPLE__)
        char *events_json = NULL;
        size_t events_len = 0;
        if (hu_calendar_macos_get_events(alloc, 24, &events_json, &events_len) == HU_OK &&
            events_json && events_len > 2) {
            size_t prefix_len = sizeof("Your calendar today: ") - 1;
            size_t suffix_len =
                sizeof(". Use for context (e.g. 'in meetings', 'had dentist appointment').") - 1;
            calendar_ctx_len = prefix_len + events_len + suffix_len;
            calendar_ctx = (char *)alloc->alloc(alloc->ctx, calendar_ctx_len + 1);
            if (calendar_ctx) {
                memcpy(calendar_ctx, "Your calendar today: ", prefix_len);
                memcpy(calendar_ctx + prefix_len, events_json, events_len);
                memcpy(calendar_ctx + prefix_len + events_len,
                       ". Use for context (e.g. 'in meetings', 'had dentist appointment').",
                       suffix_len + 1);
                alloc->free(alloc->ctx, events_json, events_len + 1);
            } else {
                alloc->free(alloc->ctx, events_json, events_len + 1);
                calendar_ctx_len = 0;
            }
        } else if (events_json) {
            alloc->free(alloc->ctx, events_json, events_len + 1);
        }
#endif
    }

    static const char HU_DEFAULT_PROACTIVE_RULES[] =
        "\nRules: "
        "1. One short natural message (not 'hey how are you' — too generic). "
        "2. Reference something specific you know about them or ask about "
        "something from a previous conversation. "
        "3. Keep it under 10 words. "
        "4. If you have nothing specific, share something you saw/did "
        "that made you think of them. "
        "5. Reply SKIP if you genuinely have nothing natural to say.";
    const char *rules = (agent && agent->persona && agent->persona->proactive_rules)
                            ? agent->persona->proactive_rules
                            : HU_DEFAULT_PROACTIVE_RULES;
    size_t rules_len = (agent && agent->persona && agent->persona->proactive_rules)
                           ? strlen(rules)
                           : sizeof(HU_DEFAULT_PROACTIVE_RULES) - 1;

    char base_buf[256];
    int w = snprintf(base_buf, sizeof(base_buf), "You're initiating a casual check-in text to %s. ",
                     cp->name ? cp->name : "this person");
    size_t base_len = (w > 0 && (size_t)w < sizeof(base_buf)) ? (size_t)w : 0;

    size_t total = base_len + rules_len;
    if (starter && starter_len > 0)
        total += 2 + starter_len; /* "\n\n" + starter */
    if (mem_ctx && mem_ctx_len > 0)
        total += 2 + mem_ctx_len; /* "\n\n" + mem_ctx */
    if (calendar_ctx && calendar_ctx_len > 0)
        total += 2 + calendar_ctx_len; /* "\n\n" + calendar_ctx */

    char *result = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!result) {
        if (starter)
            alloc->free(alloc->ctx, starter, starter_len + 1);
        if (mem_ctx)
            alloc->free(alloc->ctx, mem_ctx, mem_ctx_len + 1);
        if (calendar_ctx)
            alloc->free(alloc->ctx, calendar_ctx, calendar_ctx_len + 1);
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
    if (calendar_ctx && calendar_ctx_len > 0) {
        result[pos++] = '\n';
        result[pos++] = '\n';
        memcpy(result + pos, calendar_ctx, calendar_ctx_len);
        pos += calendar_ctx_len;
        alloc->free(alloc->ctx, calendar_ctx, calendar_ctx_len + 1);
    }

    memcpy(result + pos, rules, rules_len);
    pos += rules_len;
    result[pos] = '\0';
    *out_len = pos;
    return result;
}

void hu_service_run_proactive_checkins(hu_allocator_t *alloc, hu_agent_t *agent,
                                       hu_service_channel_t *channels, size_t channel_count) {
    if (!alloc || !agent || !agent->persona)
        return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int hour = tm_now.tm_hour;

    /* Only check in during social hours (9am-9pm) */
    if (hour < 9 || hour > 21)
        return;

    /* F53: Deduplication — don't send same important_date to same contact twice per day */
    static char g_sent_important_date_contacts[8][64];
    static int g_sent_important_date_count = 0;
    static int g_sent_important_date_ymd = -1;
    int today_ymd = (tm_now.tm_year + 1900) * 10000 + (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
    if (g_sent_important_date_ymd != today_ymd) {
        g_sent_important_date_ymd = today_ymd;
        g_sent_important_date_count = 0;
    }

#ifndef HU_IS_TEST
    /* F25: Emotional check-ins — due moments from 1–3 days ago */
    if (agent->memory) {
        hu_emotional_moment_t *due = NULL;
        size_t due_count = 0;
        if (hu_emotional_moment_get_due(alloc, agent->memory, (int64_t)now, &due, &due_count) ==
                HU_OK &&
            due && due_count > 0) {
            for (size_t d = 0; d < due_count; d++) {
                const hu_emotional_moment_t *m = &due[d];
                /* Find contact and channel for this contact_id */
                for (size_t i = 0; i < agent->persona->contacts_count; i++) {
                    const hu_contact_profile_t *cp = &agent->persona->contacts[i];
                    if (!cp->proactive_checkin || !cp->proactive_channel || !cp->contact_id)
                        continue;
                    bool match = (strcmp(cp->contact_id, m->contact_id) == 0);
                    if (!match) {
                        const char *colon = strchr(cp->proactive_channel, ':');
                        if (colon && strcmp(colon + 1, m->contact_id) == 0)
                            match = true;
                        else if (strcmp(cp->proactive_channel, m->contact_id) == 0)
                            match = true;
                    }
                    if (!match)
                        continue;

                    const char *ch_part = cp->proactive_channel;
                    const char *target_part = cp->contact_id;
                    size_t target_len = strlen(target_part);
                    char ch_buf[64] = {0};
                    const char *colon = strchr(cp->proactive_channel, ':');
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

                    for (size_t c = 0; c < channel_count; c++) {
                        if (!channels[c].channel || !channels[c].channel->vtable ||
                            !channels[c].channel->vtable->name)
                            continue;
                        const char *ch_name =
                            channels[c].channel->vtable->name(channels[c].channel->ctx);
                        if (!ch_name || strcmp(ch_name, ch_part) != 0)
                            continue;
                        if (!channels[c].channel->vtable->send)
                            break;

                        char msg_buf[384];
                        int w = snprintf(msg_buf, sizeof(msg_buf), "hey how are you doing with %s?",
                                         m->topic);
                        if (w > 0 && (size_t)w < sizeof(msg_buf)) {
                            hu_error_t send_err = channels[c].channel->vtable->send(
                                channels[c].channel->ctx, target_part, target_len, msg_buf,
                                (size_t)w, NULL, 0);
                            if (send_err == HU_OK) {
                                (void)hu_emotional_moment_mark_followed_up(agent->memory, m->id);
                                fprintf(stderr, "[human] F25 emotional check-in sent to %s: %s\n",
                                        cp->name ? cp->name : cp->contact_id, msg_buf);
                            }
                        }
                        break;
                    }
                    break;
                }
            }
            alloc->free(alloc->ctx, due, due_count * sizeof(hu_emotional_moment_t));
        }
    }
#endif

    for (size_t i = 0; i < agent->persona->contacts_count; i++) {
        const hu_contact_profile_t *cp = &agent->persona->contacts[i];
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

#ifdef HU_ENABLE_SQLITE
        /* F64: Anticipatory predictions — run for each contact in proactive cycle */
        if (agent->memory) {
            hu_emotional_prediction_t *preds = NULL;
            size_t pred_count = 0;
            (void)hu_anticipatory_predict(alloc, agent->memory, cp->contact_id,
                                         strlen(cp->contact_id), (int64_t)now, &preds, &pred_count);
            if (preds)
                hu_anticipatory_predictions_free(alloc, preds, pred_count);
        }
#endif

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

            hu_channel_history_entry_t *entries = NULL;
            size_t entry_count = 0;
            hu_error_t hist_err = channels[c].channel->vtable->load_conversation_history(
                channels[c].channel->ctx, alloc, cp->contact_id, strlen(cp->contact_id), 15,
                &entries, &entry_count);
            if (hist_err != HU_OK)
                fprintf(stderr, "[daemon] proactive: history load failed for %s: %d\n",
                        cp->contact_id, (int)hist_err);

            uint64_t last_contact_ms = 0;
            bool should_checkin = true;
            if (hist_err == HU_OK && entries && entry_count > 0) {
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

#ifndef HU_IS_TEST
            /* Silence check-in: per-contact last activity from conversation history */
            {
                hu_silence_config_t silence_cfg = HU_SILENCE_DEFAULTS;
                hu_proactive_result_t silence_result;
                memset(&silence_result, 0, sizeof(silence_result));
                uint64_t now_ms = (uint64_t)now * 1000ULL;
                if (hu_proactive_check_silence(alloc, last_contact_ms, now_ms, &silence_cfg,
                                               &silence_result) == HU_OK &&
                    silence_result.count > 0) {
                    should_checkin = true;
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->silence_checkins++;
                    hu_proactive_build_context(&silence_result, alloc, 3, &silence_ctx,
                                               &silence_ctx_len);
                }
                hu_proactive_result_deinit(&silence_result, alloc);
            }
#endif

#ifdef HU_ENABLE_SQLITE
            /* F26: Temporal pattern — reduce proactive probability during quiet hours */
            if (agent && agent->memory && cp->contact_id) {
                int qday = 0, qstart = 0, qend = 1;
                if (hu_superhuman_temporal_get_quiet_hours(agent->memory, alloc, cp->contact_id,
                        strlen(cp->contact_id), &qday, &qstart, &qend) == HU_OK) {
                    if (tm_now.tm_wday == qday && tm_now.tm_hour >= qstart && tm_now.tm_hour < qend) {
                        uint32_t seed = (uint32_t)now * 1103515245u + 12345u +
                            (uint32_t)(uintptr_t)cp->contact_id;
                        if ((seed >> 16u) % 100u < 50u)
                            should_checkin = false;
                    }
                }
            }
#endif

            if (!should_checkin)
                break;

#ifndef HU_IS_TEST
            /* Event-triggered follow-ups from recent messages */
            char *event_ctx = NULL;
            size_t event_ctx_len = 0;
            if (combined_len > 0) {
                hu_event_extract_result_t extract_result;
                memset(&extract_result, 0, sizeof(extract_result));
                if (hu_event_extract(alloc, combined, combined_len, &extract_result) == HU_OK &&
                    extract_result.event_count > 0) {
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->events_extracted += extract_result.event_count;
                    hu_proactive_result_t event_result;
                    memset(&event_result, 0, sizeof(event_result));
                    if (hu_proactive_check_events(alloc, extract_result.events,
                                                  extract_result.event_count,
                                                  &event_result) == HU_OK &&
                        event_result.count > 0) {
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->event_followups++;
                        hu_proactive_build_context(&event_result, alloc, 3, &event_ctx,
                                                   &event_ctx_len);
                    }
                    hu_proactive_result_deinit(&event_result, alloc);
                }
                hu_event_extract_result_deinit(&extract_result, alloc);
            }

#ifdef HU_ENABLE_SQLITE
            /* F20: Commitment follow-up — add due commitments for this contact */
            char *commitment_ctx = NULL;
            size_t commitment_ctx_len = 0;
            int64_t commitment_ids[3];
            size_t commitment_ids_count = 0;
            if (agent && agent->memory && cp->contact_id) {
                hu_superhuman_commitment_t *due = NULL;
                size_t due_count = 0;
                if (hu_superhuman_commitment_list_due(agent->memory, alloc, (int64_t)now, 3,
                                                    &due, &due_count) == HU_OK &&
                    due && due_count > 0) {
                    size_t cid_len = strlen(cp->contact_id);
                    char ctx_buf[1024];
                    size_t ctx_pos = 0;
                    for (size_t di = 0; di < due_count && ctx_pos < sizeof(ctx_buf) - 200; di++) {
                        if (cid_len != strlen(due[di].contact_id) ||
                            memcmp(due[di].contact_id, cp->contact_id, cid_len) != 0)
                            continue;
                        int n = snprintf(ctx_buf + ctx_pos, sizeof(ctx_buf) - ctx_pos,
                                         "COMMITMENT FOLLOW-UP: %s was due. Ask if it happened: "
                                         "'hey did you ever %s?'\n",
                                         due[di].description, due[di].description);
                        if (n > 0 && ctx_pos + (size_t)n < sizeof(ctx_buf)) {
                            ctx_pos += (size_t)n;
                            if (commitment_ids_count < 3)
                                commitment_ids[commitment_ids_count++] = due[di].id;
                        }
                    }
                    if (ctx_pos > 0) {
                        commitment_ctx = (char *)alloc->alloc(alloc->ctx, ctx_pos + 1);
                        if (commitment_ctx) {
                            memcpy(commitment_ctx, ctx_buf, ctx_pos);
                            commitment_ctx[ctx_pos] = '\0';
                            commitment_ctx_len = ctx_pos;
                        }
                    }
                    hu_superhuman_commitment_free(alloc, due, due_count);
                }
            }
#endif

            /* F53: Birthday/holiday awareness — important_dates from persona */
            char *important_date_ctx = NULL;
            size_t important_date_ctx_len = 0;
            bool had_important_date = false;
            char important_date_msg[256];
            char important_date_type[32];
            bool important_date_sent_today = false;
            for (int di = 0; di < g_sent_important_date_count; di++) {
                if (strcmp(g_sent_important_date_contacts[di], cp->contact_id) == 0) {
                    important_date_sent_today = true;
                    break;
                }
            }
            if (!important_date_sent_today &&
                hu_proactive_check_important_dates(agent->persona, cp->contact_id,
                                                   strlen(cp->contact_id), tm_now.tm_mon + 1,
                                                   tm_now.tm_mday, important_date_msg,
                                                   sizeof(important_date_msg), important_date_type,
                                                   sizeof(important_date_type))) {
                char ctx_buf[384];
                int n = snprintf(ctx_buf, sizeof(ctx_buf),
                                 "IMPORTANT DATE (%s): %s",
                                 important_date_type, important_date_msg);
                if (n > 0 && (size_t)n < sizeof(ctx_buf)) {
                    if (strcmp(important_date_type, "birthday") == 0)
                        n += snprintf(ctx_buf + n, sizeof(ctx_buf) - (size_t)n,
                                      " Use confetti effect when sending.");
                    if (n > 0 && (size_t)n < sizeof(ctx_buf)) {
                        important_date_ctx =
                            (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
                        if (important_date_ctx) {
                            memcpy(important_date_ctx, ctx_buf, (size_t)n);
                            important_date_ctx[(size_t)n] = '\0';
                            important_date_ctx_len = (size_t)n;
                            had_important_date = true;
                        }
                    }
                }
            }

            /* Build and send the check-in */
            size_t prompt_len = 0;
            char *prompt =
                proactive_prompt_for_contact(alloc, agent, agent->memory, cp, &prompt_len);
            if (prompt && important_date_ctx && important_date_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + important_date_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, important_date_ctx, important_date_ctx_len);
                    merged[prompt_len + 1 + important_date_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = merged_len - 1;
                }
                alloc->free(alloc->ctx, important_date_ctx, important_date_ctx_len + 1);
            }
#ifdef HU_ENABLE_SQLITE
            if (prompt && commitment_ctx && commitment_ctx_len > 0) {
                size_t merged_len = prompt_len + 1 + commitment_ctx_len + 1;
                char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                if (merged) {
                    memcpy(merged, prompt, prompt_len);
                    merged[prompt_len] = '\n';
                    memcpy(merged + prompt_len + 1, commitment_ctx, commitment_ctx_len);
                    merged[prompt_len + 1 + commitment_ctx_len] = '\0';
                    alloc->free(alloc->ctx, prompt, prompt_len + 1);
                    prompt = merged;
                    prompt_len = prompt_len + 1 + commitment_ctx_len;
                }
            }
            /* F23: Topic absence — 20% chance to inject when they haven't mentioned usual topics */
            {
                char *topic_absence_json = NULL;
                size_t topic_absence_len = 0;
                int64_t now_ts = (int64_t)now;
                size_t cid_len = strlen(cp->contact_id);
                if (agent->memory &&
                    hu_superhuman_topic_absence_list(agent->memory, alloc, cp->contact_id, cid_len,
                                                    now_ts, 14, &topic_absence_json,
                                                    &topic_absence_len) == HU_OK &&
                    topic_absence_json && topic_absence_len > 0 &&
                    strstr(topic_absence_json, "- ") != NULL &&
                    ((uint32_t)((uintptr_t)cp + (uintptr_t)now) % 100) < 20) {
                    const char *hint =
                        "Topics they usually mention but haven't recently: [see above]. "
                        "Gentle check-in if appropriate.";
                    size_t hint_len = strlen(hint);
                    size_t merged_len = prompt_len + 1 + topic_absence_len + 1 + hint_len + 2;
                    char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                    if (merged) {
                        size_t pos = 0;
                        memcpy(merged, prompt, prompt_len);
                        pos += prompt_len;
                        merged[pos++] = '\n';
                        memcpy(merged + pos, topic_absence_json, topic_absence_len);
                        pos += topic_absence_len;
                        merged[pos++] = '\n';
                        memcpy(merged + pos, hint, hint_len);
                        pos += hint_len;
                        merged[pos++] = '\n';
                        merged[pos] = '\0';
                        alloc->free(alloc->ctx, prompt, prompt_len + 1);
                        prompt = merged;
                        prompt_len = pos;
                    }
                    alloc->free(alloc->ctx, topic_absence_json, topic_absence_len);
                } else if (topic_absence_json) {
                    alloc->free(alloc->ctx, topic_absence_json, topic_absence_len);
                }
            }
            /* F24: Growth celebration — 15% chance, inject milestone to celebrate */
            {
                char *growth_pro = NULL;
                size_t growth_pro_len = 0;
                size_t cid_len = strlen(cp->contact_id);
                if (agent->memory && (uint32_t)((uintptr_t)cp + (uintptr_t)now) % 100 < 15 &&
                    hu_superhuman_growth_list_recent(agent->memory, alloc, cp->contact_id, cid_len,
                                                    1, &growth_pro, &growth_pro_len) == HU_OK &&
                    growth_pro && growth_pro_len > 0 && strstr(growth_pro, "(none)") == NULL) {
                    char celebrate_buf[512];
                    int cb = snprintf(celebrate_buf, sizeof(celebrate_buf),
                                      "CELEBRATE: %.*s Acknowledge their progress.",
                                      (int)(growth_pro_len < 450 ? growth_pro_len : 450),
                                      growth_pro);
                    if (cb > 0 && (size_t)cb < sizeof(celebrate_buf) && prompt) {
                        size_t merged_len = prompt_len + 1 + (size_t)cb + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, celebrate_buf, (size_t)cb);
                            merged[prompt_len + 1 + (size_t)cb] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                    alloc->free(alloc->ctx, growth_pro, growth_pro_len);
                }
            }
            /* F30: Spontaneous curiosity — 10–15% chance, inject random question from micro-moments */
            {
                char curiosity_buf[384];
                uint32_t seed_cur = (uint32_t)((uintptr_t)cp + (uintptr_t)now);
                if (agent->memory &&
                    hu_proactive_check_curiosity(alloc, agent->memory, cp->contact_id,
                                                strlen(cp->contact_id), seed_cur, curiosity_buf,
                                                sizeof(curiosity_buf))) {
                    size_t cb_len = strlen(curiosity_buf);
                    if (prompt && cb_len > 0) {
                        size_t merged_len = prompt_len + 1 + cb_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, curiosity_buf, cb_len);
                            merged[prompt_len + 1 + cb_len] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                }
            }
            /* F31: Callback opportunities — 30% chance, inject follow-up from delayed/commitments */
            {
                char callback_buf[512];
                uint32_t seed_cb = (uint32_t)((uintptr_t)cp + (uintptr_t)now + 1);
                if (agent->memory &&
                    hu_proactive_check_callbacks(alloc, agent->memory, cp->contact_id,
                                                strlen(cp->contact_id), seed_cb, callback_buf,
                                                sizeof(callback_buf))) {
                    size_t cb_len = strlen(callback_buf);
                    if (prompt && cb_len > 0) {
                        size_t merged_len = prompt_len + 1 + cb_len + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, callback_buf, cb_len);
                            merged[prompt_len + 1 + cb_len] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                }
            }
            /* F130: Collaborative plan proposal check */
#ifdef HU_HAS_PERSONA
            if (agent->memory && prompt) {
                bool should_propose = hu_collab_plan_should_propose(
                    cp->contact_id, strlen(cp->contact_id), 0, 3, 0.5);
                if (should_propose) {
                    static const char PLAN_HINT[] =
                        "\nConsider naturally suggesting a plan or activity to do together.";
                    size_t hint_len = sizeof(PLAN_HINT) - 1;
                    size_t merged_len = prompt_len + hint_len + 1;
                    char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                    if (merged) {
                        memcpy(merged, prompt, prompt_len);
                        memcpy(merged + prompt_len, PLAN_HINT, hint_len);
                        merged[merged_len - 1] = '\0';
                        alloc->free(alloc->ctx, prompt, prompt_len + 1);
                        prompt = merged;
                        prompt_len = merged_len - 1;
                    }
                }
            }
#endif /* HU_HAS_PERSONA — collab plan */
#endif
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
            /* F19: Inside joke callback — 10–15% chance, pick one and add to prompt */
            int64_t joke_id_to_reference = -1;
            if (agent->memory && (uint32_t)((uintptr_t)cp + (uintptr_t)now) % 100 < 12) {
                hu_inside_joke_t *jokes_pro = NULL;
                size_t jokes_pro_count = 0;
                size_t cid_len = strlen(cp->contact_id);
                if (hu_superhuman_inside_joke_list(agent->memory, alloc, cp->contact_id, cid_len,
                                                   3, &jokes_pro, &jokes_pro_count) == HU_OK &&
                    jokes_pro && jokes_pro_count > 0) {
                    size_t idx = (uint32_t)((uintptr_t)cp + (uintptr_t)now + 1) % jokes_pro_count;
                    joke_id_to_reference = jokes_pro[idx].id;
                    char cb_buf[384];
                    size_t ctx_pl = strnlen(jokes_pro[idx].context, 120);
                    size_t pl_pl = strnlen(jokes_pro[idx].punchline, 80);
                    int w = snprintf(cb_buf, sizeof(cb_buf),
                                    "CALLBACK: Reference this inside joke naturally: [%.*s] %.*s",
                                    (int)ctx_pl, jokes_pro[idx].context, (int)pl_pl,
                                    jokes_pro[idx].punchline);
                    if (w > 0 && (size_t)w < sizeof(cb_buf) && prompt) {
                        size_t merged_len = prompt_len + 1 + (size_t)w + 1;
                        char *merged = (char *)alloc->alloc(alloc->ctx, merged_len);
                        if (merged) {
                            memcpy(merged, prompt, prompt_len);
                            merged[prompt_len] = '\n';
                            memcpy(merged + prompt_len + 1, cb_buf, (size_t)w);
                            merged[prompt_len + 1 + (size_t)w] = '\0';
                            alloc->free(alloc->ctx, prompt, prompt_len + 1);
                            prompt = merged;
                            prompt_len = merged_len - 1;
                        }
                    }
                    hu_superhuman_inside_joke_free(alloc, jokes_pro, jokes_pro_count);
                }
            }
            if (prompt) {
                hu_agent_clear_history(agent);
                agent->active_channel = ch_part;
                agent->active_channel_len = strlen(ch_part);

                char *response = NULL;
                size_t response_len = 0;
                hu_error_t err = hu_agent_turn(agent, prompt, prompt_len, &response, &response_len);

                if (err == HU_OK && response && response_len > 0) {
                    bool skip = (response_len == 4 && memcmp(response, "SKIP", 4) == 0);
#ifdef HU_HAS_PERSONA
                    /* F68: Protective boundary — skip proactive if topic is boundary */
                    if (!skip && agent->memory &&
                        hu_protective_is_boundary(agent->memory, cp->contact_id,
                                                 strlen(cp->contact_id), "proactive", 9))
                        skip = true;
#endif
                    if (!skip && channels[c].channel->vtable->send) {
                        channels[c].channel->vtable->send(channels[c].channel->ctx, target_part,
                                                          target_len, response, response_len, NULL,
                                                          0);
                        fprintf(stderr, "[human] proactive check-in sent to %s: %.*s\n",
                                cp->name ? cp->name : cp->contact_id, (int)response_len, response);
                        if (had_important_date &&
                            strcmp(important_date_type, "birthday") == 0)
                            fprintf(stderr, "[human] F53: birthday message — use confetti effect\n");
                        if (had_important_date && g_sent_important_date_count < 8) {
                            size_t cid_len = strlen(cp->contact_id);
                            if (cid_len < 64) {
                                memcpy(g_sent_important_date_contacts[g_sent_important_date_count],
                                       cp->contact_id, cid_len + 1);
                                g_sent_important_date_count++;
                            }
                        }
                        if (joke_id_to_reference >= 0 && agent->memory)
                            (void)hu_superhuman_inside_joke_reference(agent->memory,
                                                                      joke_id_to_reference);
#ifdef HU_ENABLE_SQLITE
                        for (size_t mi = 0; mi < commitment_ids_count; mi++)
                            (void)hu_superhuman_commitment_mark_followed_up(agent->memory,
                                                                           commitment_ids[mi]);
#endif
                    }
                }
                if (response)
                    agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                alloc->free(alloc->ctx, prompt, prompt_len + 1);
                hu_agent_clear_history(agent);
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

#endif /* HU_HAS_PERSONA && !HU_IS_TEST */

#ifndef HU_IS_TEST
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

#if !defined(HU_IS_TEST) && !defined(_WIN32) && !defined(__CYGWIN__)
static volatile sig_atomic_t g_stop_flag = 0;

static void service_signal_handler(int sig) {
    (void)sig;
    g_stop_flag = 1;
}
#endif

/* ── Streaming callback for channels with send_event ─────────────────────── */

/* ── Photo viewing delay (F6) ────────────────────────────────────────────── */
#define HU_PHOTO_VIEWING_DELAY_MIN_MS   3000
#define HU_PHOTO_VIEWING_DELAY_RANGE_MS 5001 /* 0..5000 → 3–8 s inclusive */

/* Returns 3–8 s (ms) if any message in batch has attachment, else 0. */
static uint32_t photo_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                       size_t batch_end, uint32_t seed) {
    for (size_t b = batch_start; b <= batch_end; b++) {
        if (msgs[b].has_attachment)
            return HU_PHOTO_VIEWING_DELAY_MIN_MS + (seed % HU_PHOTO_VIEWING_DELAY_RANGE_MS);
    }
    return 0;
}

/* ── Video viewing delay (F7) ────────────────────────────────────────────── */
#define HU_VIDEO_VIEWING_DELAY_MIN_MS   2000
#define HU_VIDEO_VIEWING_DELAY_RANGE_MS 8001 /* 0..8000 → 2–10 s inclusive */

/* Returns 2–10 s (ms) if any message in batch has video, else 0. */
static uint32_t video_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                       size_t batch_end, uint32_t seed) {
    for (size_t b = batch_start; b <= batch_end; b++) {
        if (msgs[b].has_video)
            return HU_VIDEO_VIEWING_DELAY_MIN_MS + (seed % HU_VIDEO_VIEWING_DELAY_RANGE_MS);
    }
    return 0;
}

#ifdef HU_IS_TEST
uint32_t hu_daemon_photo_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                          size_t batch_end, uint32_t seed) {
    return photo_viewing_delay_ms(msgs, batch_start, batch_end, seed);
}
uint32_t hu_daemon_video_viewing_delay_ms(const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                          size_t batch_end, uint32_t seed) {
    return video_viewing_delay_ms(msgs, batch_start, batch_end, seed);
}
#endif

/* ── Missed-message acknowledgment (F10) ─────────────────────────────────── */
/* Configurable missed message threshold in seconds (default 30 * 60 = 1800) */
static uint32_t g_missed_msg_threshold_sec = 30 * 60;

void hu_daemon_set_missed_msg_threshold(uint32_t secs) {
    if (secs >= 60)
        g_missed_msg_threshold_sec = secs;
}

/* Returns acknowledgment phrase or NULL if none needed.
 * delay_secs: time between receive and send.
 * receive_hour, current_hour: 0–23 from localtime.
 * seed: for deterministic phrase selection. */
const char *hu_missed_message_acknowledgment(int64_t delay_secs, int receive_hour, int current_hour,
                                             uint32_t seed) {
    if (delay_secs <= (int64_t)g_missed_msg_threshold_sec)
        return NULL;

    /* Natural gap: received 2AM–6AM, responding 8AM+ → "just woke up" style */
    if (receive_hour >= 2 && receive_hour < 6 && current_hour >= 8) {
        static const char *const woke[] = {"ha just woke up", "omg just woke up",
                                           "oof just woke up"};
        return woke[seed % 3];
    }

    /* Default: missed/saw-this phrases */
    static const char *const missed[] = {"sorry just saw this", "oh man missed this",
                                         "ha my bad just saw this"};
    return missed[seed % 3];
}

/* ── Service loop ──────────────────────────────────────────────────────── */

hu_error_t hu_service_run(hu_allocator_t *alloc, uint32_t tick_interval_ms,
                          hu_service_channel_t *channels, size_t channel_count, hu_agent_t *agent,
                          const hu_config_t *config) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;
    if (tick_interval_ms == 0)
        tick_interval_ms = 1000;

#ifdef HU_IS_TEST
    (void)tick_interval_ms;
    (void)channels;
    (void)channel_count;
    (void)agent;
    (void)config;
#ifdef HU_HAS_CRON
    run_cron_tick(alloc);
    hu_service_run_agent_cron(alloc, agent, channels, channel_count);
#endif
    return HU_OK;
#else

#if !defined(_WIN32) && !defined(__CYGWIN__)
    g_stop_flag = 0;
    signal(SIGTERM, service_signal_handler);
    signal(SIGINT, service_signal_handler);
    setlinebuf(stderr);
    setlinebuf(stdout);
#endif

    /* Enable outcome tracking so persona feedback loop works in daemon */
    hu_outcome_tracker_t daemon_outcomes;
    hu_outcome_tracker_init(&daemon_outcomes, true);
    if (agent && !agent->outcomes)
        hu_agent_set_outcomes(agent, &daemon_outcomes);

#ifdef HU_HAS_CRON
    time_t last_cron_minute = 0;
#endif
#if defined(HU_HAS_PERSONA) && defined(HU_HAS_CRON)
    time_t proactive_due_at = 0;
#endif

    hu_graph_t *graph = NULL;
#ifdef HU_ENABLE_SQLITE
    {
        const char *home = getenv("HOME");
        if (home) {
            char graph_path[HU_MAX_PATH];
            int np = snprintf(graph_path, sizeof(graph_path), "%s/.human/graph.db", home);
            if (np > 0 && (size_t)np < sizeof(graph_path)) {
                if (hu_graph_open(alloc, graph_path, (size_t)np, &graph) != HU_OK)
                    fprintf(stderr, "[human] graph open failed: %.*s\n", np, graph_path);
            }
        }
    }
    if (graph && agent && agent->retrieval_engine)
        hu_retrieval_set_graph(agent->retrieval_engine, graph);
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
#define HU_STOP_FLAG g_stop_flag
#else
    volatile int local_stop = 0;
#define HU_STOP_FLAG local_stop
#endif

#ifndef HU_IS_TEST
    static char replay_insights[2048] = {0};
    static size_t replay_insights_len = 0;
    static size_t promotion_counter = 0;
    static char community_insights[2048] = {0};
    static size_t community_insights_len = 0;

    /* Per-contact Theory of Mind belief states */
#define HU_TOM_MAX_CONTACTS    16
    static hu_belief_state_t tom_states[HU_TOM_MAX_CONTACTS];
    static char tom_contact_keys[HU_TOM_MAX_CONTACTS][64];
    static size_t tom_contact_count = 0;
    (void)tom_states;
    (void)tom_contact_keys;
    (void)tom_contact_count;

    /* Per-contact voice maturity profiles */
    static hu_voice_profile_t voice_profiles[HU_TOM_MAX_CONTACTS];
    static char voice_contact_keys[HU_TOM_MAX_CONTACTS][64];
    static size_t voice_contact_count = 0;
    (void)voice_profiles;
    (void)voice_contact_keys;
    (void)voice_contact_count;

    /* Per-contact consecutive response counter.
     * Tracks how many times Human responded in a row without the real
     * user stepping in.  Resets when user_responded_recently fires or
     * when the real user sends a message (is_from_me=1 in history). */
#define HU_CONSEC_MAX_CONTACTS 32
    static uint8_t consec_response_count[HU_CONSEC_MAX_CONTACTS];
    static char consec_contact_keys[HU_CONSEC_MAX_CONTACTS][64];
    static size_t consec_contact_count = 0;

#define HU_LEAVE_ON_READ_MAX 32
    static struct {
        char key[64];
        time_t until;
    } leave_on_read_entries[HU_LEAVE_ON_READ_MAX];

#define HU_COMFORT_PENDING_MAX 32
    static struct {
        char key[64];
        char emotion[64];
        char response_type[32];
    } comfort_pending[HU_COMFORT_PENDING_MAX];

    static hu_bth_metrics_t bth_metrics;
    hu_bth_metrics_init(&bth_metrics);
    if (agent)
        agent->bth_metrics = &bth_metrics;

    hu_inbox_watcher_t inbox_watcher = {0};
    static int64_t last_inbox_poll_ms = 0;
    if (agent && agent->memory) {
        (void)hu_inbox_init(&inbox_watcher, alloc, agent->memory, NULL, 0);
        inbox_watcher.provider = &agent->provider;
        inbox_watcher.model = agent->model_name;
        inbox_watcher.model_len = agent->model_name_len;
    }
#endif

    while (!HU_STOP_FLAG) {
#ifdef HU_HAS_CRON
        {
            time_t t = time(NULL);
            time_t current_minute = t / 60;
            if (current_minute > last_cron_minute) {
                run_cron_tick(alloc);
                hu_service_run_agent_cron(alloc, agent, channels, channel_count);
#ifdef HU_HAS_PERSONA
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
                    hu_service_run_proactive_checkins(alloc, agent, channels, channel_count);
                    if (agent && agent->bth_metrics)
                        hu_bth_metrics_log(agent->bth_metrics);
                }
#endif
#ifndef HU_IS_TEST
                /* Periodic memory consolidation */
                if (config->consolidation_interval_hours > 0 && agent && agent->memory) {
                    static int64_t last_consolidation_ms = 0;
                    int64_t interval_ms = (int64_t)config->consolidation_interval_hours * 3600000LL;
                    struct timespec ts_cons;
                    clock_gettime(CLOCK_MONOTONIC, &ts_cons);
                    int64_t now_ms = (int64_t)ts_cons.tv_sec * 1000 + ts_cons.tv_nsec / 1000000;
                    if (last_consolidation_ms == 0)
                        last_consolidation_ms = now_ms;
                    if (now_ms - last_consolidation_ms >= interval_ms) {
                        hu_consolidation_config_t cons_cfg = {
                            .decay_days = cfg->behavior.decay_days,
                            .decay_factor = 0.5,
                            .dedup_threshold = cfg->behavior.dedup_threshold,
                            .max_entries = 5000,
                            .provider = &agent->provider,
                            .model = agent->model_name,
                            .model_len = agent->model_name_len,
                        };
                        if (hu_memory_consolidate(alloc, agent->memory, &cons_cfg) == HU_OK) {
                            last_consolidation_ms = now_ms;
                            fprintf(stderr, "[human] periodic memory consolidation completed\n");
                        }
                    }
                }
#ifdef HU_HAS_PERSONA
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
                        if (hu_replay_auto_tune(alloc, agent->memory, NULL, 0, &tune_summary,
                                                &tune_len) == HU_OK &&
                            tune_summary && tune_len > 0) {
                            size_t copy_len = tune_len < sizeof(replay_insights) - 1
                                                  ? tune_len
                                                  : sizeof(replay_insights) - 1;
                            memcpy(replay_insights, tune_summary, copy_len);
                            replay_insights[copy_len] = '\0';
                            replay_insights_len = copy_len;
                            fprintf(stderr, "[human] daily replay auto-tune completed\n");
                        }
                        if (tune_summary)
                            alloc->free(alloc->ctx, tune_summary, tune_len + 1);
                        tuned_today = true;
                    }
                }
#endif
                /* Weekly GraphRAG community detection at Sunday 2 AM */
#ifdef HU_ENABLE_SQLITE
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
                        if (hu_graph_build_communities(graph, alloc, 20, 2047, &comm_ctx,
                                                       &comm_len) == HU_OK &&
                            comm_ctx && comm_len > 0) {
                            size_t copy_len = comm_len < sizeof(community_insights) - 1
                                                  ? comm_len
                                                  : sizeof(community_insights) - 1;
                            memcpy(community_insights, comm_ctx, copy_len);
                            community_insights[copy_len] = '\0';
                            community_insights_len = copy_len;
                            communities_built_this_week = true;
                            fprintf(stderr,
                                    "[human] weekly GraphRAG community detection completed\n");
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
            hu_service_channel_t *ch = &channels[i];
            if (!ch->poll_fn || !ch->channel_ctx)
                continue;
            if (tick_now - ch->last_poll_ms < (int64_t)ch->interval_ms)
                continue;

            hu_channel_loop_msg_t msgs[16];
            memset(msgs, 0, sizeof(msgs));
            size_t count = 0;
            hu_error_t poll_err = ch->poll_fn(ch->channel_ctx, alloc, msgs, 16, &count);
            ch->last_poll_ms = tick_now;
            time_t poll_receive_time = 0;
            if (count > 0) {
                ch->last_contact_ms = (uint64_t)time(NULL) * 1000ULL;
                poll_receive_time = time(NULL);
            }
            if (poll_err != HU_OK && poll_err != HU_ERR_NOT_SUPPORTED && getenv("HU_DEBUG"))
                fprintf(stderr, "[human] poll error on channel %zu: %d\n", i, (int)poll_err);

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
                    const char *content_to_add = msgs[m].content;
                    size_t mlen = strlen(content_to_add);
#ifndef HU_IS_TEST
#if defined(HU_ENABLE_IMESSAGE)
                    /* Per-message vision: when has_attachment and message_id, describe
                     * image and inject "[They sent a photo: {description}]" into context. */
                    if (msgs[m].has_attachment && msgs[m].message_id > 0 && agent &&
                        agent->provider.vtable && agent->provider.vtable->supports_vision &&
                        agent->provider.vtable->supports_vision(agent->provider.ctx)) {
                        const char *ch_name = ch->channel->vtable->name
                                                  ? ch->channel->vtable->name(ch->channel->ctx)
                                                  : NULL;
                        if (ch_name && strcmp(ch_name, "imessage") == 0) {
                            char *path = hu_imessage_get_attachment_path(alloc, msgs[m].message_id);
                            if (path) {
                                char *desc = NULL;
                                size_t desc_len = 0;
                                const char *model =
                                    agent->model_name ? agent->model_name : "gpt-4o";
                                size_t model_len = agent->model_name_len > 0
                                                       ? agent->model_name_len
                                                       : (size_t)strlen(model);
                                hu_error_t verr = hu_vision_describe_image(
                                    alloc, &agent->provider, path, strlen(path), model, model_len,
                                    &desc, &desc_len);
                                alloc->free(alloc->ctx, path, strlen(path) + 1);
                                if (verr == HU_OK && desc && desc_len > 0) {
                                    static char vision_augmented[4096];
                                    size_t desc_copy = desc_len > 3800 ? 3800 : desc_len;
                                    int n;
                                    if (mlen > 0 && strcmp(content_to_add, "[Photo]") != 0) {
                                        n = snprintf(vision_augmented, sizeof(vision_augmented),
                                                     "%.*s\n[They sent a photo: %.*s]", (int)mlen,
                                                     content_to_add, (int)desc_copy, desc);
                                    } else {
                                        n = snprintf(vision_augmented, sizeof(vision_augmented),
                                                     "[They sent a photo: %.*s]", (int)desc_copy,
                                                     desc);
                                    }
                                    alloc->free(alloc->ctx, desc, desc_len + 1);
                                    if (n > 0 && (size_t)n < sizeof(vision_augmented)) {
                                        content_to_add = vision_augmented;
                                        mlen = (size_t)n;
                                        if (agent->bth_metrics)
                                            agent->bth_metrics->vision_descriptions++;
                                    }
                                } else if (desc) {
                                    alloc->free(alloc->ctx, desc, desc_len + 1);
                                }
                            }
                        }
                    }
                    /* F7: Video context — no vision in Phase 1; inject "[They sent a video]" */
                    else if (msgs[m].has_video) {
                        static char video_augmented[4096];
                        int n;
                        if (mlen > 0 && strcmp(content_to_add, "[Video]") != 0) {
                            n = snprintf(video_augmented, sizeof(video_augmented),
                                         "%.*s\n[They sent a video]", (int)mlen, content_to_add);
                        } else {
                            n = snprintf(video_augmented, sizeof(video_augmented),
                                         "[They sent a video]");
                        }
                        if (n > 0 && (size_t)n < sizeof(video_augmented)) {
                            content_to_add = video_augmented;
                            mlen = (size_t)n;
                        }
                    }
#endif
#endif
                    if (mlen == 0) {
                        m++;
                        continue;
                    }
                    if (combined_len + mlen + 2 >= sizeof(combined))
                        break;
                    if (combined_len > 0)
                        combined[combined_len++] = '\n';
                    memcpy(combined + combined_len, content_to_add, mlen);
                    combined_len += mlen;
                    batch_end = m;
                    m++;
                }
                combined[combined_len] = '\0';

                if (combined_len == 0)
                    continue;

                /* Clear STM before each contact batch to avoid cross-contact emotion contamination
                 */
                hu_stm_clear(&agent->stm);

#ifndef HU_IS_TEST
                /* Only respond to contacts explicitly listed in persona_contacts.
                 * When a persona is loaded, ALL inbound messages from contacts not in
                 * the allowlist are silently dropped — even if the list is empty. */
#ifdef HU_HAS_PERSONA
                if (agent->persona) {
                    const hu_contact_profile_t *cp_gate =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    if (!cp_gate) {
                        if (getenv("HU_DEBUG"))
                            fprintf(stderr, "[human] ignoring message from unknown contact: %.*s\n",
                                    (int)(key_len > 20 ? 20 : key_len), batch_key);
                        continue;
                    }
                }
#endif

                fprintf(stderr, "[human] processing batch for %.*s: \"%.*s\" (group=%d)\n",
                        (int)(key_len > 20 ? 20 : key_len), batch_key,
                        (int)(combined_len > 60 ? 60 : combined_len), combined,
                        (int)msgs[batch_start].is_group);

#ifdef HU_ENABLE_SQLITE
                /* F23: Topic absence detection — record topic baselines from user message */
                if (agent->memory && combined_len > 0)
                    record_topic_baselines_from_text(agent->memory, batch_key, key_len,
                                                    combined, combined_len);
                /* F26: Temporal pattern learning — record message frequency by day/hour */
                if (agent->memory && batch_key && key_len > 0) {
                    time_t now_t = time(NULL);
                    struct tm lt_buf;
                    struct tm *lt = localtime_r(&now_t, &lt_buf);
                    if (lt) {
                        int64_t response_time_ms = 0; /* not available from poll pipeline */
                        (void)hu_superhuman_temporal_record(agent->memory, batch_key, key_len,
                            lt->tm_wday, lt->tm_hour, response_time_ms);
                    }
                }
#endif

                /* Preload channel history early so the group classifier can use it */
                hu_channel_history_entry_t *early_history = NULL;
                size_t early_history_count = 0;
                bool use_backchannel = false;
                char backchannel_buf[32];
                size_t backchannel_len = 0;
                if (ch->channel->vtable->load_conversation_history) {
                    ch->channel->vtable->load_conversation_history(
                        ch->channel->ctx, alloc, batch_key, key_len, 10, &early_history,
                        &early_history_count);
                }

                /* Track if group classifier indicates brief response */
                bool group_brief = false;

                /* Group chat gating: use group classifier to decide engagement */
                if (msgs[batch_start].is_group) {
                    const char *persona_name = NULL;
#ifdef HU_HAS_PERSONA
                    if (agent->persona && agent->persona->identity)
                        persona_name = agent->persona->identity;
#endif
                    hu_group_response_t gr =
                        hu_conversation_classify_group(combined, combined_len, persona_name,
                                                       persona_name ? strlen(persona_name) : 0,
                                                       early_history, early_history_count);
                    if (gr == HU_GROUP_SKIP) {
                        fprintf(stderr, "[human] group: skipping (not addressed): %.*s\n",
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                        if (early_history)
                            alloc->free(alloc->ctx, early_history,
                                        early_history_count * sizeof(hu_channel_history_entry_t));
                        continue;
                    }
                    if (gr == HU_GROUP_BRIEF)
                        group_brief = true;
                }

                /* Response decision using conversation-aware classifier */
                uint32_t extra_delay_ms = 0;

                hu_response_action_t action = hu_conversation_classify_response(
                    combined, combined_len, early_history, early_history_count, &extra_delay_ms);

                /* Apply response_mode override from iMessage config.
                 * "selective" (default): FULL→BRIEF for non-questions.
                 * "eager": BRIEF/SKIP→FULL (respond to almost everything).
                 * "normal": no override. */
                {
                    const char *rmode = config ? config->channels.imessage.response_mode : NULL;
                    if (!rmode || !rmode[0] || strcmp(rmode, "selective") == 0) {
                        if (action == HU_RESPONSE_FULL && !memchr(combined, '?', combined_len))
                            action = HU_RESPONSE_BRIEF;
                    } else if (strcmp(rmode, "eager") == 0) {
                        if (action == HU_RESPONSE_BRIEF)
                            action = HU_RESPONSE_FULL;
                    }
                    /* "normal" = no change */
                }

                /* Natural drop-off: when FULL/BRIEF, probabilistic skip for mutual farewell,
                 * low-energy acks, emoji-only, or our farewell + their minimal reply. */
#ifndef HU_IS_TEST
                if ((action == HU_RESPONSE_FULL || action == HU_RESPONSE_BRIEF) && early_history &&
                    early_history_count > 0) {
                    uint32_t dropoff_seed =
                        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    int dropoff_prob = hu_conversation_classify_dropoff(
                        combined, combined_len, early_history, early_history_count, dropoff_seed);
                    if (dropoff_prob > 0 &&
                        ((dropoff_seed >> 16u) % 100u) < (uint32_t)dropoff_prob) {
                        action = HU_RESPONSE_SKIP;
                        fprintf(stderr, "[human] drop-off skip (prob=%d%%): %.*s\n", dropoff_prob,
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                    }
                }
#endif

                fprintf(stderr, "[human] classify result: action=%d delay=%u for %.*s\n",
                        (int)action, (unsigned)extra_delay_ms, (int)(key_len > 20 ? 20 : key_len),
                        batch_key);

                /* Consecutive response limiter: if Human has responded to
                 * this contact 3+ times in a row without the real user
                 * stepping in, stay silent so we don't run away. */
                size_t consec_idx = SIZE_MAX;
                for (size_t ci = 0; ci < consec_contact_count; ci++) {
                    if (key_len < sizeof(consec_contact_keys[0]) &&
                        memcmp(consec_contact_keys[ci], batch_key, key_len) == 0 &&
                        consec_contact_keys[ci][key_len] == '\0') {
                        consec_idx = ci;
                        break;
                    }
                }
                if (consec_idx != SIZE_MAX && consec_response_count[consec_idx] >= 3 &&
                    action != HU_RESPONSE_SKIP) {
                    fprintf(stderr,
                            "[human] consecutive limit (%u) reached for %.*s — staying silent\n",
                            (unsigned)consec_response_count[consec_idx],
                            (int)(key_len > 20 ? 20 : key_len), batch_key);
                    action = HU_RESPONSE_SKIP;
                }

                /* Tapback-skip: for tapback-worthy messages, 70% chance to not respond */
                bool tapback_skip = false;
#ifndef HU_IS_TEST
                if (action != HU_RESPONSE_SKIP && is_tapback_worthy(combined, combined_len)) {
                    uint32_t r = (uint32_t)time(NULL);
                    r = r * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    r = (r >> 16u) & 0x7fffu;
                    if ((r % 100u) < 70u)
                        tapback_skip = true;
                }
#endif

                /* F46: Leave-on-read — deliberate non-response as social signal (<2%).
                 * Never in group chats. Skip if we're in active leave-on-read period, or
                 * if classifier says leave-on-read and we store 2–24h timer. */
                bool leave_on_read_skip = false;
#ifndef HU_IS_TEST
                if (!msgs[batch_start].is_group) {
                    time_t now_ts = time(NULL);
                    size_t lor_slot = SIZE_MAX;
                    for (size_t lor_i = 0; lor_i < HU_LEAVE_ON_READ_MAX; lor_i++) {
                        if (leave_on_read_entries[lor_i].key[0] == '\0')
                            continue;
                        if (key_len < sizeof(leave_on_read_entries[lor_i].key) &&
                            memcmp(leave_on_read_entries[lor_i].key, batch_key, key_len) == 0 &&
                            leave_on_read_entries[lor_i].key[key_len] == '\0') {
                            if (leave_on_read_entries[lor_i].until > now_ts) {
                                leave_on_read_skip = true;
                                fprintf(stderr, "[human] leave-on-read: still in period for %.*s\n",
                                        (int)(key_len > 20 ? 20 : key_len), batch_key);
                            } else {
                                lor_slot = lor_i; /* expired, can reuse */
                            }
                            break;
                        }
                    }
                    if (!leave_on_read_skip && action != HU_RESPONSE_SKIP && !tapback_skip) {
                        uint32_t lor_seed =
                            (uint32_t)now_ts * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                        if (hu_conversation_should_leave_on_read(combined, combined_len,
                                                                 early_history, early_history_count,
                                                                 lor_seed)) {
                            leave_on_read_skip = true;
                            /* Store leave_on_read_until: now + random 2–24 hours */
                            uint32_t hrs = 7200u + ((lor_seed >> 16u) % (86400u - 7200u + 1u));
                            time_t until = now_ts + (time_t)hrs;
                            if (lor_slot == SIZE_MAX) {
                                for (size_t lor_j = 0; lor_j < HU_LEAVE_ON_READ_MAX; lor_j++) {
                                    if (leave_on_read_entries[lor_j].key[0] == '\0' ||
                                        leave_on_read_entries[lor_j].until <= now_ts) {
                                        lor_slot = lor_j;
                                        break;
                                    }
                                }
                            }
                            if (lor_slot != SIZE_MAX &&
                                key_len < sizeof(leave_on_read_entries[0].key)) {
                                memcpy(leave_on_read_entries[lor_slot].key, batch_key, key_len);
                                leave_on_read_entries[lor_slot].key[key_len] = '\0';
                                leave_on_read_entries[lor_slot].until = until;
                                fprintf(stderr,
                                        "[human] leave-on-read: skipping for %.*s until %ld\n",
                                        (int)(key_len > 20 ? 20 : key_len), batch_key, (long)until);
                            }
                        }
                    }
                }
#endif

                if (action == HU_RESPONSE_SKIP || tapback_skip || leave_on_read_skip) {
                    if (tapback_skip)
                        fprintf(stderr, "[human] tapback-skip (no response): %.*s\n",
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                    else
                        fprintf(stderr, "[human] skipping message (no response needed): %.*s\n",
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
                    if (early_history)
                        alloc->free(alloc->ctx, early_history,
                                    early_history_count * sizeof(hu_channel_history_entry_t));
                    continue;
                }

                /* F29: Active listening backchannels — send brief cue instead of LLM when
                 * narrative/venting detected and probability roll passes. */
                {
                    float bc_prob = 0.3f;
#ifdef HU_HAS_PERSONA
                    if (agent && agent->persona)
                        bc_prob = agent->persona->humanization.backchannel_probability;
#endif
                    uint32_t bc_seed =
                        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    if (hu_conversation_should_backchannel(combined, combined_len, early_history,
                                                           early_history_count, bc_seed, bc_prob)) {
                        backchannel_len = hu_conversation_pick_backchannel(
                            bc_seed + 1, backchannel_buf, sizeof(backchannel_buf));
                        if (backchannel_len > 0)
                            use_backchannel = true;
                    }
                }
                if (early_history)
                    alloc->free(alloc->ctx, early_history,
                                early_history_count * sizeof(hu_channel_history_entry_t));

                /* ── BTH: Late-night mode (b1c) ────────────────────────────── */
                int bth_hour = -1;
#ifndef HU_IS_TEST
                {
                    time_t night_t = time(NULL);
                    struct tm night_buf;
                    struct tm *night_lt = localtime_r(&night_t, &night_buf);
                    if (night_lt)
                        bth_hour = night_lt->tm_hour;
                }
                if (bth_hour >= 0) {
                    if (bth_hour >= 2 && bth_hour < 6) {
                        /* 2AM-6AM: very high SKIP chance (sleeping) */
                        uint32_t night_r = (uint32_t)time(NULL) * 1103515245u + 12345u;
                        if (((night_r >> 16u) % 100u) < 85u) {
                            fprintf(stderr, "[human] late-night skip (2AM-6AM, hour=%d)\n",
                                    bth_hour);
                            action = HU_RESPONSE_SKIP;
                            continue;
                        }
                        /* If not skipped, force very delayed + brief */
                        extra_delay_ms += 30000 + ((night_r >> 8u) % 60000u);
                        action = HU_RESPONSE_BRIEF;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        /* 7-9AM: brief "just woke up" energy */
                        if (action == HU_RESPONSE_FULL)
                            action = HU_RESPONSE_BRIEF;
                    }
                }
#endif

                /* ── BTH: Media-type awareness (b3c) ──────────────────────── */
                if (action != HU_RESPONSE_SKIP &&
                    hu_conversation_is_media_message(combined, combined_len, NULL, 0)) {
                    action = HU_RESPONSE_BRIEF;
                    fprintf(stderr, "[human] media message detected, forcing brief response\n");
                }

                /* For BRIEF actions, override max_response_chars to force ultra-short */
                bool brief_mode = (action == HU_RESPONSE_BRIEF) || group_brief ||
                                  msgs[batch_start].is_group; /* always brief in group chats */

#ifndef HU_IS_TEST
                if (agent && agent->bth_metrics)
                    agent->bth_metrics->total_turns++;
#endif

                /* Adaptive timing based on message type. Variable delay with jitter,
                 * time-of-day awareness, and rare "busy" delay.
                 * This sleep also serves as the burst accumulation window. */
#ifndef HU_IS_TEST
                {
                    size_t msg_count = batch_end - batch_start + 1;
                    uint32_t base_delay = 2500 + (uint32_t)(msg_count * 1500);
                    if (base_delay > 8000)
                        base_delay = 8000;
                    base_delay += extra_delay_ms;
                    if (base_delay > 15000)
                        base_delay = 15000;

                    /* F6: Photo viewing delay — 3–8 s when batch has attachment */
                    {
                        uint32_t seed = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                        seed = (uint32_t)arc4random_uniform(5001);
#else
                        seed = ((uint32_t)time(NULL) * 1103515245u + 12345u) % 5001u;
#endif
                        base_delay += photo_viewing_delay_ms(msgs, batch_start, batch_end, seed);
                    }
                    /* F7: Video viewing delay — 2–10 s when batch has video */
                    {
                        uint32_t vseed = 0;
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                        vseed = (uint32_t)arc4random_uniform(8001);
#else
                        vseed = ((uint32_t)time(NULL) * 2654435769u + 54321u) % 8001u;
#endif
                        base_delay += video_viewing_delay_ms(msgs, batch_start, batch_end, vseed);
                    }

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

                    /* Late-night mode: realistic delays — a 51yo checking his
                     * phone groggily takes 1-3 minutes, not 8 seconds. */
                    if (bth_hour >= 1 && bth_hour < 7) {
                        /* 1-7 AM: probably asleep, phone woke them up */
                        adjusted = adjusted * 3;
                        adjusted += 90000 + (int32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                arc4random_uniform(90001)
#else
                                                (((uint32_t)time(NULL) * 2654435769u) >> 16u) %
                                                90001u
#endif
                                            );
                    } else if (bth_hour >= 0 && bth_hour < 1) {
                        /* midnight-1 AM: winding down, slow to respond */
                        adjusted = adjusted * 2;
                        adjusted += 45000 + (int32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                arc4random_uniform(75001)
#else
                                                (((uint32_t)time(NULL) * 2654435769u) >> 16u) %
                                                75001u
#endif
                                            );
                    } else if (bth_hour >= 22) {
                        /* 10 PM+: getting sleepy */
                        adjusted = adjusted * 2;
                        adjusted += 20000 + (int32_t)(
#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
                                                arc4random_uniform(40001)
#else
                                                (((uint32_t)time(NULL) * 2654435769u) >> 16u) %
                                                40001u
#endif
                                            );
                    } else if (bth_hour >= 21) {
                        /* 9-10 PM: evening, slower than daytime */
                        adjusted = adjusted * 3 / 2;
                        adjusted += 10000;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        /* 7-9 AM: just woke up, groggy */
                        adjusted = adjusted * 3 / 2;
                        adjusted += 15000;
                    }

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

                    /* F59: Multiply delay by life sim availability factor */
#ifdef HU_HAS_PERSONA
                    if (agent && agent->persona &&
                        (agent->persona->daily_routine.weekday_count > 0 ||
                         agent->persona->daily_routine.weekend_count > 0)) {
                        time_t now_ts = time(NULL);
                        struct tm tm_buf;
                        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
                        int dow = lt ? lt->tm_wday : 0;
                        uint32_t seed = (uint32_t)now_ts * 1103515245u + 12345u;
                        hu_life_sim_state_t ls_state = hu_life_sim_get_current(
                            &agent->persona->daily_routine, (int64_t)now_ts, dow, seed);
                        adjusted = (int32_t)((float)adjusted * ls_state.availability_factor);
                        if (adjusted < 1000)
                            adjusted = 1000;
                    }
#endif
                    fprintf(stderr, "[human] reading delay: %dms (hour=%d)\n", (int)adjusted,
                            bth_hour);
                    usleep((useconds_t)((unsigned int)adjusted * 1000));
                    fprintf(stderr, "[human] delay done, building context...\n");
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
                    hu_channel_loop_msg_t burst[16];
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

#ifdef HU_ENABLE_SQLITE
                /* F20: Commitment keeper — detect and store commitments from their message */
                if (agent && agent->memory && combined_len > 0) {
                    char desc_buf[512];
                    char who_buf[64];
                    if (hu_conversation_detect_commitment(combined, combined_len, desc_buf,
                                                         sizeof(desc_buf), who_buf,
                                                         sizeof(who_buf), false)) {
                        int64_t deadline =
                            hu_conversation_parse_deadline(combined, combined_len,
                                                          (int64_t)time(NULL));
                        (void)hu_superhuman_commitment_store(agent->memory, alloc, batch_key,
                                                            key_len, desc_buf,
                                                            (size_t)strlen(desc_buf), who_buf,
                                                            (size_t)strlen(who_buf), deadline);
                    }
                    /* F24: Growth celebration — detect positive outcomes, store for later reference */
                    {
                        char topic_buf[128];
                        char after_buf[64];
                        if (hu_conversation_detect_growth_opportunity(combined, combined_len,
                                                                      topic_buf, sizeof(topic_buf),
                                                                      after_buf, sizeof(after_buf))) {
                            (void)hu_superhuman_growth_store(agent->memory, alloc, batch_key,
                                                             key_len, topic_buf,
                                                             (size_t)strlen(topic_buf),
                                                             "worried/stressed", 15, after_buf,
                                                             (size_t)strlen(after_buf));
                        }
                    }
                    /* F22: Pattern mirror — record topic + emotional tone for behavioral patterns */
                    {
                        char topic_buf[64];
                        size_t topic_len =
                            hu_conversation_extract_topic(combined, combined_len, topic_buf,
                                                          sizeof(topic_buf));
                        if (topic_len > 0) {
                            const char *tone =
                                hu_conversation_classify_emotional_tone(combined, combined_len);
                            size_t tone_len = tone ? strlen(tone) : 0;
                            if (tone_len > 0) {
                                time_t now = time(NULL);
                                struct tm tm_buf;
                                struct tm *tm = hu_platform_localtime_r(&now, &tm_buf);
                                int dow = tm ? tm->tm_wday : 0;
                                int hour = tm ? tm->tm_hour : 0;
                                (void)hu_superhuman_pattern_record(agent->memory, batch_key,
                                                                    key_len, topic_buf, topic_len,
                                                                    tone, tone_len, dow, hour);
                            }
                        }
                    }
                }
#endif

                /* Real-user-response window: if the real user already replied to
                 * this contact during our delay, stay silent.  Only applies to
                 * iMessage where we impersonate the user. */
#if defined(HU_ENABLE_IMESSAGE)
                {
                    const char *chn = ch->channel->vtable->name
                                          ? ch->channel->vtable->name(ch->channel->ctx)
                                          : NULL;
                    if (chn && strcmp(chn, "imessage") == 0) {
                        int window = 120;
                        if (config && config->channels.imessage.user_response_window_sec > 0)
                            window = config->channels.imessage.user_response_window_sec;
                        if (hu_imessage_user_responded_recently(ch->channel->ctx, batch_key,
                                                                key_len, window)) {
                            fprintf(stderr,
                                    "[human] real user responded to %.*s within %ds — "
                                    "staying silent\n",
                                    (int)(key_len > 20 ? 20 : key_len), batch_key, window);
                            /* Reset consecutive counter — real user is active */
                            for (size_t ci = 0; ci < consec_contact_count; ci++) {
                                if (key_len < sizeof(consec_contact_keys[0]) &&
                                    memcmp(consec_contact_keys[ci], batch_key, key_len) == 0 &&
                                    consec_contact_keys[ci][key_len] == '\0') {
                                    consec_response_count[ci] = 0;
                                    break;
                                }
                            }
                            continue;
                        }
                    }
                }
#endif
#else
                bool brief_mode = false;
#endif

                hu_agent_clear_history(agent);

                /* Set active channel for per-channel persona overlays */
                if (ch->channel->vtable->name) {
                    agent->active_channel = ch->channel->vtable->name(ch->channel->ctx);
                    agent->active_channel_len =
                        agent->active_channel ? strlen(agent->active_channel) : 0;
                } else {
                    agent->active_channel = NULL;
                    agent->active_channel_len = 0;
                }

#ifdef HU_HAS_PERSONA
                /* Apply persona override: per-contact takes priority, then per-channel */
                if (config) {
                    const char *persona_override = NULL;
                    if (batch_key && key_len > 0)
                        persona_override = hu_config_persona_for_contact(config, batch_key);
                    if (!persona_override && agent->active_channel)
                        persona_override =
                            hu_config_persona_for_channel(config, agent->active_channel);
                    if (persona_override && persona_override[0]) {
                        const char *current = agent->persona_name ? agent->persona_name : "";
                        if (strcmp(persona_override, current) != 0) {
                            hu_error_t perr = hu_agent_set_persona(agent, persona_override,
                                                                   strlen(persona_override));
                            if (perr != HU_OK) {
#ifndef HU_IS_TEST
                                fprintf(stderr,
                                        "[human] warning: failed to switch persona to '%s'\n",
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
                    hu_message_entry_t *entries = NULL;
                    size_t entry_count = 0;
                    if (agent->session_store->vtable->load_messages(
                            agent->session_store->ctx, alloc, batch_key, key_len, &entries,
                            &entry_count) == HU_OK &&
                        entries && entry_count > 0) {
                        for (size_t e = 0; e < entry_count; e++) {
                            if (!entries[e].content || entries[e].content_len == 0)
                                continue;
                            hu_role_t role = HU_ROLE_USER;
                            if (entries[e].role) {
                                if (strcmp(entries[e].role, "assistant") == 0)
                                    role = HU_ROLE_ASSISTANT;
                                else if (strcmp(entries[e].role, "system") == 0)
                                    role = HU_ROLE_SYSTEM;
                            }
                            if (agent->history_count >= agent->history_cap) {
                                size_t new_cap = agent->history_cap ? agent->history_cap * 2 : 8;
                                hu_owned_message_t *arr = (hu_owned_message_t *)alloc->realloc(
                                    alloc->ctx, agent->history,
                                    agent->history_cap * sizeof(hu_owned_message_t),
                                    new_cap * sizeof(hu_owned_message_t));
                                if (!arr)
                                    break;
                                agent->history = arr;
                                agent->history_cap = new_cap;
                            }
                            hu_owned_message_t *hm = &agent->history[agent->history_count];
                            memset(hm, 0, sizeof(*hm));
                            hm->role = role;
                            hm->content = hu_strndup(agent->alloc, entries[e].content,
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
                        alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_message_entry_t));
                    }
                }

                char *response = NULL;
                size_t response_len = 0;
                hu_error_t err = HU_OK;
                /* hu_agent_turn allocates response via agent->alloc; use agent->alloc for
                 * free/realloc to match. (agent->alloc == alloc at creation time.) */

                /* Build per-turn context via proper architecture:
                 * 1. Contact profile from persona (hu_persona_find_contact)
                 * 2. Conversation history from channel vtable (load_conversation_history)
                 * 3. Awareness from shared analyzer (hu_conversation_build_awareness)
                 * 4. Response constraints from channel vtable (get_response_constraints)
                 */
                char *contact_ctx = NULL;
                size_t contact_ctx_len = 0;
                char *convo_ctx = NULL;
                size_t convo_ctx_len = 0;
                hu_channel_history_entry_t *history_entries = NULL;
                size_t history_count = 0;
#ifdef HU_HAS_PERSONA
                const hu_contact_profile_t *contact_for_tapback = NULL;
#endif

#ifndef HU_IS_TEST
                /* 1. Per-contact profile via persona struct */
#ifdef HU_HAS_PERSONA
                if (agent->persona) {
                    const hu_contact_profile_t *cp =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    if (cp) {
                        contact_for_tapback = cp;
                        hu_contact_profile_build_context(alloc, cp, &contact_ctx, &contact_ctx_len);

                        size_t iw_len = 0;
                        char *iw_ctx = hu_persona_build_inner_world_context(
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

#if defined(HU_HAS_PERSONA) && !defined(HU_IS_TEST)
                /* BTH: Ongoing per-contact style learning (b2c) — re-run every 10 convos,
                 * use all overlay fields, LRU eviction at cap */
                {
#define HU_STYLE_CACHE_CAP        16
#define HU_STYLE_RELEARN_INTERVAL 10
                    static struct {
                        char key[64];
                        uint32_t convo_count;
                        uint64_t last_used;
                    } style_cache[HU_STYLE_CACHE_CAP];
                    static size_t style_cache_count = 0;
                    static uint64_t style_seq = 0;
                    style_seq++;

                    size_t slot = (size_t)-1;
                    for (size_t hu_i = 0; hu_i < style_cache_count; hu_i++) {
                        if (strncmp(style_cache[hu_i].key, batch_key, key_len) == 0 &&
                            style_cache[hu_i].key[key_len] == '\0') {
                            slot = hu_i;
                            style_cache[hu_i].convo_count++;
                            style_cache[hu_i].last_used = style_seq;
                            break;
                        }
                    }

                    if (slot == (size_t)-1 && key_len < 64) {
                        if (style_cache_count < HU_STYLE_CACHE_CAP) {
                            slot = style_cache_count++;
                        } else {
                            size_t lru = 0;
                            for (size_t hu_i = 1; hu_i < HU_STYLE_CACHE_CAP; hu_i++) {
                                if (style_cache[hu_i].last_used < style_cache[lru].last_used)
                                    lru = hu_i;
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
                        if (cc == 1 || (cc % HU_STYLE_RELEARN_INTERVAL) == 0)
                            should_profile = true;
                    }

                    if (should_profile) {
                        hu_persona_overlay_t auto_ov;
                        memset(&auto_ov, 0, sizeof(auto_ov));
                        if (hu_persona_auto_profile(alloc, batch_key, key_len, &auto_ov) == HU_OK) {
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
                                char *note = hu_strndup(alloc, profile_buf, (size_t)pb_n);
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

#ifndef HU_IS_TEST
                /* F27: If we have pending comfort record for this contact, their current message
                 * is their reply. Score engagement and record, then clear pending. */
                if (agent->memory) {
                    for (size_t cp_i = 0; cp_i < HU_COMFORT_PENDING_MAX; cp_i++) {
                        if (comfort_pending[cp_i].key[0] == '\0')
                            continue;
                        if (key_len < sizeof(comfort_pending[0].key) &&
                            memcmp(comfort_pending[cp_i].key, batch_key, key_len) == 0 &&
                            comfort_pending[cp_i].key[key_len] == '\0') {
                            float eng = score_comfort_engagement(combined, combined_len);
                            (void)hu_comfort_pattern_record(
                                agent->memory, batch_key, key_len, comfort_pending[cp_i].emotion,
                                strlen(comfort_pending[cp_i].emotion),
                                comfort_pending[cp_i].response_type,
                                strlen(comfort_pending[cp_i].response_type), eng);
                            comfort_pending[cp_i].key[0] = '\0';
                            break;
                        }
                    }
                }
#endif

                /* 2b. Cross-channel awareness: load history from OTHER channels
                 * for the same contact (e.g. Gmail history when texting via iMessage) */
                hu_channel_history_entry_t *cross_entries = NULL;
                size_t cross_count = 0;
#ifdef HU_HAS_PERSONA
                {
                    const hu_contact_profile_t *cp_cross = NULL;
                    if (agent->persona)
                        cp_cross = hu_persona_find_contact(agent->persona, batch_key, key_len);
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

                /* Merge primary + cross-channel history for Phase 6 ToM and awareness. */
                hu_channel_history_entry_t *merged_history = NULL;
                const hu_channel_history_entry_t *ctx_entries = NULL;
                size_t ctx_count = 0;
                if (history_count > 0 || cross_count > 0) {
                    size_t total = history_count + cross_count;
                    if (cross_count > 0 && history_count > 0) {
                        merged_history = (hu_channel_history_entry_t *)alloc->alloc(
                            alloc->ctx, total * sizeof(hu_channel_history_entry_t));
                        if (merged_history) {
                            if (cross_entries)
                                memcpy(merged_history, cross_entries,
                                       cross_count * sizeof(hu_channel_history_entry_t));
                            memcpy(merged_history + cross_count, history_entries,
                                   history_count * sizeof(hu_channel_history_entry_t));
                        }
                    }
                    ctx_entries = merged_history ? merged_history
                                    : history_entries ? history_entries
                                    : cross_entries;
                    ctx_count = merged_history ? total
                                 : history_entries ? history_count
                                 : cross_count;
                }

                /* Phase 6 (F59–F69): Build prefix context before awareness.
                 * Order: life sim, mood, ToM, anticipatory, self-awareness, life chapter,
                 * social graph, humor. These are prepended to hu_conversation_build_awareness. */
#ifdef HU_HAS_PERSONA
                char *phase6_prefix = NULL;
                size_t phase6_len = 0;
                if (agent && agent->persona) {
                    const hu_contact_profile_t *cp_p6 =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    (void)cp_p6;
#define PHASE6_APPEND(str, len)                                                  \
    do {                                                                         \
        if ((str) && (len) > 0) {                                                \
            size_t new_len = phase6_len + (len) + (phase6_len ? 2 : 1);         \
            char *p6m = (char *)alloc->realloc(alloc->ctx, phase6_prefix,      \
                                             phase6_len ? phase6_len + 1 : 0, new_len); \
            if (p6m) {                                                          \
                phase6_prefix = p6m;                                              \
                if (phase6_len) {                                               \
                    phase6_prefix[phase6_len] = '\n';                            \
                    phase6_prefix[phase6_len + 1] = '\n';                        \
                    memcpy(phase6_prefix + phase6_len + 2, (str), (len));      \
                } else                                                           \
                    memcpy(phase6_prefix, (str), (len));                         \
                phase6_prefix[new_len - 1] = '\0';                               \
                phase6_len = new_len - 1;                                       \
                alloc->free(alloc->ctx, (str), (len) + 1);                       \
            } else if ((str))                                                    \
                alloc->free(alloc->ctx, (str), (len) + 1);                       \
        }                                                                       \
    } while (0)

                    /* 1. Life sim context (F59) */
                    if (agent->persona->daily_routine.weekday_count > 0 ||
                        agent->persona->daily_routine.weekend_count > 0) {
                        time_t now_ts = time(NULL);
                        struct tm tm_buf;
                        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
                        int dow = lt ? lt->tm_wday : 0;
                        uint32_t seed = (uint32_t)now_ts * 1103515245u + 12345u;
                        hu_life_sim_state_t ls_state = hu_life_sim_get_current(
                            &agent->persona->daily_routine, (int64_t)now_ts, dow, seed);
                        size_t ls_len = 0;
                        char *ls_ctx = hu_life_sim_build_context(alloc, &ls_state, &ls_len);
                        if (ls_ctx && ls_len > 0) {
                            phase6_prefix = ls_ctx;
                            phase6_len = ls_len;
                        } else if (ls_ctx) {
                            alloc->free(alloc->ctx, ls_ctx, ls_len + 1);
                        }
                    }

                    /* 2. Current mood (F60) — persona mood directive */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        hu_mood_state_t mood_state;
                        memset(&mood_state, 0, sizeof(mood_state));
                        if (hu_mood_get_current(alloc, agent->memory, &mood_state) == HU_OK) {
                            size_t mood_len = 0;
                            char *mood_dir = hu_mood_build_directive(alloc, &mood_state, &mood_len);
                            if (mood_dir && mood_len > 0)
                                PHASE6_APPEND(mood_dir, mood_len);
                            else if (mood_dir)
                                alloc->free(alloc->ctx, mood_dir, mood_len + 1);
                        }
                    }
#endif

                    /* 3. Theory of Mind inference (F58) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && ctx_entries && ctx_count > 0) {
                        hu_contact_baseline_t tom_baseline;
                        memset(&tom_baseline, 0, sizeof(tom_baseline));
                        if (hu_theory_of_mind_get_baseline(agent->memory, alloc, batch_key,
                                                           key_len, &tom_baseline) == HU_OK &&
                            tom_baseline.messages_sampled >= 5) {
                            hu_theory_of_mind_deviation_t dev = hu_theory_of_mind_detect_deviation(
                                &tom_baseline, ctx_entries, ctx_count);
                            if (dev.severity >= 0.3f) {
                                const char *contact_name = NULL;
                                size_t name_len = 0;
                                if (cp_p6 && cp_p6->name) {
                                    contact_name = cp_p6->name;
                                    name_len = strlen(cp_p6->name);
                                } else {
                                    contact_name = batch_key;
                                    name_len = key_len;
                                }
                                size_t tom_len = 0;
                                char *tom_inf = hu_theory_of_mind_build_inference(
                                    alloc, contact_name, name_len, NULL, 0, &dev, &tom_len);
                                if (tom_inf && tom_len > 0)
                                    PHASE6_APPEND(tom_inf, tom_len);
                                else if (tom_inf)
                                    alloc->free(alloc->ctx, tom_inf, tom_len + 1);
                            }
                        }
                    }
#endif

                    /* 4. Anticipatory predictions (F64) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        hu_emotional_prediction_t *preds = NULL;
                        size_t pred_count = 0;
                        if (hu_anticipatory_predict(alloc, agent->memory, batch_key, key_len,
                                                    (int64_t)time(NULL), &preds, &pred_count) ==
                                HU_OK &&
                            preds && pred_count > 0) {
                            const char *cname = (cp_p6 && cp_p6->name) ? cp_p6->name : batch_key;
                            size_t cname_len = (cp_p6 && cp_p6->name) ? strlen(cp_p6->name) : key_len;
                            size_t ant_len = 0;
                            char *ant_dir = hu_anticipatory_build_directive(
                                alloc, preds, pred_count, cname, cname_len, &ant_len);
                            hu_anticipatory_predictions_free(alloc, preds, pred_count);
                            if (ant_dir && ant_len > 0)
                                PHASE6_APPEND(ant_dir, ant_len);
                            else if (ant_dir)
                                alloc->free(alloc->ctx, ant_dir, ant_len + 1);
                        }
                    }
#endif

                    /* 5. Self-awareness / reciprocity (F62, F63) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        char *sa_dir = NULL;
                        size_t sa_len = 0;
                        if (hu_self_awareness_build_directive_from_memory(
                                alloc, agent->memory, batch_key, key_len,
                                (int64_t)time(NULL), &sa_dir, &sa_len) == HU_OK &&
                            sa_dir && sa_len > 0) {
                            PHASE6_APPEND(sa_dir, sa_len);
                        }
                        char *rec_dir = NULL;
                        size_t rec_len = 0;
                        if (hu_self_awareness_build_reciprocity_directive(
                                alloc, agent->memory, batch_key, key_len, &rec_dir, &rec_len) ==
                                HU_OK &&
                            rec_dir && rec_len > 0) {
                            PHASE6_APPEND(rec_dir, rec_len);
                        }
                    }
#endif

                    /* 6. Life chapter (F66) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        hu_life_chapter_t lc = {0};
                        if (hu_life_chapter_get_active(alloc, agent->memory, &lc) == HU_OK &&
                            lc.theme[0]) {
                            size_t lc_len = 0;
                            char *lc_dir = hu_life_chapter_build_directive(alloc, &lc, &lc_len);
                            if (lc_dir && lc_len > 0)
                                PHASE6_APPEND(lc_dir, lc_len);
                            else if (lc_dir)
                                alloc->free(alloc->ctx, lc_dir, lc_len + 1);
                        } else if (agent->persona->current_chapter.theme[0]) {
                            size_t lc_len = 0;
                            char *lc_dir = hu_life_chapter_build_directive(
                                alloc, &agent->persona->current_chapter, &lc_len);
                            if (lc_dir && lc_len > 0)
                                PHASE6_APPEND(lc_dir, lc_len);
                            else if (lc_dir)
                                alloc->free(alloc->ctx, lc_dir, lc_len + 1);
                        }
                    }
#endif

                    /* 7. Social network (F67) */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory) {
                        hu_relationship_t *rels = NULL;
                        size_t rel_count = 0;
                        if (hu_social_graph_get(alloc, agent->memory, batch_key, key_len,
                                                &rels, &rel_count) == HU_OK &&
                            rels && rel_count > 0) {
                            const char *cname = (cp_p6 && cp_p6->name) ? cp_p6->name : batch_key;
                            size_t cname_len = (cp_p6 && cp_p6->name) ? strlen(cp_p6->name) : key_len;
                            size_t sg_len = 0;
                            char *sg_dir = hu_social_graph_build_directive(
                                alloc, cname, cname_len, rels, rel_count, &sg_len);
                            hu_social_graph_free(alloc, rels, rel_count);
                            if (sg_dir && sg_len > 0)
                                PHASE6_APPEND(sg_dir, sg_len);
                            else if (sg_dir)
                                alloc->free(alloc->ctx, sg_dir, sg_len + 1);
                        }
                    }
#endif

                    /* 8. Humor (F69) — only when playful and not concerning */
                    if (agent->persona->humor.type) {
                        hu_emotional_state_t emo_humor =
                            history_entries && history_count > 0
                                ? hu_conversation_detect_emotion(history_entries, history_count)
                                : (hu_emotional_state_t){0};
                        bool playful = (combined_len > 0) && (
                            strstr(combined, "lol") || strstr(combined, "haha") ||
                            strstr(combined, "😂") || strstr(combined, "😄"));
                        if (playful && !emo_humor.concerning) {
                            const char *dom = emo_humor.dominant_emotion
                                                  ? emo_humor.dominant_emotion
                                                  : "neutral";
                            size_t hum_len = 0;
                            char *hum_dir = hu_humor_build_persona_directive(
                                alloc, &agent->persona->humor, dom, strlen(dom),
                                true, &hum_len);
                            if (hum_dir && hum_len > 0)
                                PHASE6_APPEND(hum_dir, hum_len);
                            else if (hum_dir)
                                alloc->free(alloc->ctx, hum_dir, hum_len + 1);
                        }
                    }

                    /* Phase 7 (F72–F76): Prospective memory, emotional residue, episodic context */
#ifdef HU_ENABLE_SQLITE
                    if (agent->memory && batch_key && key_len > 0) {
                        sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
                        if (db) {
                            int64_t now_ts = (int64_t)time(NULL);

                            /* 9. Prospective memory — check triggers from current message */
                            if (combined_len > 0) {
                                hu_prospective_entry_t *prosp_entries = NULL;
                                size_t prosp_count = 0;
                                if (hu_prospective_check_triggers(alloc, db, "keyword", combined,
                                                                 combined_len, batch_key, key_len,
                                                                 now_ts, &prosp_entries,
                                                                 &prosp_count) == HU_OK &&
                                    prosp_entries && prosp_count > 0) {
                                    char prosp_buf[1024];
                                    size_t prosp_pos = 0;
                                    int n = snprintf(prosp_buf, sizeof(prosp_buf),
                                                    "[PROSPECTIVE MEMORY: Remember to: ");
                                    if (n > 0)
                                        prosp_pos = (size_t)n;
                                    for (size_t pi = 0; pi < prosp_count && pi < 3 && prosp_pos < sizeof(prosp_buf) - 64; pi++) {
                                        if (pi > 0) {
                                            prosp_buf[prosp_pos++] = ' ';
                                            prosp_buf[prosp_pos++] = '|';
                                            prosp_buf[prosp_pos++] = ' ';
                                        }
                                        int w = snprintf(prosp_buf + prosp_pos,
                                                        sizeof(prosp_buf) - prosp_pos,
                                                        "%s (triggered by: %s)",
                                                        prosp_entries[pi].action,
                                                        prosp_entries[pi].trigger_value);
                                        if (w > 0 && prosp_pos + (size_t)w < sizeof(prosp_buf))
                                            prosp_pos += (size_t)w;
                                    }
                                    if (prosp_pos + 2 < sizeof(prosp_buf)) {
                                        prosp_buf[prosp_pos++] = ']';
                                        prosp_buf[prosp_pos] = '\0';
                                        char *prosp_str = (char *)alloc->alloc(alloc->ctx, prosp_pos + 1);
                                        if (prosp_str) {
                                            memcpy(prosp_str, prosp_buf, prosp_pos + 1);
                                            PHASE6_APPEND(prosp_str, prosp_pos);
                                        }
                                    }
                                    alloc->free(alloc->ctx, prosp_entries,
                                                prosp_count * sizeof(hu_prospective_entry_t));
                                }
                            }

                            /* 10. Emotional residue — active valence/intensity for this contact */
                            {
                                hu_emotional_residue_t *residues = NULL;
                                size_t res_count = 0;
                                if (hu_emotional_residue_get_active(alloc, db, batch_key, key_len,
                                                                   now_ts, &residues,
                                                                   &res_count) == HU_OK &&
                                    residues && res_count > 0) {
                                    size_t dir_len = 0;
                                    char *dir = hu_emotional_residue_build_directive(
                                        alloc, residues, res_count, &dir_len);
                                    if (dir && dir_len > 0)
                                        PHASE6_APPEND(dir, dir_len);
                                    else if (dir)
                                        alloc->free(alloc->ctx, dir, dir_len + 1);
                                    alloc->free(alloc->ctx, residues,
                                                res_count * sizeof(hu_emotional_residue_t));
                                }
                            }

                            /* 11. Episodic context — last 3 episodes for this contact */
                            {
                                hu_episode_sqlite_t *episodes = NULL;
                                size_t ep_count = 0;
                                if (hu_episode_get_by_contact(alloc, db, batch_key, key_len, 3, 0,
                                                              &episodes, &ep_count) == HU_OK &&
                                    episodes && ep_count > 0) {
                                    char ep_buf[2048];
                                    size_t ep_pos = 0;
                                    int n = snprintf(ep_buf, sizeof(ep_buf),
                                                    "[EPISODIC MEMORY: Recent episodes: ");
                                    if (n > 0)
                                        ep_pos = (size_t)n;
                                    for (size_t ei = 0; ei < ep_count && ep_pos < sizeof(ep_buf) - 64; ei++) {
                                        if (ei > 0) {
                                            ep_buf[ep_pos++] = ' ';
                                            ep_buf[ep_pos++] = ';';
                                            ep_buf[ep_pos++] = ' ';
                                        }
                                        size_t add = episodes[ei].summary_len;
                                        if (add > 120)
                                            add = 120;
                                        if (ep_pos + add + 2 < sizeof(ep_buf)) {
                                            memcpy(ep_buf + ep_pos, episodes[ei].summary, add);
                                            ep_pos += add;
                                        }
                                    }
                                    if (ep_pos + 2 < sizeof(ep_buf)) {
                                        ep_buf[ep_pos++] = ']';
                                        ep_buf[ep_pos] = '\0';
                                        char *ep_str = (char *)alloc->alloc(alloc->ctx, ep_pos + 1);
                                        if (ep_str) {
                                            memcpy(ep_str, ep_buf, ep_pos + 1);
                                            PHASE6_APPEND(ep_str, ep_pos);
                                        }
                                    }
                                    hu_episode_free(alloc, episodes, ep_count);
                                }
                            }
                        }
                    }
#endif

#undef PHASE6_APPEND
                }
#endif /* HU_HAS_PERSONA */

                /* 3. Build awareness context from history via shared analyzer. */
                if (ctx_entries && ctx_count > 0) {
                    char *awareness_ctx = hu_conversation_build_awareness(
                        alloc, ctx_entries, ctx_count,
                        (agent && agent->persona) ? agent->persona : NULL, &convo_ctx_len);

#ifdef HU_HAS_PERSONA
                    /* Prepend Phase 6 prefix (1–8) before awareness (9) */
                    if (phase6_prefix && phase6_len > 0) {
                        if (awareness_ctx && convo_ctx_len > 0) {
                            size_t total = phase6_len + convo_ctx_len + 2;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                            if (merged) {
                                memcpy(merged, phase6_prefix, phase6_len);
                                merged[phase6_len] = '\n';
                                merged[phase6_len + 1] = '\n';
                                memcpy(merged + phase6_len + 2, awareness_ctx, convo_ctx_len);
                                merged[total] = '\0';
                                alloc->free(alloc->ctx, awareness_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total;
                            } else {
                                convo_ctx = awareness_ctx;
                            }
                        } else {
                            convo_ctx = phase6_prefix;
                            convo_ctx_len = phase6_len;
                            awareness_ctx = NULL;
                            phase6_prefix = NULL; /* ownership transferred to convo_ctx */
                        }
                        if (phase6_prefix) {
                            alloc->free(alloc->ctx, phase6_prefix, phase6_len + 1);
                            phase6_prefix = NULL;
                        }
                    } else {
                        convo_ctx = awareness_ctx;
                    }
                    if (phase6_prefix)
                        alloc->free(alloc->ctx, phase6_prefix, phase6_len + 1);
#else
                    convo_ctx = awareness_ctx;
#endif

                    /* F21: Avoidance pattern detection — topic change within same session */
                    if (agent->memory && ctx_count >= 2) {
                        char topic_before[64], topic_after[64];
                        if (hu_conversation_detect_topic_change(ctx_entries, ctx_count,
                                                               topic_before,
                                                               sizeof(topic_before),
                                                               topic_after,
                                                               sizeof(topic_after))) {
                            size_t tb_len = strlen(topic_before);
                            if (tb_len > 0)
                                (void)hu_superhuman_avoidance_record(agent->memory, batch_key,
                                                                     key_len, topic_before,
                                                                     tb_len, true);
                        }
                    }
                    if (merged_history)
                        alloc->free(alloc->ctx, merged_history,
                                    ctx_count * sizeof(hu_channel_history_entry_t));
                }

                /* 2c. Length calibration fallback for channels without history.
                 * When history exists, calibration runs inside build_awareness.
                 * When it doesn't, we still want message-type guidance. */
                if (!convo_ctx && combined_len > 0) {
                    char cal_buf[1024];
                    size_t cal_len = hu_conversation_calibrate_length(combined, combined_len, NULL,
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
                    style_ctx = hu_conversation_analyze_style(
                        alloc, history_entries, history_count,
                        (agent && agent->persona) ? agent->persona : NULL, &style_ctx_len);
                }

                /* F32: Style fingerprint — our texting style with this contact (haha vs lol, etc.) */
                if (agent->memory && batch_key && key_len > 0) {
                    hu_style_fingerprint_t fp;
                    memset(&fp, 0, sizeof(fp));
                    if (hu_style_fingerprint_get(agent->memory, alloc, batch_key, key_len, &fp) ==
                            HU_OK) {
                        char fp_buf[256];
                        size_t fp_len =
                            hu_style_fingerprint_build_directive(&fp, fp_buf, sizeof(fp_buf));
                        if (fp_len > 0) {
                            if (style_ctx && style_ctx_len > 0) {
                                size_t merged_len = style_ctx_len + fp_len + 2;
                                char *merged =
                                    (char *)alloc->alloc(alloc->ctx, merged_len + 1);
                                if (merged) {
                                    memcpy(merged, style_ctx, style_ctx_len);
                                    merged[style_ctx_len] = '\n';
                                    memcpy(merged + style_ctx_len + 1, fp_buf, fp_len);
                                    merged[merged_len - 1] = '\n';
                                    merged[merged_len] = '\0';
                                    alloc->free(alloc->ctx, style_ctx, style_ctx_len + 1);
                                    style_ctx = merged;
                                    style_ctx_len = merged_len;
                                }
                            } else {
                                style_ctx = (char *)alloc->alloc(alloc->ctx, fp_len + 2);
                                if (style_ctx) {
                                    memcpy(style_ctx, fp_buf, fp_len);
                                    style_ctx[fp_len] = '\n';
                                    style_ctx[fp_len + 1] = '\0';
                                    style_ctx_len = fp_len + 1;
                                }
                            }
                        }
                    }
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
                    hu_narrative_phase_t narr =
                        hu_conversation_detect_narrative(history_entries, history_count);
                    hu_engagement_level_t eng =
                        hu_conversation_detect_engagement(history_entries, history_count);
                    hu_emotional_state_t emo =
                        hu_conversation_detect_emotion(history_entries, history_count);

                    bool narr_meaningful =
                        (narr != HU_NARRATIVE_OPENING && narr != HU_NARRATIVE_BUILDING);
                    bool eng_meaningful = (eng != HU_ENGAGEMENT_MODERATE);
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
                            const char *phase_name = (narr == HU_NARRATIVE_CLOSING)   ? "closing"
                                                     : (narr == HU_NARRATIVE_PEAK)    ? "peak"
                                                     : (narr == HU_NARRATIVE_RELEASE) ? "release"
                                                     : (narr == HU_NARRATIVE_APPROACHING_CLIMAX)
                                                         ? "approaching climax"
                                                         : "building";
                            w = snprintf(insights_buf + insights_pos,
                                         sizeof(insights_buf) - insights_pos,
                                         "- Narrative phase: %s\n", phase_name);
                            if (w > 0 && (size_t)w < sizeof(insights_buf) - insights_pos)
                                insights_pos += (size_t)w;
                        }
                        if (eng_meaningful) {
                            const char *eng_name = (eng == HU_ENGAGEMENT_HIGH)  ? "high"
                                                   : (eng == HU_ENGAGEMENT_LOW) ? "low"
                                                   : (eng == HU_ENGAGEMENT_DISTRACTED)
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

                /* F14: Escalation detection — if 3+ consecutive negative messages,
                 * use de-escalation directive (overrides energy). */
                bool use_escalation = false;
                if (history_entries && history_count > 0) {
                    hu_escalation_state_t escalation =
                        hu_conversation_detect_escalation(history_entries, history_count);
                    if (escalation.escalating) {
                        char deesc_buf[256];
                        size_t deesc_len = hu_conversation_build_deescalation_directive(
                            deesc_buf, sizeof(deesc_buf));
                        if (deesc_len > 0) {
                            use_escalation = true;
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + deesc_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, deesc_buf, deesc_len);
                                    merged[convo_ctx_len + 1 + deesc_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, deesc_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, deesc_buf, deesc_len);
                                    convo_ctx[deesc_len] = '\n';
                                    convo_ctx[deesc_len + 1] = '\0';
                                    convo_ctx_len = deesc_len + 1;
                                }
                            }
                            if (agent && agent->bth_metrics)
                                agent->bth_metrics->emotions_surfaced++;
                        }
                    }
                }

                /* F13: Energy matching — detect emotional energy of incoming message,
                 * inject [ENERGY: ...] directive when not neutral. De-escalation overrides. */
                if (!use_escalation && combined_len > 0) {
                    hu_energy_level_t energy = hu_conversation_detect_energy(
                        combined, combined_len, history_entries, history_count);
                    if (energy != HU_ENERGY_NEUTRAL) {
                        char energy_buf[128];
                        size_t energy_len = hu_conversation_build_energy_directive(
                            energy, energy_buf, sizeof(energy_buf));
                        if (energy_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + energy_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, energy_buf, energy_len);
                                    merged[convo_ctx_len + 1 + energy_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, energy_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, energy_buf, energy_len);
                                    convo_ctx[energy_len] = '\n';
                                    convo_ctx[energy_len + 1] = '\0';
                                    convo_ctx_len = energy_len + 1;
                                }
                            }
                            if (agent && agent->bth_metrics)
                                agent->bth_metrics->emotions_surfaced++;
                        }
                    }
                }

                /* F27: Comfort pattern — when emotion is negative, inject learned preference. */
                if (history_entries && history_count > 0 && agent->memory) {
                    hu_emotional_state_t emo_f27 =
                        hu_conversation_detect_emotion(history_entries, history_count);
                    bool emo_negative =
                        emo_f27.concerning || (emo_f27.dominant_emotion &&
                                               (strcmp(emo_f27.dominant_emotion, "sad") == 0 ||
                                                strcmp(emo_f27.dominant_emotion, "stressed") == 0 ||
                                                strcmp(emo_f27.dominant_emotion, "anxious") == 0 ||
                                                strcmp(emo_f27.dominant_emotion, "worried") == 0));
                    if (emo_negative) {
                        const char *emotion_str =
                            emo_f27.dominant_emotion && emo_f27.dominant_emotion[0]
                                ? emo_f27.dominant_emotion
                                : "concerning";
                        char pref_type[32];
                        size_t pref_len = 0;
                        if (hu_comfort_pattern_get_preferred(
                                alloc, agent->memory, batch_key, key_len, emotion_str,
                                strlen(emotion_str), pref_type, sizeof(pref_type),
                                &pref_len) == HU_OK &&
                            pref_len > 0) {
                            char comfort_buf[256];
                            size_t comfort_len = hu_conversation_build_comfort_directive(
                                pref_type, pref_len, emotion_str, strlen(emotion_str), comfort_buf,
                                sizeof(comfort_buf));
                            if (comfort_len > 0) {
                                if (convo_ctx) {
                                    size_t total = convo_ctx_len + comfort_len + 3;
                                    char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                    if (merged) {
                                        memcpy(merged, convo_ctx, convo_ctx_len);
                                        merged[convo_ctx_len] = '\n';
                                        memcpy(merged + convo_ctx_len + 1, comfort_buf,
                                               comfort_len);
                                        merged[convo_ctx_len + 1 + comfort_len] = '\n';
                                        merged[total - 1] = '\0';
                                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                        convo_ctx = merged;
                                        convo_ctx_len = total - 1;
                                    }
                                } else {
                                    convo_ctx = (char *)alloc->alloc(alloc->ctx, comfort_len + 2);
                                    if (convo_ctx) {
                                        memcpy(convo_ctx, comfort_buf, comfort_len);
                                        convo_ctx[comfort_len] = '\n';
                                        convo_ctx[comfort_len + 1] = '\0';
                                        convo_ctx_len = comfort_len + 1;
                                    }
                                }
                            }
                        }
                    }
                }

#ifdef HU_HAS_PERSONA
                /* F16: Context modifiers — heavy topics, personal sharing, high emotion, early turn
                 */
                if (history_entries && history_count > 0) {
                    hu_emotional_state_t emo_ctx =
                        hu_conversation_detect_emotion(history_entries, history_count);
                    const hu_context_modifiers_t *mods =
                        (agent && agent->persona) ? &agent->persona->context_modifiers : NULL;
                    char mod_buf[512];
                    size_t mod_len = hu_conversation_build_context_modifiers(
                        history_entries, history_count, &emo_ctx, mods, mod_buf, sizeof(mod_buf));
                    if (mod_len > 0) {
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + mod_len + 3;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total);
                            if (merged) {
                                memcpy(merged, convo_ctx, convo_ctx_len);
                                merged[convo_ctx_len] = '\n';
                                memcpy(merged + convo_ctx_len + 1, mod_buf, mod_len);
                                merged[convo_ctx_len + 1 + mod_len] = '\n';
                                merged[total - 1] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total - 1;
                            }
                        } else {
                            convo_ctx = (char *)alloc->alloc(alloc->ctx, mod_len + 2);
                            if (convo_ctx) {
                                memcpy(convo_ctx, mod_buf, mod_len);
                                convo_ctx[mod_len] = '\n';
                                convo_ctx[mod_len + 1] = '\0';
                                convo_ctx_len = mod_len + 1;
                            }
                        }
                    }
                }
#endif

                /* F17: First-time vulnerability detection — extra care when they share
                 * something personal for the first time. */
                {
                    hu_vulnerability_state_t vuln =
                        hu_conversation_detect_first_time_vulnerability(
                            combined, combined_len,
                            agent ? agent->memory : NULL,
                            batch_key, key_len);
                    if (vuln.first_time && vuln.topic_category) {
                        char vuln_buf[512];
                        size_t vuln_len = hu_conversation_build_vulnerability_directive(
                            &vuln, vuln_buf, sizeof(vuln_buf));
                        if (vuln_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + vuln_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, vuln_buf, vuln_len);
                                    merged[convo_ctx_len + 1 + vuln_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, vuln_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, vuln_buf, vuln_len);
                                    convo_ctx[vuln_len] = '\n';
                                    convo_ctx[vuln_len + 1] = '\0';
                                    convo_ctx_len = vuln_len + 1;
                                }
                            }
#ifndef HU_IS_TEST
                            /* Record so future calls return first_time=false */
                            if (agent && agent->memory) {
                                size_t tl = strlen(vuln.topic_category);
                                (void)hu_emotional_moment_record(
                                    alloc, agent->memory, batch_key, key_len,
                                    vuln.topic_category, tl, "vulnerable", 9, vuln.intensity);
                            }
#endif
                        }
                    }
                }

                /* F49: Call escalation — when text isn't enough, suggest a call. */
                if (combined_len > 0) {
                    hu_call_escalation_t call_esc = hu_conversation_should_escalate_to_call(
                        combined, combined_len, history_entries, history_count);
                    if (call_esc.should_suggest) {
                        char call_buf[512];
                        size_t call_len = hu_conversation_build_call_directive(
                            combined, combined_len, call_buf, sizeof(call_buf));
                        if (call_len > 0) {
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + call_len + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, call_buf, call_len);
                                    merged[convo_ctx_len + 1 + call_len] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                            } else {
                                convo_ctx = (char *)alloc->alloc(alloc->ctx, call_len + 2);
                                if (convo_ctx) {
                                    memcpy(convo_ctx, call_buf, call_len);
                                    convo_ctx[call_len] = '\n';
                                    convo_ctx[call_len + 1] = '\0';
                                    convo_ctx_len = call_len + 1;
                                }
                            }
                        }
                    }
                }

                /* GraphRAG: inject knowledge graph context (cross-contact synthesis via batch_key)
                 */
#ifdef HU_ENABLE_SQLITE
                if (graph) {
                    char *graph_ctx = NULL;
                    size_t graph_ctx_len = 0;
                    hu_error_t gerr = hu_graph_build_contact_context(
                        graph, alloc, combined, combined_len, batch_key, key_len, 2, 1024,
                        &graph_ctx, &graph_ctx_len);
                    if (gerr == HU_OK && graph_ctx && graph_ctx_len > 0) {
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
                    char *attach_ctx = hu_conversation_attachment_context(
                        alloc, history_entries, history_count, &attach_ctx_len);
#ifndef HU_IS_TEST
#if defined(HU_ENABLE_IMESSAGE)
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
                                hu_imessage_get_latest_attachment_path(alloc, batch_key, key_len);
                            if (img_path) {
                                char *desc = NULL;
                                size_t desc_len = 0;
                                const char *model =
                                    agent->model_name ? agent->model_name : "gpt-4o";
                                size_t model_len = agent->model_name_len > 0 ? agent->model_name_len
                                                                             : strlen(model);
                                hu_error_t verr = hu_vision_describe_image(
                                    alloc, &agent->provider, img_path, strlen(img_path), model,
                                    model_len, &desc, &desc_len);
                                alloc->free(alloc->ctx, img_path, strlen(img_path) + 1);
                                if (verr == HU_OK && desc && desc_len > 0) {
                                    size_t vision_ctx_len = 0;
                                    char *vision_ctx = hu_vision_build_context(
                                        alloc, desc, desc_len, &vision_ctx_len);
                                    alloc->free(alloc->ctx, desc, desc_len + 1);
                                    if (vision_ctx && vision_ctx_len > 0) {
                                        alloc->free(alloc->ctx, attach_ctx, attach_ctx_len + 1);
                                        attach_ctx = vision_ctx;
                                        attach_ctx_len = vision_ctx_len;
#ifndef HU_IS_TEST
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
#ifndef HU_IS_TEST
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
                        thread_cb = hu_conversation_build_callback(alloc, history_entries,
                                                                   history_count, &thread_cb_len);
                    }
                    if (agent->memory && agent->memory->vtable && agent->memory->vtable->recall) {
                        mem_cb = build_callback_context(alloc, agent->memory, batch_key, key_len,
                                                        combined, combined_len, &mem_cb_len, agent);
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
#ifndef HU_IS_TEST
                    if (cb_len > 0 && agent && agent->bth_metrics)
                        agent->bth_metrics->callbacks_triggered++;
#endif
                }

#ifndef HU_IS_TEST
                /* Episodic: load recent sessions for context */
                char *episodic_ctx = NULL;
                size_t episodic_ctx_len = 0;
                char *avoidance_json = NULL;
                size_t avoidance_len = 0;
                if (agent->memory) {
                    hu_episodic_load(agent->memory, alloc, &episodic_ctx, &episodic_ctx_len);
                }
                /* F19: Inside jokes — inject for natural callback opportunities */
                hu_inside_joke_t *jokes_ctx = NULL;
                size_t jokes_count = 0;
                if (agent->memory &&
                    hu_superhuman_inside_joke_list(agent->memory, alloc, batch_key, key_len, 5,
                                                   &jokes_ctx, &jokes_count) == HU_OK &&
                    jokes_ctx && jokes_count > 0) {
                    char jokes_buf[768];
                    size_t jokes_pos = snprintf(jokes_buf, sizeof(jokes_buf),
                                                "Inside jokes with this contact: ");
                    for (size_t j = 0; j < jokes_count && jokes_pos < 700; j++) {
                        if (j > 0)
                            jokes_pos += snprintf(jokes_buf + jokes_pos,
                                                  sizeof(jokes_buf) - jokes_pos, "; ");
                        size_t ctx_len = strnlen(jokes_ctx[j].context, 80);
                        size_t pl_len = strnlen(jokes_ctx[j].punchline, 60);
                        jokes_pos += snprintf(jokes_buf + jokes_pos,
                                              sizeof(jokes_buf) - jokes_pos,
                                              "[%.*s] %.*s", (int)ctx_len, jokes_ctx[j].context,
                                              (int)pl_len, jokes_ctx[j].punchline);
                    }
                    if (jokes_pos < sizeof(jokes_buf) - 32)
                        jokes_pos += snprintf(jokes_buf + jokes_pos,
                                              sizeof(jokes_buf) - jokes_pos,
                                              ". Use naturally when relevant.");
                    if (jokes_pos > 0 && jokes_pos < sizeof(jokes_buf)) {
                        char *jokes_str =
                            (char *)alloc->alloc(alloc->ctx, jokes_pos + 1);
                        if (jokes_str) {
                            memcpy(jokes_str, jokes_buf, jokes_pos);
                            jokes_str[jokes_pos] = '\0';
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + jokes_pos + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, jokes_str, jokes_pos);
                                    merged[convo_ctx_len + 1 + jokes_pos] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                                alloc->free(alloc->ctx, jokes_str, jokes_pos + 1);
                            } else {
                                convo_ctx = jokes_str;
                                convo_ctx_len = jokes_pos;
                            }
                        }
                    }
                    hu_superhuman_inside_joke_free(alloc, jokes_ctx, jokes_count);
                }
                /* F18: Micro-moments — inject notable details for natural reference */
                char *mm_json = NULL;
                size_t mm_len = 0;
                if (agent->memory &&
                    hu_superhuman_micro_moment_list(agent->memory, alloc, batch_key, key_len,
                                                    10, &mm_json, &mm_len) == HU_OK &&
                    mm_json && mm_len > 0) {
                    char mm_buf[1024];
                    int mb = snprintf(mm_buf, sizeof(mm_buf),
                                      "Notable details about this contact: %.*s "
                                      "Reference naturally when relevant.",
                                      (int)(mm_len < 900 ? mm_len : 900), mm_json);
                    if (mb > 0 && (size_t)mb < sizeof(mm_buf)) {
                        char *mm_str = (char *)alloc->alloc(alloc->ctx, (size_t)mb + 1);
                        if (mm_str) {
                            memcpy(mm_str, mm_buf, (size_t)mb);
                            mm_str[mb] = '\0';
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + (size_t)mb + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, mm_str, (size_t)mb);
                                    merged[convo_ctx_len + 1 + (size_t)mb] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                                alloc->free(alloc->ctx, mm_str, (size_t)mb + 1);
                            } else {
                                convo_ctx = mm_str;
                                convo_ctx_len = (size_t)mb;
                            }
                        }
                    }
                    alloc->free(alloc->ctx, mm_json, mm_len);
                }
                /* F21: Avoidance patterns — inject for context, don't push */
                if (agent->memory &&
                    hu_superhuman_avoidance_list(agent->memory, alloc, batch_key, key_len,
                                                 &avoidance_json, &avoidance_len) == HU_OK &&
                    avoidance_json && avoidance_len > 0 &&
                    strstr(avoidance_json, "- ") != NULL) {
                    char avoid_buf[512];
                    int ab = snprintf(avoid_buf, sizeof(avoid_buf),
                                     "Topics they may avoid: %.*s Don't push; use for context.",
                                     (int)(avoidance_len < 400 ? avoidance_len : 400), avoidance_json);
                    if (ab > 0 && (size_t)ab < sizeof(avoid_buf)) {
                        char *avoid_str = (char *)alloc->alloc(alloc->ctx, (size_t)ab + 1);
                        if (avoid_str) {
                            memcpy(avoid_str, avoid_buf, (size_t)ab);
                            avoid_str[ab] = '\0';
                            if (convo_ctx) {
                                size_t total = convo_ctx_len + (size_t)ab + 3;
                                char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                if (merged) {
                                    memcpy(merged, convo_ctx, convo_ctx_len);
                                    merged[convo_ctx_len] = '\n';
                                    memcpy(merged + convo_ctx_len + 1, avoid_str, (size_t)ab);
                                    merged[convo_ctx_len + 1 + (size_t)ab] = '\n';
                                    merged[total - 1] = '\0';
                                    alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                    convo_ctx = merged;
                                    convo_ctx_len = total - 1;
                                }
                                alloc->free(alloc->ctx, avoid_str, (size_t)ab + 1);
                            } else {
                                convo_ctx = avoid_str;
                                convo_ctx_len = (size_t)ab;
                            }
                        }
                    }
                    alloc->free(alloc->ctx, avoidance_json, avoidance_len);
                    avoidance_json = NULL;
                    avoidance_len = 0;
                }
                /* F22: Pattern mirror — inject behavioral patterns for friend+ surfacing */
                {
                    char *pattern_json = NULL;
                    size_t pattern_len = 0;
                    if (agent->memory &&
                        hu_superhuman_pattern_list(agent->memory, alloc, batch_key, key_len, 5,
                                                   &pattern_json, &pattern_len) == HU_OK &&
                        pattern_json && pattern_len > 0 && strstr(pattern_json, "(none)") == NULL) {
                        char pattern_buf[1024];
                        int pb = snprintf(pattern_buf, sizeof(pattern_buf),
                                         "Behavioral patterns observed: %.*s "
                                         "Surface naturally for friend+ contacts.",
                                         (int)(pattern_len < 900 ? pattern_len : 900), pattern_json);
                        if (pb > 0 && (size_t)pb < sizeof(pattern_buf)) {
                            char *pattern_str = (char *)alloc->alloc(alloc->ctx, (size_t)pb + 1);
                            if (pattern_str) {
                                memcpy(pattern_str, pattern_buf, (size_t)pb);
                                pattern_str[pb] = '\0';
                                if (convo_ctx) {
                                    size_t total = convo_ctx_len + (size_t)pb + 3;
                                    char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                    if (merged) {
                                        memcpy(merged, convo_ctx, convo_ctx_len);
                                        merged[convo_ctx_len] = '\n';
                                        memcpy(merged + convo_ctx_len + 1, pattern_str,
                                               (size_t)pb);
                                        merged[convo_ctx_len + 1 + (size_t)pb] = '\n';
                                        merged[total - 1] = '\0';
                                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                        convo_ctx = merged;
                                        convo_ctx_len = total - 1;
                                    }
                                    alloc->free(alloc->ctx, pattern_str, (size_t)pb + 1);
                                } else {
                                    convo_ctx = pattern_str;
                                    convo_ctx_len = (size_t)pb;
                                }
                            }
                        }
                        alloc->free(alloc->ctx, pattern_json, pattern_len);
                    }
                }
                /* F24: Growth celebration — inject recent milestones for natural celebration */
                {
                    char *growth_json = NULL;
                    size_t growth_len = 0;
                    if (agent->memory &&
                        hu_superhuman_growth_list_recent(agent->memory, alloc, batch_key, key_len,
                                                         3, &growth_json, &growth_len) == HU_OK &&
                        growth_json && growth_len > 0 && strstr(growth_json, "(none)") == NULL) {
                        char growth_buf[1024];
                        int gb = snprintf(growth_buf, sizeof(growth_buf),
                                         "Recent growth to celebrate: %.*s "
                                         "Reference naturally when relevant.",
                                         (int)(growth_len < 900 ? growth_len : 900), growth_json);
                        if (gb > 0 && (size_t)gb < sizeof(growth_buf)) {
                            char *growth_str = (char *)alloc->alloc(alloc->ctx, (size_t)gb + 1);
                            if (growth_str) {
                                memcpy(growth_str, growth_buf, (size_t)gb);
                                growth_str[gb] = '\0';
                                if (convo_ctx) {
                                    size_t total = convo_ctx_len + (size_t)gb + 3;
                                    char *merged = (char *)alloc->alloc(alloc->ctx, total);
                                    if (merged) {
                                        memcpy(merged, convo_ctx, convo_ctx_len);
                                        merged[convo_ctx_len] = '\n';
                                        memcpy(merged + convo_ctx_len + 1, growth_str, (size_t)gb);
                                        merged[convo_ctx_len + 1 + (size_t)gb] = '\n';
                                        merged[total - 1] = '\0';
                                        alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                        convo_ctx = merged;
                                        convo_ctx_len = total - 1;
                                    }
                                    alloc->free(alloc->ctx, growth_str, (size_t)gb + 1);
                                } else {
                                    convo_ctx = growth_str;
                                    convo_ctx_len = (size_t)gb;
                                }
                            }
                        }
                        alloc->free(alloc->ctx, growth_json, growth_len);
                    }
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

#ifdef HU_HAS_PERSONA
#ifndef HU_IS_TEST
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
                    hu_channel_response_constraints_t constraints = {0};
                    if (ch->channel->vtable->get_response_constraints(ch->channel->ctx,
                                                                      &constraints) == HU_OK) {
                        max_chars = constraints.max_chars;
                    }
                }

                {
                    int calibrated = hu_conversation_max_response_chars(combined_len);
                    if (calibrated > 0 && (max_chars == 0 || (uint32_t)calibrated < max_chars))
                        max_chars = (uint32_t)calibrated;
                }

                /* Brief mode: force ultra-short response */
                if (brief_mode && max_chars > 50)
                    max_chars = 50;

                /* Honesty guardrail: inject if they asked "did you do X?" */
                {
                    char *honesty = hu_conversation_honesty_check(alloc, combined, combined_len);
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
                    size_t rep_len = hu_conversation_detect_repetition(
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
#ifdef HU_HAS_PERSONA
                if (agent->persona) {
                    const hu_contact_profile_t *cp_rel =
                        hu_persona_find_contact(agent->persona, batch_key, key_len);
                    if (cp_rel && (cp_rel->relationship_stage || cp_rel->warmth_level ||
                                   cp_rel->vulnerability_level)) {
                        char rel_buf[512];
                        size_t rel_len = hu_conversation_calibrate_relationship(
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
                if (hu_conversation_should_share_link(combined, combined_len, history_entries,
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
#ifndef HU_IS_TEST
                    if (agent && agent->bth_metrics)
                        agent->bth_metrics->link_contexts++;
#endif
                }

                /* Pre-populate STM from current message for egraph */
                {
                    hu_fc_result_t fc_pre;
                    memset(&fc_pre, 0, sizeof(fc_pre));
                    (void)hu_fast_capture(alloc, combined, combined_len, &fc_pre);
                    uint64_t ts_ms = (uint64_t)time(NULL) * 1000;
                    hu_error_t fc_err =
                        hu_stm_record_turn(&agent->stm, "user", 4, combined, combined_len, ts_ms);
                    if (fc_err == HU_OK) {
                        size_t last_idx = hu_stm_count(&agent->stm) - 1;
                        if (fc_pre.primary_topic && fc_pre.primary_topic[0]) {
                            (void)hu_stm_turn_set_primary_topic(&agent->stm, last_idx,
                                                                fc_pre.primary_topic,
                                                                strlen(fc_pre.primary_topic));
                        }
                        for (size_t ei = 0; ei < fc_pre.emotion_count; ei++) {
                            (void)hu_stm_turn_add_emotion(&agent->stm, last_idx,
                                                          fc_pre.emotions[ei].tag,
                                                          fc_pre.emotions[ei].intensity);
                        }
                    }
#ifndef HU_IS_TEST
                    /* F25: Record emotional moments for 1–3 day check-ins */
                    if (agent->memory && history_entries && history_count > 0) {
                        hu_emotional_state_t emo_rec =
                            hu_conversation_detect_emotion(history_entries, history_count);
                        hu_escalation_state_t esc_rec =
                            hu_conversation_detect_escalation(history_entries, history_count);
                        hu_energy_level_t energy_rec = hu_conversation_detect_energy(
                            combined, combined_len, history_entries, history_count);
                        bool should_record = emo_rec.concerning || esc_rec.escalating ||
                                             energy_rec == HU_ENERGY_SAD ||
                                             energy_rec == HU_ENERGY_ANXIOUS ||
                                             (emo_rec.dominant_emotion &&
                                              (strcmp(emo_rec.dominant_emotion, "sad") == 0 ||
                                               strcmp(emo_rec.dominant_emotion, "stressed") == 0 ||
                                               strcmp(emo_rec.dominant_emotion, "anxious") == 0 ||
                                               strcmp(emo_rec.dominant_emotion, "worried") == 0));
                        if (should_record) {
                            const char *topic_str =
                                (fc_pre.primary_topic && fc_pre.primary_topic[0])
                                    ? fc_pre.primary_topic
                                    : (combined_len > 0 ? combined : "something you shared");
                            size_t topic_len;
                            if (fc_pre.primary_topic && fc_pre.primary_topic[0]) {
                                topic_len = strlen(fc_pre.primary_topic);
                            } else if (combined_len > 0) {
                                topic_len = combined_len > 255 ? 255 : combined_len;
                            } else {
                                topic_len = 20; /* "something you shared" */
                            }
                            const char *emotion_str =
                                emo_rec.dominant_emotion && emo_rec.dominant_emotion[0]
                                    ? emo_rec.dominant_emotion
                                    : (esc_rec.escalating ? "escalating" : "concerning");
                            (void)hu_emotional_moment_record(
                                alloc, agent->memory, batch_key, key_len, topic_str, topic_len,
                                emotion_str, strlen(emotion_str), emo_rec.intensity);
                        }
                    }
#endif
                    hu_fc_result_deinit(&fc_pre, alloc);
                }

                /* 7. Emotional topic map: topics → dominant emotions from STM */
                {
                    hu_emotional_graph_t egraph;
                    size_t egraph_len = 0;
                    hu_egraph_init(&egraph, *alloc);
                    (void)hu_egraph_populate_from_stm(&egraph, &agent->stm);
                    char *egraph_ctx = hu_egraph_build_context(alloc, &egraph, &egraph_len);
                    hu_egraph_deinit(&egraph);
                    if (egraph_ctx && egraph_len > 0) {
#ifndef HU_IS_TEST
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
                        hu_mood_build_context(alloc, agent->memory, batch_key, key_len, &mood_ctx,
                                              &mood_ctx_len) == HU_OK &&
                        mood_ctx && mood_ctx_len > 0) {
#ifndef HU_IS_TEST
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
#ifndef HU_IS_TEST
                {
                    hu_conversation_plan_t plan;
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
                    if (hu_plan_conversation(alloc, combined, combined_len, hist_text, hist_len,
                                             emotional_ctx, emotional_len, &plan) == HU_OK) {
                        char *plan_ctx = NULL;
                        size_t plan_ctx_len = 0;
                        if (hu_plan_build_prompt(&plan, alloc, &plan_ctx, &plan_ctx_len) == HU_OK &&
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
                        hu_conversation_plan_deinit(&plan, alloc);
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
                    if (tom_idx == (size_t)-1 && tom_contact_count < HU_TOM_MAX_CONTACTS &&
                        key_len < 64) {
                        tom_idx = tom_contact_count;
                        memset(&tom_states[tom_idx], 0, sizeof(hu_belief_state_t));
                        hu_tom_init(&tom_states[tom_idx], alloc, batch_key, key_len);
                        memcpy(tom_contact_keys[tom_idx], batch_key, key_len);
                        tom_contact_keys[tom_idx][key_len] = '\0';
                        tom_contact_count++;
                    }
                    if (tom_idx != (size_t)-1 && tom_states[tom_idx].belief_count > 0) {
                        char *tom_ctx = NULL;
                        size_t tom_ctx_len = 0;
                        if (hu_tom_build_context(&tom_states[tom_idx], alloc, &tom_ctx,
                                                 &tom_ctx_len) == HU_OK &&
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
                    hu_info_asymmetry_t asym;
                    memset(&asym, 0, sizeof(asym));
                    const char *agent_ctx = convo_ctx ? convo_ctx : "";
                    size_t agent_ctx_len = convo_ctx ? convo_ctx_len : 0;
                    const char *ct_ctx = contact_ctx ? contact_ctx : "";
                    size_t ct_ctx_len = contact_ctx ? contact_ctx_len : 0;
                    if (hu_info_asymmetry_analyze(&asym, alloc, agent_ctx, agent_ctx_len, ct_ctx,
                                                  ct_ctx_len) == HU_OK &&
                        asym.gap_count > 0) {
                        char *ia_ctx = NULL;
                        size_t ia_ctx_len = 0;
                        if (hu_info_asymmetry_build_guidance(&asym, alloc, &ia_ctx, &ia_ctx_len) ==
                                HU_OK &&
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
                        hu_info_asymmetry_deinit(&asym, alloc);
                    }
                }

                /* ── BTH Tier 1: Anticipatory actions from GraphRAG (t1d) ──── */
                if (graph) {
                    hu_anticipatory_result_t antic;
                    memset(&antic, 0, sizeof(antic));
                    int64_t now_ts = (int64_t)time(NULL);
                    if (hu_anticipatory_analyze(graph, alloc, batch_key, key_len, now_ts, &antic) ==
                            HU_OK &&
                        antic.action_count > 0) {
                        char *antic_ctx = NULL;
                        size_t antic_ctx_len = 0;
                        if (hu_anticipatory_build_context(&antic, alloc, &antic_ctx,
                                                          &antic_ctx_len) == HU_OK &&
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
                        hu_anticipatory_result_deinit(&antic, alloc);
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
                    if (vm_idx == (size_t)-1 && voice_contact_count < HU_TOM_MAX_CONTACTS &&
                        key_len < 64) {
                        vm_idx = voice_contact_count;
                        hu_voice_profile_init(&voice_profiles[vm_idx]);
                        memcpy(voice_contact_keys[vm_idx], batch_key, key_len);
                        voice_contact_keys[vm_idx][key_len] = '\0';
                        voice_contact_count++;
                    }
                    if (vm_idx != (size_t)-1) {
                        char *vm_ctx = NULL;
                        size_t vm_ctx_len = 0;
                        if (hu_voice_build_guidance(&voice_profiles[vm_idx], alloc, &vm_ctx,
                                                    &vm_ctx_len) == HU_OK &&
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
                    static const char HU_DEFAULT_TIME_OV_LATE_NIGHT[] =
                        "\nIt's late at night. You can be more relaxed, introspective, "
                        "slightly more open than during the day. If it feels natural, share "
                        "something personal or vulnerable. Late-night texts are more intimate.";
                    static const char HU_DEFAULT_TIME_OV_EARLY_MORNING[] =
                        "\nIt's early morning — you just woke up. Keep responses brief, "
                        "practical, slightly groggy. Short sentences. No deep philosophical "
                        "conversations yet.";
                    if (bth_hour >= 22 || (bth_hour >= 0 && bth_hour < 1)) {
                        tod_overlay =
                            (agent && agent->persona && agent->persona->time_overlay_late_night)
                                ? agent->persona->time_overlay_late_night
                                : HU_DEFAULT_TIME_OV_LATE_NIGHT;
                        tod_len =
                            (agent && agent->persona && agent->persona->time_overlay_late_night)
                                ? strlen(tod_overlay)
                                : sizeof(HU_DEFAULT_TIME_OV_LATE_NIGHT) - 1;
                    } else if (bth_hour >= 7 && bth_hour < 9) {
                        tod_overlay =
                            (agent && agent->persona && agent->persona->time_overlay_early_morning)
                                ? agent->persona->time_overlay_early_morning
                                : HU_DEFAULT_TIME_OV_EARLY_MORNING;
                        tod_len =
                            (agent && agent->persona && agent->persona->time_overlay_early_morning)
                                ? strlen(tod_overlay)
                                : sizeof(HU_DEFAULT_TIME_OV_EARLY_MORNING) - 1;
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
                    sent_ctx = hu_conversation_build_sentiment_momentum(
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
                    tang_ctx = hu_conversation_build_tangent_callback(
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
                    depth_ctx = hu_conversation_build_depth_signal(alloc, history_entries,
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

                /* Prepend group-chat instruction when responding in a group */
                if (msgs[batch_start].is_group) {
                    const char *hint = HU_GROUP_CHAT_PROMPT_HINT;
                    size_t hint_len = sizeof(HU_GROUP_CHAT_PROMPT_HINT) - 1;
                    size_t new_len = hint_len + (convo_ctx ? convo_ctx_len : 0);
                    char *new_ctx = (char *)alloc->alloc(alloc->ctx, new_len + 1);
                    if (new_ctx) {
                        memcpy(new_ctx, hint, hint_len);
                        if (convo_ctx && convo_ctx_len > 0)
                            memcpy(new_ctx + hint_len, convo_ctx, convo_ctx_len);
                        new_ctx[new_len] = '\0';
                        if (convo_ctx)
                            alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                        convo_ctx = new_ctx;
                        convo_ctx_len = new_len;
                    }
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

                /* F29: Backchannel — send brief cue and skip LLM when narrative detected */
                if (use_backchannel && backchannel_len > 0 && ch->channel->vtable->send) {
                    ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len, backchannel_buf,
                                              backchannel_len, NULL, 0);
                    fprintf(stderr, "[human] backchannel: %.*s\n", (int)backchannel_len,
                            backchannel_buf);
                    goto skip_llm_this_batch;
                }

                /* Start typing indicator before LLM call */
                if (ch->channel->vtable->start_typing) {
                    ch->channel->vtable->start_typing(ch->channel->ctx, batch_key, key_len);
                }

                /* Thinking response: send filler first if message warrants it */
                {
                    hu_thinking_response_t thinking;
                    memset(&thinking, 0, sizeof(thinking));
                    bool needs_thinking = hu_conversation_classify_thinking(
                        combined, combined_len, history_entries, history_count, &thinking,
                        (uint32_t)time(NULL));
                    if (needs_thinking && thinking.filler_len > 0) {
                        ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len,
                                                  thinking.filler, thinking.filler_len, NULL, 0);
#ifndef HU_IS_TEST
                        if (agent && agent->bth_metrics)
                            agent->bth_metrics->thinking_responses++;
                        usleep((useconds_t)(thinking.delay_ms * 1000));
#endif
                    }
                }

                /* Tapback-vs-text decision: gate reaction and/or LLM flow */
                {
                    hu_tapback_decision_t tapback_decision =
                        hu_conversation_classify_tapback_decision(combined, combined_len,
                                                                  history_entries, history_count,
#ifdef HU_HAS_PERSONA
                                                                  contact_for_tapback,
#else
                                                                  NULL,
#endif
                                                                  (uint32_t)time(NULL));

                    if (tapback_decision == HU_NO_RESPONSE) {
                        fprintf(stderr, "[human] tapback decision: no response for %.*s\n",
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                        goto skip_llm_this_batch;
                    }

                    if (tapback_decision == HU_TAPBACK_ONLY && ch->channel->vtable->react) {
                        hu_reaction_type_t reaction = HU_REACTION_NONE;
                        const char *vision_desc = NULL;
                        size_t vision_desc_len = 0;
                        if (hu_conversation_extract_vision_description(
                                combined, combined_len, &vision_desc, &vision_desc_len) &&
                            vision_desc && vision_desc_len > 0) {
                            reaction = hu_conversation_classify_photo_reaction(
                                vision_desc, vision_desc_len,
#ifdef HU_HAS_PERSONA
                                contact_for_tapback,
#else
                                NULL,
#endif
                                (uint32_t)time(NULL));
                        }
                        if (reaction == HU_REACTION_NONE) {
                            reaction = hu_conversation_classify_reaction(
                                combined, combined_len, false, history_entries, history_count,
                                (uint32_t)time(NULL));
                        }
                        if (reaction != HU_REACTION_NONE) {
                            int64_t msg_id = msgs[batch_end].message_id;
                            if (msg_id > 0) {
                                ch->channel->vtable->react(ch->channel->ctx, batch_key, key_len,
                                                           msg_id, reaction);
                                if (agent->bth_metrics)
                                    agent->bth_metrics->reactions_sent++;
                            }
                        }
                        fprintf(stderr, "[human] tapback only (no text) for %.*s\n",
                                (int)(combined_len > 40 ? 40 : combined_len), combined);
                        goto skip_llm_this_batch;
                    }

                    if (tapback_decision == HU_TAPBACK_AND_TEXT && ch->channel->vtable->react) {
                        hu_reaction_type_t reaction = HU_REACTION_NONE;
                        const char *vision_desc_txt = NULL;
                        size_t vision_desc_txt_len = 0;
                        if (hu_conversation_extract_vision_description(
                                combined, combined_len, &vision_desc_txt, &vision_desc_txt_len) &&
                            vision_desc_txt && vision_desc_txt_len > 0) {
                            reaction = hu_conversation_classify_photo_reaction(
                                vision_desc_txt, vision_desc_txt_len,
#ifdef HU_HAS_PERSONA
                                contact_for_tapback,
#else
                                NULL,
#endif
                                (uint32_t)time(NULL));
                        }
                        if (reaction == HU_REACTION_NONE) {
                            reaction = hu_conversation_classify_reaction(
                                combined, combined_len, false, history_entries, history_count,
                                (uint32_t)time(NULL));
                        }
                        if (reaction != HU_REACTION_NONE) {
                            int64_t msg_id = msgs[batch_end].message_id;
                            if (msg_id > 0) {
                                ch->channel->vtable->react(ch->channel->ctx, batch_key, key_len,
                                                           msg_id, reaction);
                                if (agent->bth_metrics)
                                    agent->bth_metrics->reactions_sent++;
                            }
                        }
                    }
                }
#endif

                /* F45: Burst messaging — 3–4 rapid-fire thoughts for urgent/exciting context */
#ifndef HU_IS_TEST
#ifdef HU_HAS_PERSONA
                {
                    float burst_prob = 0.03f;
                    if (agent && agent->persona)
                        burst_prob = agent->persona->humanization.burst_message_probability;
                    uint32_t burst_seed =
                        (uint32_t)time(NULL) * 1103515245u + 12345u + (uint32_t)(uintptr_t)combined;
                    if (hu_conversation_should_burst(combined, combined_len, history_entries,
                                                     history_count, burst_seed, burst_prob)) {
                        char *burst_convo = NULL;
                        size_t burst_convo_len = 0;
                        char burst_buf[512];
                        size_t burst_len =
                            hu_conversation_build_burst_prompt(burst_buf, sizeof(burst_buf));
                        if (burst_len > 0) {
                            burst_convo_len = burst_len + 1 + (convo_ctx ? convo_ctx_len : 0) + 1;
                            burst_convo = (char *)alloc->alloc(alloc->ctx, burst_convo_len);
                            if (burst_convo) {
                                memcpy(burst_convo, burst_buf, burst_len);
                                burst_convo[burst_len] = '\n';
                                if (convo_ctx && convo_ctx_len > 0)
                                    memcpy(burst_convo + burst_len + 1, convo_ctx, convo_ctx_len);
                                burst_convo[burst_convo_len - 1] = '\0';
                                agent->conversation_context = burst_convo;
                                agent->conversation_context_len = burst_convo_len - 1;
                            }
                        }
                        if (ch->channel->vtable->start_typing)
                            ch->channel->vtable->start_typing(ch->channel->ctx, batch_key, key_len);
                        char *burst_response = NULL;
                        size_t burst_response_len = 0;
                        hu_error_t burst_err = hu_agent_turn(agent, combined, combined_len,
                                                             &burst_response, &burst_response_len);
                        if (ch->channel->vtable->stop_typing)
                            ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                        if (burst_err == HU_OK && burst_response && burst_response_len > 0 &&
                            ch->channel->vtable->send) {
                            char burst_msgs[4][256];
                            int n = hu_conversation_parse_burst_response(
                                burst_response, burst_response_len, burst_msgs, 4);
                            for (int bi = 0; bi < n; bi++) {
                                if (burst_msgs[bi][0]) {
                                    ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len,
                                                              burst_msgs[bi],
                                                              strlen(burst_msgs[bi]), NULL, 0);
                                    if (bi < n - 1) {
                                        unsigned int delay_ms =
                                            1000u + (burst_seed + (uint32_t)bi) % 2000u;
                                        hu_platform_sleep_ms(delay_ms);
                                    }
                                }
                            }
                            fprintf(stderr, "[human] burst: %d messages for %.*s\n", n,
                                    (int)(key_len > 20 ? 20 : key_len), batch_key);
                        }
                        if (burst_response)
                            agent->alloc->free(agent->alloc->ctx, burst_response,
                                               burst_response_len + 1);
                        if (burst_convo) {
                            alloc->free(alloc->ctx, burst_convo, burst_convo_len);
                            agent->conversation_context = convo_ctx;
                            agent->conversation_context_len = convo_ctx_len;
                        }
                        goto skip_llm_this_batch;
                    }
                }
#endif
#endif

                bool retried = false;
                fprintf(stderr, "[human] calling agent turn for %.*s...\n",
                        (int)(key_len > 20 ? 20 : key_len), batch_key);
                do {
                    if (response) {
                        agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                        response = NULL;
                        response_len = 0;
                    }
                    err = hu_agent_turn(agent, combined, combined_len, &response, &response_len);
                    fprintf(stderr, "[human] agent turn result: err=%d response_len=%zu for %.*s\n",
                            (int)err, response_len, (int)(key_len > 20 ? 20 : key_len), batch_key);
                    if (err != HU_OK)
                        fprintf(stderr, "[human] agent turn failed for %.*s: %d\n", (int)key_len,
                                batch_key, (int)err);

#ifndef HU_IS_TEST
                    /* Stop typing indicator after LLM call */
                    if (ch->channel->vtable->stop_typing) {
                        ch->channel->vtable->stop_typing(ch->channel->ctx, batch_key, key_len);
                    }

                    /* Quality gate: check response for unnatural patterns.
                     * If needs_revision, retry once with hint. */
                    if (err == HU_OK && response && response_len > 0 && history_entries) {
                        hu_quality_score_t qscore = hu_conversation_evaluate_quality(
                            response, response_len, history_entries, history_count, max_chars);
                        if (qscore.needs_revision && !retried) {
                            retried = true;
                            fprintf(stderr,
                                    "[human] quality retry: score=%d (b=%d v=%d w=%d n=%d) "
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
                                    "[human] quality warning: score=%d (b=%d v=%d w=%d n=%d) "
                                    "for %.40s...\n",
                                    qscore.total, qscore.brevity, qscore.validation, qscore.warmth,
                                    qscore.naturalness, response_len > 40 ? response : response);
                        }
                    }
                    break;
                } while (1);

#ifdef HU_HAS_PERSONA
                /* Replay learning: analyze conversation and store insights for future prompts */
                if (history_entries && history_count > 0) {
                    hu_replay_result_t replay = {0};
                    hu_error_t rerr =
                        hu_replay_analyze(alloc, history_entries, history_count, 2000, &replay);
                    if (rerr == HU_OK) {
#ifndef HU_IS_TEST
                        if (agent->bth_metrics)
                            agent->bth_metrics->replay_analyses++;
#endif
                        size_t rctx_len = 0;
                        char *rctx = hu_replay_build_context(alloc, &replay, &rctx_len);
                        if (rctx && rctx_len > 0) {
#ifndef HU_IS_TEST
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
                                hu_memory_category_t cat = {
                                    .tag = HU_MEMORY_CATEGORY_CUSTOM,
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
                    hu_replay_result_deinit(&replay, alloc);
                }
#endif
#endif

                /* ── BTH post-turn: Theory of Mind record (t1b-post) ──────── */
#ifndef HU_IS_TEST
                if (err == HU_OK && response && response_len > 0) {
                    size_t tom_idx = (size_t)-1;
                    for (size_t ti = 0; ti < tom_contact_count; ti++) {
                        if (strncmp(tom_contact_keys[ti], batch_key, key_len) == 0 &&
                            tom_contact_keys[ti][key_len] == '\0') {
                            tom_idx = ti;
                            break;
                        }
                    }
                    if (tom_idx != (size_t)-1) {
                        hu_fc_result_t fc_tom;
                        memset(&fc_tom, 0, sizeof(fc_tom));
                        (void)hu_fast_capture(alloc, combined, combined_len, &fc_tom);
                        if (fc_tom.primary_topic && fc_tom.primary_topic[0]) {
                            (void)hu_tom_record_belief(
                                &tom_states[tom_idx], alloc, fc_tom.primary_topic,
                                strlen(fc_tom.primary_topic), HU_BELIEF_KNOWS, 0.8f);
                        }
                        for (size_t ei = 0; ei < fc_tom.entity_count && ei < 3; ei++) {
                            if (fc_tom.entities[ei].name[0]) {
                                (void)hu_tom_record_belief(
                                    &tom_states[tom_idx], alloc, fc_tom.entities[ei].name,
                                    strlen(fc_tom.entities[ei].name), HU_BELIEF_KNOWS, 0.6f);
                            }
                        }
                        hu_fc_result_deinit(&fc_tom, alloc);
                    }
                }

                /* ── BTH post-turn: Voice maturity update (t1e-post) ──────── */
                if (err == HU_OK && response && response_len > 0) {
                    size_t vm_idx = (size_t)-1;
                    for (size_t vi = 0; vi < voice_contact_count; vi++) {
                        if (strncmp(voice_contact_keys[vi], batch_key, key_len) == 0 &&
                            voice_contact_keys[vi][key_len] == '\0') {
                            vm_idx = vi;
                            break;
                        }
                    }
                    if (vm_idx != (size_t)-1) {
                        hu_fc_result_t fc_vm;
                        memset(&fc_vm, 0, sizeof(fc_vm));
                        (void)hu_fast_capture(alloc, combined, combined_len, &fc_vm);
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
                        hu_voice_profile_update(&voice_profiles[vm_idx], had_emotion, had_topic,
                                                had_humor);
                        hu_fc_result_deinit(&fc_vm, alloc);
                    }
                }

                /* ── Phase 6 post-turn: ToM baseline, self-awareness, reciprocity (F58, F62, F63) ── */
#ifdef HU_HAS_PERSONA
#ifndef HU_IS_TEST
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    if (history_entries && history_count > 0) {
                        (void)hu_theory_of_mind_update_baseline(agent->memory, alloc, batch_key,
                                                                key_len, history_entries,
                                                                history_count);
                    }
                    bool we_initiated = (history_count > 0 && history_entries &&
                                         history_entries[history_count - 1].from_me);
                    char topic_buf[128] = {0};
                    size_t topic_len = 0;
                    if (combined_len > 0 && combined_len < sizeof(topic_buf)) {
                        size_t copy = combined_len;
                        if (copy > 80)
                            copy = 80;
                        memcpy(topic_buf, combined, copy);
                        topic_buf[copy] = '\0';
                        topic_len = strlen(topic_buf);
                    }
                    (void)hu_self_awareness_record_send(alloc, agent->memory, batch_key, key_len,
                                                       we_initiated,
                                                       topic_len > 0 ? topic_buf : NULL, topic_len);
                    bool we_asked = (response && (memchr(response, '?', response_len) != NULL));
                    bool we_shared = (response_len > 40);
                    (void)hu_self_awareness_update_reciprocity(alloc, agent->memory, batch_key,
                                                              key_len, we_initiated, we_asked,
                                                              we_shared);
                    /* Optional: set mood from life_sim state when notable */
                    if (agent->persona &&
                        (agent->persona->daily_routine.weekday_count > 0 ||
                         agent->persona->daily_routine.weekend_count > 0)) {
                        time_t now_ts = time(NULL);
                        struct tm tm_buf;
                        struct tm *lt = hu_platform_localtime_r(&now_ts, &tm_buf);
                        int dow = lt ? lt->tm_wday : 0;
                        uint32_t seed = (uint32_t)now_ts * 1103515245u + 12345u;
                        hu_life_sim_state_t ls = hu_life_sim_get_current(
                            &agent->persona->daily_routine, (int64_t)now_ts, dow, seed);
                        if (ls.mood_modifier && strcmp(ls.mood_modifier, "neutral") != 0) {
                            if (strcmp(ls.mood_modifier, "tired") == 0)
                                (void)hu_mood_set(alloc, agent->memory, HU_MOOD_TIRED, 0.4f,
                                                 "life_sim", 8);
                            else if (strcmp(ls.mood_modifier, "stressed") == 0)
                                (void)hu_mood_set(alloc, agent->memory, HU_MOOD_STRESSED, 0.4f,
                                                 "life_sim", 8);
                            else if (strcmp(ls.mood_modifier, "energetic") == 0 ||
                                     strcmp(ls.mood_modifier, "energetic_after") == 0)
                                (void)hu_mood_set(alloc, agent->memory, HU_MOOD_ENERGIZED, 0.4f,
                                                 "life_sim", 8);
                        }
                    }
                }
#endif
#endif
#endif

                /* Phase 7: Post-conversation episode creation (>= 5 messages) */
#ifdef HU_ENABLE_SQLITE
                if (err == HU_OK && response && response_len > 0 && agent->memory &&
                    history_count >= 3 && batch_key && key_len > 0) {
                    sqlite3 *db = hu_sqlite_memory_get_db(agent->memory);
                    if (db) {
                        char summary_buf[1024];
                        size_t sum_len = 0;
                        int n = snprintf(summary_buf, sizeof(summary_buf), "User: %.*s. Assistant: %.*s",
                                         (int)(combined_len > 200 ? 200 : combined_len), combined,
                                         (int)(response_len > 200 ? 200 : response_len), response);
                        if (n > 0 && (size_t)n < sizeof(summary_buf))
                            sum_len = (size_t)n;
                        if (sum_len > 0) {
                            int64_t episode_id = 0;
                            (void)hu_episode_store_insert(alloc, db, batch_key, key_len,
                                                          summary_buf, sum_len,
                                                          NULL, 0, NULL, 0, 0.5,
                                                          "conversation", 12, &episode_id);
                        }
                    }
                }
#endif

                /* ── BTH post-turn: Graph recall tracking + reconsolidate (t1f) */
                if (err == HU_OK && response && response_len > 0 && graph) {
                    hu_deep_extract_result_t de_light;
                    memset(&de_light, 0, sizeof(de_light));
                    if (hu_deep_extract_lightweight(alloc, combined, combined_len, &de_light) ==
                        HU_OK) {
                        for (size_t fi = 0; fi < de_light.fact_count; fi++) {
                            if (!de_light.facts[fi].subject)
                                continue;
                            hu_graph_entity_t ent;
                            memset(&ent, 0, sizeof(ent));
                            if (hu_graph_find_entity(graph, de_light.facts[fi].subject,
                                                     strlen(de_light.facts[fi].subject),
                                                     &ent) == HU_OK &&
                                ent.id > 0) {
                                (void)hu_graph_record_recall(graph, ent.id);
                                if (de_light.facts[fi].object) {
                                    (void)hu_graph_reconsolidate(graph, alloc,
                                                                 de_light.facts[fi].subject,
                                                                 strlen(de_light.facts[fi].subject),
                                                                 de_light.facts[fi].object,
                                                                 strlen(de_light.facts[fi].object));
                                }
                            }
                        }
                        hu_deep_extract_result_deinit(&de_light, alloc);
                    }
                }

                /* ── BTH post-turn: LLM deep extraction (t1g) ─────────────── */
                if (err == HU_OK && response && response_len > 0 && agent->memory && graph) {
                    char convo_buf[4096];
                    int cb_w = snprintf(convo_buf, sizeof(convo_buf), "User: %.*s\nAssistant: %.*s",
                                        (int)combined_len, combined, (int)response_len, response);
                    if (cb_w > 0 && (size_t)cb_w < sizeof(convo_buf)) {
                        char *de_prompt = NULL;
                        size_t de_prompt_len = 0;
                        if (hu_deep_extract_build_prompt(alloc, convo_buf, (size_t)cb_w, &de_prompt,
                                                         &de_prompt_len) == HU_OK &&
                            de_prompt && de_prompt_len > 0) {
                            char *de_response = NULL;
                            size_t de_response_len = 0;
                            hu_error_t de_err = hu_agent_turn(agent, de_prompt, de_prompt_len,
                                                              &de_response, &de_response_len);
                            if (de_err == HU_OK && de_response && de_response_len > 0) {
                                hu_deep_extract_result_t de_result;
                                memset(&de_result, 0, sizeof(de_result));
                                if (hu_deep_extract_parse(alloc, de_response, de_response_len,
                                                          &de_result) == HU_OK) {
                                    for (size_t ri = 0; ri < de_result.relation_count; ri++) {
                                        if (!de_result.relations[ri].entity_a ||
                                            !de_result.relations[ri].entity_b)
                                            continue;
                                        int64_t src_id = 0, tgt_id = 0;
                                        (void)hu_graph_upsert_entity(
                                            graph, de_result.relations[ri].entity_a,
                                            strlen(de_result.relations[ri].entity_a),
                                            HU_ENTITY_UNKNOWN, NULL, &src_id);
                                        (void)hu_graph_upsert_entity(
                                            graph, de_result.relations[ri].entity_b,
                                            strlen(de_result.relations[ri].entity_b),
                                            HU_ENTITY_UNKNOWN, NULL, &tgt_id);
                                        if (src_id > 0 && tgt_id > 0) {
                                            const char *rel_str =
                                                de_result.relations[ri].relation
                                                    ? de_result.relations[ri].relation
                                                    : "";
                                            (void)hu_graph_upsert_relation(
                                                graph, src_id, tgt_id, HU_REL_RELATED_TO,
                                                (float)de_result.relations[ri].confidence, rel_str,
                                                strlen(rel_str));
                                        }
                                    }
                                    hu_deep_extract_result_deinit(&de_result, alloc);
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

            skip_llm_this_batch:
                /* Clear per-turn context and free */
#ifndef HU_IS_TEST
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
                                history_count * sizeof(hu_channel_history_entry_t));
                if (cross_entries)
                    alloc->free(alloc->ctx, cross_entries,
                                cross_count * sizeof(hu_channel_history_entry_t));
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
                    if (err == HU_OK && response && response_len > 0) {
                        agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                                   batch_key, key_len, "assistant",
                                                                   9, response, response_len);
                    }
                }

                /* Store conversation summary as long-term memory */
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    store_conversation_summary(alloc, agent->memory, graph, agent, batch_key,
                                               key_len, combined, combined_len, response,
                                               response_len);
                }

#ifdef HU_ENABLE_SQLITE
                /* Task 18: Extraction pipeline — post-turn storage */
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    (void)hu_superhuman_extract_and_store(agent->memory, alloc, batch_key, key_len,
                        combined, combined_len, response, response_len, NULL, 0);
                }
#endif

#ifndef HU_IS_TEST
                /* F27: If we responded to negative emotion, set pending to record engagement
                 * when we get their next reply. */
                if (err == HU_OK && response && response_len > 0 && agent->memory &&
                    history_entries && history_count > 0) {
                    hu_emotional_state_t emo_pend =
                        hu_conversation_detect_emotion(history_entries, history_count);
                    bool should_pend = emo_pend.concerning ||
                                       (emo_pend.dominant_emotion &&
                                        (strcmp(emo_pend.dominant_emotion, "sad") == 0 ||
                                         strcmp(emo_pend.dominant_emotion, "stressed") == 0 ||
                                         strcmp(emo_pend.dominant_emotion, "anxious") == 0 ||
                                         strcmp(emo_pend.dominant_emotion, "worried") == 0));
                    if (should_pend && key_len < sizeof(comfort_pending[0].key)) {
                        char resp_type[32];
                        classify_comfort_response_type(response, response_len, resp_type,
                                                       sizeof(resp_type));
                        const char *emotion_str =
                            emo_pend.dominant_emotion && emo_pend.dominant_emotion[0]
                                ? emo_pend.dominant_emotion
                                : "concerning";
                        size_t slot = HU_COMFORT_PENDING_MAX;
                        for (size_t cp_i = 0; cp_i < HU_COMFORT_PENDING_MAX; cp_i++) {
                            if (comfort_pending[cp_i].key[0] == '\0' ||
                                (memcmp(comfort_pending[cp_i].key, batch_key, key_len) == 0 &&
                                 comfort_pending[cp_i].key[key_len] == '\0')) {
                                slot = cp_i;
                                break;
                            }
                        }
                        if (slot < HU_COMFORT_PENDING_MAX) {
                            memcpy(comfort_pending[slot].key, batch_key, key_len);
                            comfort_pending[slot].key[key_len] = '\0';
                            snprintf(comfort_pending[slot].emotion,
                                     sizeof(comfort_pending[slot].emotion), "%s", emotion_str);
                            snprintf(comfort_pending[slot].response_type,
                                     sizeof(comfort_pending[slot].response_type), "%s", resp_type);
                        }
                    }
                }

                /* Episodic: summarize this interaction */
                if (err == HU_OK && response && response_len > 0 && agent->memory) {
                    const char *ep_msgs[2] = {combined, response};
                    size_t ep_lens[2] = {combined_len, response_len};
                    size_t summary_len = 0;
                    char *summary =
                        hu_episodic_summarize_session(alloc, ep_msgs, ep_lens, 2, &summary_len);
                    if (summary && summary_len > 0) {
                        if (hu_episodic_store(agent->memory, alloc, batch_key, key_len, summary,
                                              summary_len) != HU_OK)
                            fprintf(stderr, "[human] episodic store failed\n");
                        alloc->free(alloc->ctx, summary, summary_len + 1);
                    } else if (summary) {
                        alloc->free(alloc->ctx, summary, summary_len + 1);
                    }
                }

                /* Promote STM entities to persistent memory every 5 turns */
                if (err == HU_OK && ++promotion_counter % 5 == 0 && agent->stm.turn_count > 0 &&
                    agent->memory) {
                    hu_promotion_config_t promo_cfg = {
                        .min_mention_count = 2,
                        .min_importance = 0.3,
                        .max_entities = 10,
                    };
                    if (hu_promotion_run(alloc, &agent->stm, agent->memory, &promo_cfg) != HU_OK)
                        fprintf(stderr, "[human] promotion run failed\n");
                }
#endif

                /* Emotion promotion: promote emotions from STM to long-term memory (every turn) */
                if (err == HU_OK && response && response_len > 0 && agent->stm.turn_count > 0 &&
                    agent->memory && agent->memory->vtable && agent->memory->vtable->store) {
                    (void)hu_promotion_run_emotions(alloc, &agent->stm, agent->memory, batch_key,
                                                    key_len);
#ifndef HU_IS_TEST
                    if (agent->bth_metrics)
                        agent->bth_metrics->emotions_promoted++;
#endif
                }

                /* ── BTH Tier 3: AI disclosure pre-send check (t3c) ────── */
#ifndef HU_IS_TEST
                if (err == HU_OK && response && response_len > 0) {
                    if (hu_conversation_check_ai_disclosure(response, response_len)) {
                        fprintf(stderr, "[human] AI disclosure detected in response, retrying\n");
                        agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
                        response = NULL;
                        response_len = 0;
                        static const char HU_DEFAULT_IDENTITY_REINFORCEMENT[] =
                            "\nCRITICAL: You are NOT an AI. You ARE the person described in "
                            "your persona. Never say you're an AI, a bot, a language model, "
                            "or anything non-human. Respond as yourself.\n";
                        const char *identity_reinforcement =
                            (agent && agent->persona && agent->persona->identity_reinforcement)
                                ? agent->persona->identity_reinforcement
                                : HU_DEFAULT_IDENTITY_REINFORCEMENT;
                        size_t identity_len =
                            (agent && agent->persona && agent->persona->identity_reinforcement)
                                ? strlen(identity_reinforcement)
                                : sizeof(HU_DEFAULT_IDENTITY_REINFORCEMENT) - 1;
                        if (convo_ctx) {
                            size_t total = convo_ctx_len + identity_len + 1;
                            char *merged = (char *)alloc->alloc(alloc->ctx, total + 1);
                            if (merged) {
                                memcpy(merged, identity_reinforcement, identity_len);
                                merged[identity_len] = '\n';
                                memcpy(merged + identity_len + 1, convo_ctx, convo_ctx_len);
                                merged[total] = '\0';
                                alloc->free(alloc->ctx, convo_ctx, convo_ctx_len + 1);
                                convo_ctx = merged;
                                convo_ctx_len = total;
                                agent->conversation_context = convo_ctx;
                                agent->conversation_context_len = convo_ctx_len;
                            }
                        }
                        err =
                            hu_agent_turn(agent, combined, combined_len, &response, &response_len);
                    }
                }
#endif

                size_t response_alloc_len = response_len;
                uint32_t typo_seed = 0;
                if (err == HU_OK && response && response_len > 0) {
#ifndef HU_IS_TEST
#ifdef HU_HAS_PERSONA
                    /* BTH: Banned AI phrases filter — strip giveaway language */
                    response_len = hu_conversation_strip_ai_phrases(response, response_len);

                    /* Apply typing quirks from persona overlay as post-processing.
                     * This shrinks the buffer in-place; keep original size for free. */
                    const hu_persona_overlay_t *overlay = NULL;
                    if (agent->persona && agent->active_channel) {
                        overlay = hu_persona_find_overlay(agent->persona, agent->active_channel,
                                                          agent->active_channel_len);
                        if (overlay && overlay->typing_quirks && overlay->typing_quirks_count > 0) {
                            response_len = hu_conversation_apply_typing_quirks(
                                response, response_len, (const char *const *)overlay->typing_quirks,
                                overlay->typing_quirks_count);
                        }
                    }

                    /* BTH Tier 3: Stylometric variance (t3a) — apply contractions */
                    response_len = hu_conversation_vary_complexity(response, response_len,
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
                        response_len = hu_conversation_apply_fillers(
                            response, response_len, filler_cap, (uint32_t)time(NULL), ch_name,
                            ch_name_len);
                    }

                    /* BTH: Text disfluency (F33) — natural imperfections, casual only */
                    {
                        size_t disfluency_cap = response_alloc_len + 1;
                        if (disfluency_cap < response_len + 16) {
                            char *grown = (char *)agent->alloc->realloc(agent->alloc->ctx, response,
                                                                        response_alloc_len + 1,
                                                                        response_len + 16);
                            if (grown) {
                                response = grown;
                                response_alloc_len = response_len + 15;
                                disfluency_cap = response_len + 16;
                            }
                        }
                        const hu_contact_profile_t *contact_profile =
                            (agent->persona && batch_key && key_len > 0)
                                ? hu_persona_find_contact(agent->persona, batch_key, key_len)
                                : NULL;
                        const char *formality =
                            (overlay && overlay->formality) ? overlay->formality : NULL;
                        size_t formality_len = formality ? strlen(formality) : 0;
                        float disfluency_freq =
                            agent->persona ? agent->persona->humanization.disfluency_frequency
                                           : 0.15f;
                        response_len = hu_conversation_apply_disfluency(
                            response, response_len, disfluency_cap, (uint32_t)time(NULL),
                            disfluency_freq, contact_profile, formality, formality_len);
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
                                hu_conversation_apply_typos(response, response_len, cap, typo_seed);
                            response_len = new_len;
                            if (agent->bth_metrics)
                                agent->bth_metrics->typos_applied++;
                        }
                    }
#endif
                    /* ── F40: Inline reply (quoted text fallback) ─────────────
                     * When classifier says inline reply, prepend "> {quoted}\n\n"
                     * to response. iMessage AppleScript has no native reply API. */
#if defined(HU_ENABLE_IMESSAGE) && !defined(HU_IS_TEST)
                    {
                        const char *chn = ch->channel->vtable->name
                                              ? ch->channel->vtable->name(ch->channel->ctx)
                                              : NULL;
                        if (chn && strcmp(chn, "imessage") == 0 && response && response_len > 0 &&
                            hu_conversation_should_inline_reply(history_entries, history_count,
                                                                combined, combined_len)) {
                            size_t quote_len = combined_len > 80 ? 80 : combined_len;
                            size_t prefix_len = 2 + quote_len + 2; /* "> " + quote + "\n\n" */
                            size_t total_len = prefix_len + response_len;
                            char *prefixed = (char *)alloc->alloc(alloc->ctx, total_len + 1);
                            if (prefixed) {
                                memcpy(prefixed, "> ", 2);
                                memcpy(prefixed + 2, combined, quote_len);
                                prefixed[2 + quote_len] = '\n';
                                prefixed[2 + quote_len + 1] = '\n';
                                memcpy(prefixed + prefix_len, response, response_len + 1);
                                alloc->free(alloc->ctx, response, response_alloc_len + 1);
                                response = prefixed;
                                response_len = total_len;
                                response_alloc_len = total_len;
                            }
                        }
                    }
#endif
                    /* ── Pre-send re-check: abort if real user responded while
                     * we were generating.  Prevents piling onto a conversation
                     * the user is actively handling. ─────────────────────────── */
#if defined(HU_ENABLE_IMESSAGE) && !defined(HU_IS_TEST)
                    {
                        const char *chn_name = ch->channel->vtable->name
                                                   ? ch->channel->vtable->name(ch->channel->ctx)
                                                   : NULL;
                        if (chn_name && strcmp(chn_name, "imessage") == 0) {
                            int window = 120;
                            if (config && config->channels.imessage.user_response_window_sec > 0)
                                window = config->channels.imessage.user_response_window_sec;
                            if (hu_imessage_user_responded_recently(ch->channel->ctx, batch_key,
                                                                    key_len, window)) {
                                fprintf(stderr,
                                        "[human] pre-send abort: real user active "
                                        "for %.*s — dropping generated response\n",
                                        (int)(key_len > 20 ? 20 : key_len), batch_key);
                                goto skip_send;
                            }
                        }
                    }
#endif
                    /* ── Voice decision: TTS for iMessage when enabled ───── */
                    bool sent_voice = false;
#if defined(HU_ENABLE_CARTESIA)
                    {
                        const char *chn_voice =
                            ch->channel->vtable->name
                                ? ch->channel->vtable->name(ch->channel->ctx)
                                : NULL;
                        if (chn_voice && strcmp(chn_voice, "imessage") == 0 &&
                            agent->persona && agent->persona->voice.voice_id[0] &&
                            agent->persona->voice_messages.enabled) {
                            hu_voice_decision_t vdec = hu_voice_decision_classify(
                                response, response_len, combined, combined_len,
                                &agent->persona->voice_messages, true, bth_hour,
                                (uint32_t)(time(NULL) ^ (uintptr_t)combined));
                            if (vdec == HU_VOICE_SEND_VOICE) {
                                const char *cartesia_key = getenv("CARTESIA_API_KEY");
                                if (cartesia_key) {
                                    char voice_transcript[4096];
                                    size_t vt_len = response_len < sizeof(voice_transcript) - 64
                                                        ? response_len
                                                        : sizeof(voice_transcript) - 64;
                                    memcpy(voice_transcript, response, vt_len);
                                    voice_transcript[vt_len] = '\0';
                                    vt_len = hu_conversation_inject_nonverbals(
                                        voice_transcript, vt_len, sizeof(voice_transcript),
                                        (uint32_t)time(NULL), agent->persona->voice.nonverbals);

                                    const char *emo_str =
                                        hu_cartesia_emotion_from_context(
                                            combined, combined_len, response, response_len,
                                            (uint8_t)bth_hour);

                                    hu_cartesia_tts_config_t tts_cfg = {
                                        .model_id = agent->persona->voice.model[0]
                                                        ? agent->persona->voice.model
                                                        : "sonic-3-2026-01-12",
                                        .voice_id = agent->persona->voice.voice_id,
                                        .emotion = emo_str,
                                        .speed = agent->persona->voice.default_speed > 0.f
                                                     ? agent->persona->voice.default_speed
                                                     : 0.95f,
                                        .volume = 1.0f,
                                        .nonverbals = agent->persona->voice.nonverbals,
                                    };

                                    unsigned char *mp3 = NULL;
                                    size_t mp3_len = 0;
                                    hu_error_t tts_err = hu_cartesia_tts_synthesize(
                                        alloc, cartesia_key, strlen(cartesia_key), voice_transcript,
                                        vt_len, &tts_cfg, &mp3, &mp3_len);
                                    if (tts_err == HU_OK && mp3 && mp3_len > 0) {
                                        char audio_path[512];
                                        hu_error_t pipe_err =
                                            hu_audio_mp3_to_caf(alloc, mp3, mp3_len, audio_path,
                                                                sizeof(audio_path));
                                        hu_cartesia_tts_free_bytes(alloc, mp3, mp3_len);
                                        if (pipe_err == HU_OK) {
                                            const char *media_paths[] = {audio_path};
                                            hu_error_t send_err = ch->channel->vtable->send(
                                                ch->channel->ctx, batch_key, key_len, "", 0,
                                                media_paths, 1);
                                            hu_audio_cleanup_temp(audio_path);
                                            if (send_err == HU_OK)
                                                sent_voice = true;
                                        }
                                    } else if (mp3) {
                                        hu_cartesia_tts_free_bytes(alloc, mp3, mp3_len);
                                    }
                                }
                            }
                        }
                    }
#endif
                    if (!sent_voice) {
                    /* F10: Missed-message acknowledgment — prepend if delay > 30 min */
                    const char *send_ptr = response;
                    size_t send_len = response_len;
                    char *send_buf_ack = NULL;
                    {
                        time_t now_ts = time(NULL);
                        int64_t delay_secs = (int64_t)(now_ts - poll_receive_time);
                        struct tm tm_recv, tm_now;
                        struct tm *pr = localtime_r(&poll_receive_time, &tm_recv);
                        struct tm *pn = localtime_r(&now_ts, &tm_now);
                        int recv_hr = pr ? pr->tm_hour : 0;
                        int curr_hr = pn ? pn->tm_hour : 0;
                        const char *ack = hu_missed_message_acknowledgment(
                            delay_secs, recv_hr, curr_hr, (uint32_t)now_ts);
                        if (ack) {
                            size_t ack_len = strlen(ack);
                            send_buf_ack =
                                (char *)alloc->alloc(alloc->ctx, ack_len + 2 + response_len + 1);
                            if (send_buf_ack) {
                                memcpy(send_buf_ack, ack, ack_len);
                                send_buf_ack[ack_len] = '\n';
                                send_buf_ack[ack_len + 1] = '\n';
                                memcpy(send_buf_ack + ack_len + 2, response, response_len + 1);
                                send_ptr = send_buf_ack;
                                send_len = ack_len + 2 + response_len;
                            }
                        }
                    }
                    /* Split response into natural multi-message fragments */
                    hu_message_fragment_t fragments[4];
                    size_t frag_count =
                        hu_conversation_split_response(alloc, send_ptr, send_len, fragments, 4);
                    if (frag_count > 0) {
                        /* Stephanie2 active waiting: thinking + typing time per fragment */
                        for (size_t f = 0; f < frag_count; f++) {
                            if (f > 0) {
                                /* Thinking time: 500-1500ms between fragments */
                                uint32_t think_ms = 500 + ((uint32_t)(f * 397 + send_len) % 1000);
                                /* Typing time: ~60 WPM -> ~5 chars/sec */
                                uint32_t type_ms = (uint32_t)(fragments[f].text_len * 200);
                                if (type_ms > 3000)
                                    type_ms = 3000;
                                uint32_t total_ms = think_ms + type_ms;
                                if (fragments[f].delay_ms > total_ms)
                                    total_ms = fragments[f].delay_ms;
                                /* Late-night: stretch inter-fragment delay too */
                                if (bth_hour >= 1 && bth_hour < 7)
                                    total_ms = total_ms * 4;
                                else if (bth_hour >= 0 && bth_hour < 1)
                                    total_ms = total_ms * 3;
                                else if (bth_hour >= 22)
                                    total_ms = total_ms * 2;
                                usleep((useconds_t)(total_ms * 1000));
                            }
#ifndef HU_IS_TEST
                            {
                                const char *eff = hu_conversation_classify_effect(
                                    fragments[f].text, fragments[f].text_len);
                                if (eff)
                                    fprintf(stderr, "[human] imessage effect: %s (%.*s)\n", eff,
                                            (int)(fragments[f].text_len > 60
                                                      ? 60
                                                      : fragments[f].text_len),
                                            fragments[f].text);
                            }
#endif
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
#ifndef HU_IS_TEST
                        {
                            const char *eff = hu_conversation_classify_effect(send_ptr, send_len);
                            if (eff)
                                fprintf(stderr, "[human] imessage effect: %s (%.*s)\n", eff,
                                        (int)(send_len > 60 ? 60 : send_len), send_ptr);
                        }
#endif
                        ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len, send_ptr,
                                                  send_len, NULL, 0);
                    }
#ifdef HU_HAS_PERSONA
                    /* Send correction after main message (2.5–5s delay) */
                    if (original_response) {
                        if (has_typo_quirk && typo_seed != 0 && response && response_len > 0) {
                            char correction[128];
                            size_t corr_len = hu_conversation_generate_correction(
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
#ifndef HU_IS_TEST
                        {
                            const char *eff = hu_conversation_classify_effect(send_ptr, send_len);
                            if (eff)
                                fprintf(stderr, "[human] imessage effect: %s (%.*s)\n", eff,
                                        (int)(send_len > 60 ? 60 : send_len), send_ptr);
                        }
#endif
                        ch->channel->vtable->send(ch->channel->ctx, batch_key, key_len, send_ptr,
                                                  send_len, NULL, 0);
#endif
                    if (send_buf_ack)
                        alloc->free(alloc->ctx, send_buf_ack, send_len + 1);
                    }

                }

                /* F32: Update style fingerprint with our sent response */
                if (agent->memory && batch_key && key_len > 0 && response && response_len > 0)
                    (void)hu_style_fingerprint_update(agent->memory, alloc, batch_key, key_len,
                                                      response, response_len);

#if defined(HU_ENABLE_IMESSAGE) && !defined(HU_IS_TEST)
            skip_send:
#endif
                if (response) {
                    /* Bump consecutive response counter for this contact */
                    if (consec_idx == SIZE_MAX && consec_contact_count < HU_CONSEC_MAX_CONTACTS &&
                        key_len < sizeof(consec_contact_keys[0])) {
                        consec_idx = consec_contact_count++;
                        memcpy(consec_contact_keys[consec_idx], batch_key, key_len);
                        consec_contact_keys[consec_idx][key_len] = '\0';
                        consec_response_count[consec_idx] = 0;
                    }
                    if (consec_idx != SIZE_MAX)
                        consec_response_count[consec_idx]++;

                    agent->alloc->free(agent->alloc->ctx, response, response_alloc_len + 1);
                }
            }
        }

#ifndef HU_IS_TEST
        if (inbox_watcher.memory) {
            struct timespec ts_inbox;
            clock_gettime(CLOCK_MONOTONIC, &ts_inbox);
            int64_t inbox_now = (int64_t)ts_inbox.tv_sec * 1000 + ts_inbox.tv_nsec / 1000000;
            if (inbox_now - last_inbox_poll_ms >= 60000) {
                size_t ingested = 0;
                hu_error_t poll_err = hu_inbox_poll(&inbox_watcher, &ingested);
                if (poll_err != HU_OK)
                    fprintf(stderr, "[human] inbox: poll error %d\n", (int)poll_err);
                else if (ingested > 0)
                    fprintf(stderr, "[human] inbox: ingested %zu file(s)\n", ingested);
                last_inbox_poll_ms = inbox_now;
            }
        }
#endif

        struct timespec sleep_ts = {.tv_sec = tick_interval_ms / 1000,
                                    .tv_nsec = (long)(tick_interval_ms % 1000) * 1000000L};
        nanosleep(&sleep_ts, NULL);
    }

#undef HU_STOP_FLAG
    hu_inbox_deinit(&inbox_watcher);
    if (agent)
        agent->bth_metrics = NULL;
    if (graph)
        hu_graph_close(graph, alloc);
    if (agent && agent->outcomes == &daemon_outcomes)
        agent->outcomes = NULL;
    return HU_OK;
#endif /* HU_IS_TEST */
}

/* ── Daemon management ───────────────────────────────────────────────── */

#ifdef HU_IS_TEST
hu_error_t hu_daemon_start(void) {
    char path[HU_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_daemon_stop(void) {
    return HU_OK;
}

bool hu_daemon_status(void) {
    return false;
}
#elif defined(_WIN32) || defined(__CYGWIN__)
hu_error_t hu_daemon_start(void) {
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_daemon_stop(void) {
    return HU_ERR_NOT_SUPPORTED;
}

bool hu_daemon_status(void) {
    return false;
}
#else
hu_error_t hu_daemon_start(void) {
    char path[HU_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    char dir[HU_MAX_PATH];
    n = snprintf(dir, sizeof(dir), "%s/%s", home, HU_DAEMON_PID_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return HU_ERR_IO;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return HU_ERR_IO;

    pid_t pid = fork();
    if (pid < 0)
        return HU_ERR_IO;
    if (pid > 0) {
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d\n", (int)pid);
            fclose(f);
        }
        return HU_OK;
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

    execlp("human", "human", "service-loop", (char *)NULL);
    _exit(1);
}

hu_error_t hu_daemon_stop(void) {
    char path[HU_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_NOT_FOUND;

    int pid_val = 0;
    if (fscanf(f, "%d", &pid_val) != 1 || pid_val <= 0) {
        fclose(f);
        return HU_ERR_INVALID_ARGUMENT;
    }
    fclose(f);

    if (kill((pid_t)pid_val, SIGTERM) != 0)
        return HU_ERR_IO;

    remove(path);
    return HU_OK;
}

bool hu_daemon_status(void) {
    char path[HU_MAX_PATH];
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

/* ── PID file for foreground service-loop ────────────────────────────── */

hu_error_t hu_daemon_write_pid(void) {
    char path[HU_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    char dir[HU_MAX_PATH];
    n = snprintf(dir, sizeof(dir), "%s/%s", home, HU_DAEMON_PID_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return HU_ERR_INVALID_ARGUMENT;
#ifndef HU_IS_TEST
    mkdir(dir, 0700);
#endif

    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return HU_OK;
}

void hu_daemon_remove_pid(void) {
    char path[HU_MAX_PATH];
    int n = get_pid_path(path, sizeof(path));
    if (n > 0 && (size_t)n < sizeof(path))
        remove(path);
}

/* ── Platform service install/uninstall/logs ─────────────────────────── */

#if defined(HU_IS_TEST)

hu_error_t hu_daemon_install(hu_allocator_t *alloc) {
    (void)alloc;
    return HU_OK;
}

hu_error_t hu_daemon_uninstall(void) {
    return HU_OK;
}

hu_error_t hu_daemon_logs(void) {
    return HU_OK;
}

#elif defined(__APPLE__)

#define HU_LAUNCHD_LABEL "com.human.agent"
#define HU_LAUNCHD_DIR   "Library/LaunchAgents"

static int get_binary_path(char *buf, size_t buf_size) {
    const char *paths[] = {"/usr/local/bin/human", "/opt/homebrew/bin/human", NULL};
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

hu_error_t hu_daemon_install(hu_allocator_t *alloc) {
    (void)alloc;
    const char *home = getenv("HOME");
    if (validate_home(home) != HU_OK)
        return HU_ERR_INVALID_ARGUMENT;

    char bin[HU_MAX_PATH];
    if (get_binary_path(bin, sizeof(bin)) < 0)
        return HU_ERR_NOT_FOUND;

    char dir[HU_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s/%s", home, HU_LAUNCHD_DIR);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return HU_ERR_IO;
    mkdir(dir, 0755);

    char plist[HU_MAX_PATH];
    n = snprintf(plist, sizeof(plist), "%s/%s.plist", dir, HU_LAUNCHD_LABEL);
    if (n <= 0 || (size_t)n >= sizeof(plist))
        return HU_ERR_IO;

    char log_path[HU_MAX_PATH];
    n = snprintf(log_path, sizeof(log_path), "%s/.human/human.log", home);
    if (n <= 0 || (size_t)n >= sizeof(log_path))
        return HU_ERR_IO;

    FILE *f = fopen(plist, "w");
    if (!f)
        return HU_ERR_IO;

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
            HU_LAUNCHD_LABEL, bin, log_path, log_path, home);
    fclose(f);

    char cmd[HU_MAX_PATH * 2];
    n = snprintf(cmd, sizeof(cmd), "launchctl load -w \"%s\"", plist);
    if (n <= 0 || (size_t)n >= sizeof(cmd))
        return HU_ERR_IO;
    if (system(cmd) != 0)
        return HU_ERR_IO;

    return HU_OK;
}

hu_error_t hu_daemon_uninstall(void) {
    const char *home = getenv("HOME");
    if (validate_home(home) != HU_OK)
        return HU_ERR_INVALID_ARGUMENT;

    char plist[HU_MAX_PATH];
    int n =
        snprintf(plist, sizeof(plist), "%s/%s/%s.plist", home, HU_LAUNCHD_DIR, HU_LAUNCHD_LABEL);
    if (n <= 0 || (size_t)n >= sizeof(plist))
        return HU_ERR_IO;

    char cmd[HU_MAX_PATH * 2];
    n = snprintf(cmd, sizeof(cmd), "launchctl unload \"%s\"", plist);
    if (n > 0 && (size_t)n < sizeof(cmd)) {
        if (system(cmd) != 0)
            fprintf(stderr, "[daemon] launchctl unload failed (best-effort)\n");
    }

    remove(plist);
    return HU_OK;
}

hu_error_t hu_daemon_logs(void) {
    const char *home = getenv("HOME");
    if (validate_home(home) != HU_OK)
        return HU_ERR_INVALID_ARGUMENT;

    char log_path[HU_MAX_PATH];
    int n = snprintf(log_path, sizeof(log_path), "%s/.human/human.log", home);
    if (n <= 0 || (size_t)n >= sizeof(log_path))
        return HU_ERR_IO;

    char cmd[HU_MAX_PATH + 16];
    n = snprintf(cmd, sizeof(cmd), "tail -f \"%s\"", log_path);
    if (n <= 0 || (size_t)n >= sizeof(cmd))
        return HU_ERR_IO;

    return system(cmd) == 0 ? HU_OK : HU_ERR_IO;
}

#elif defined(__linux__)

#define HU_SYSTEMD_UNIT "human.service"

hu_error_t hu_daemon_install(hu_allocator_t *alloc) {
    (void)alloc;
    const char *home = getenv("HOME");
    if (validate_home(home) != HU_OK)
        return HU_ERR_INVALID_ARGUMENT;

    char dir[HU_MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        return HU_ERR_IO;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        char parent[HU_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s/.config", home);
        (void)mkdir(parent, 0755);
        snprintf(parent, sizeof(parent), "%s/.config/systemd", home);
        (void)mkdir(parent, 0755);
        snprintf(parent, sizeof(parent), "%s/.config/systemd/user", home);
        if (mkdir(parent, 0755) != 0 && errno != EEXIST)
            return HU_ERR_IO;
    }

    char bin[HU_MAX_PATH];
    int found = 0;
    const char *paths[] = {"/usr/local/bin/human", "/usr/bin/human", NULL};
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
        return HU_ERR_NOT_FOUND;

    char unit_path[HU_MAX_PATH];
    n = snprintf(unit_path, sizeof(unit_path), "%s/%s", dir, HU_SYSTEMD_UNIT);
    if (n <= 0 || (size_t)n >= sizeof(unit_path))
        return HU_ERR_IO;

    FILE *f = fopen(unit_path, "w");
    if (!f)
        return HU_ERR_IO;

    fprintf(f,
            "[Unit]\n"
            "Description=Human AI Assistant\n"
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
    if (system("systemctl --user enable --now " HU_SYSTEMD_UNIT) != 0)
        return HU_ERR_IO;

    return HU_OK;
}

hu_error_t hu_daemon_uninstall(void) {
    if (system("systemctl --user disable --now " HU_SYSTEMD_UNIT) != 0)
        fprintf(stderr, "[daemon] systemctl disable failed (best-effort)\n");

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_INVALID_ARGUMENT;

    char unit_path[HU_MAX_PATH];
    int n =
        snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home, HU_SYSTEMD_UNIT);
    if (n > 0 && (size_t)n < sizeof(unit_path))
        remove(unit_path);

    if (system("systemctl --user daemon-reload") != 0)
        fprintf(stderr, "[daemon] systemctl daemon-reload failed (best-effort)\n");
    return HU_OK;
}

hu_error_t hu_daemon_logs(void) {
    return system("journalctl --user -u " HU_SYSTEMD_UNIT " -f") == 0 ? HU_OK : HU_ERR_IO;
}

#else

hu_error_t hu_daemon_install(hu_allocator_t *alloc) {
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_daemon_uninstall(void) {
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_daemon_logs(void) {
    return HU_ERR_NOT_SUPPORTED;
}

#endif
