/**
 * daemon_cron.c — Cron field parsing, schedule matching, and tick execution.
 *
 * Extracted from daemon.c. Implements:
 *   - 5-field cron expression parsing (min hour dom month dow)
 *   - System crontab tick (load, match, execute)
 *   - Agent-type cron jobs (hu_service_run_agent_cron)
 */
#include "human/daemon_cron.h"
#include "human/core/log.h"
#include "human/daemon.h"
#include "human/observability/validator_telemetry.h"

#include "human/agent.h"
#include "human/agent/output_validator_chain.h"
#include "human/agent/validators/builtin.h"
#include "human/context/conversation.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/security/moderation.h"

#ifdef HU_HAS_CRON
#include "human/cron.h"
#include "human/crontab.h"
#endif

#ifdef HU_ENABLE_SQLITE
#include "human/feeds/findings.h"
#include "human/feeds/processor.h"
#include "human/feeds/research.h"
#include "human/memory.h"
#ifdef HU_HAS_SKILLS
#include "human/intelligence/cycle.h"
#endif
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Cron field parsing (only when HU_HAS_CRON) ──────────────────────── */
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

bool hu_cron_atom_matches(const char *atom, size_t len, int value) {
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

bool hu_cron_field_matches(const char *field, int value) {
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
        if (hu_cron_atom_matches(start, len, value))
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

    return hu_cron_field_matches(fields[0], tm->tm_min) &&
           hu_cron_field_matches(fields[1], tm->tm_hour) &&
           hu_cron_field_matches(fields[2], tm->tm_mday) &&
           hu_cron_field_matches(fields[3], tm->tm_mon + 1) &&
           hu_cron_field_matches(fields[4], tm->tm_wday);
}

/* ── Cron tick execution ─────────────────────────────────────────────── */

void hu_daemon_cron_tick(hu_allocator_t *alloc) {
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
            hu_log_error("human", NULL, "cron job failed: %s (err=%s)", entries[i].command,
                         hu_error_string(run_err));
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
        char *fresh_prompt = NULL;
        size_t fresh_prompt_len = 0;

        /* Rebuild research agent prompt with fresh digest */
#ifdef HU_ENABLE_SQLITE
        if (jobs[i].name && strcmp(jobs[i].name, "research-agent") == 0 && agent->memory) {
            sqlite3 *fresh_db = hu_sqlite_memory_get_db(agent->memory);
            if (fresh_db) {
                int64_t since = (int64_t)now - 86400;
                char *digest = NULL;
                size_t digest_len = 0;
                hu_feed_build_daily_digest(alloc, fresh_db, since, 4000, &digest, &digest_len);
                if (digest && digest_len > 0) {
                    if (hu_research_build_prompt(alloc, digest, digest_len, &fresh_prompt,
                                                 &fresh_prompt_len) == HU_OK &&
                        fresh_prompt) {
                        prompt = fresh_prompt;
                    }
                    alloc->free(alloc->ctx, digest, digest_len + 1);
                }
            }
        }
#endif
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
                            /* Run outbound validator chain in place. Pre-MED-3 this
                             * was ~30 lines of build/execute/branch/destroy boilerplate
                             * duplicated across 6 sites; pre-MED-2 the rewrite path also
                             * silently dropped oversized rewrites. The helper truncates
                             * to cap-1 instead. */
                            hu_validator_chain_apply_default_in_place(
                                alloc, NULL, NULL, 0, "cron send", response, &response_len,
                                response_len + 1);
                            if (response_len == 0)
                                break;
                            response_len = hu_conversation_vary_complexity(response, response_len,
                                                                           (uint32_t)time(NULL));
                            if (response_len > 1 && response[0] >= 'A' && response[0] <= 'Z' &&
                                response[1] >= 'a' && response[1] <= 'z' && response[0] != 'I') {
                                response[0] = (char)(response[0] + 32);
                            }
                            if (response_len > 1 && response[response_len - 1] == '.') {
                                response[response_len - 1] = '\0';
                                response_len--;
                            }
                            /* SHIELD-004: Moderation check before cron send */
                            {
                                hu_moderation_result_t mod_r;
                                memset(&mod_r, 0, sizeof(mod_r));
                                hu_error_t mod_err =
                                    hu_moderation_check(alloc, response, response_len, &mod_r);
                                if (mod_err != HU_OK) {
                                    hu_log_error("human", NULL, "cron moderation check failed: %s",
                                                 hu_error_string(mod_err));
                                } else if (mod_r.flagged) {
                                    hu_log_info("human", NULL,
                                                "cron moderation flagged: v=%.2f sh=%.2f",
                                                mod_r.violence_score, mod_r.self_harm_score);
                                }
                            }
                            hu_error_t send_err = channels[c].channel->vtable->send(
                                channels[c].channel->ctx, target_part, target_part_len, response,
                                response_len, NULL, 0);
                            if (send_err != HU_OK) {
                                hu_log_error("human", NULL, "cron send failed: %s",
                                             hu_error_string(send_err));
                            }
                        }
                        break;
                    }
                }
            }
#ifdef HU_ENABLE_SQLITE
            /* Parse research agent output for findings, then run intelligence cycle */
            if (strstr(prompt, "research") && agent->memory) {
                sqlite3 *fdb = hu_sqlite_memory_get_db(agent->memory);
                if (fdb) {
                    hu_error_t parse_err =
                        hu_findings_parse_and_store(alloc, fdb, response, response_len);
                    if (parse_err != HU_OK && parse_err != HU_ERR_NOT_FOUND) {
                        hu_log_error("human", NULL, "cron findings parse failed: %s",
                                     hu_error_string(parse_err));
                    }
#ifdef HU_HAS_SKILLS
                    hu_intelligence_cycle_result_t cron_cycle = {0};
                    hu_error_t cycle_err = hu_intelligence_run_cycle(alloc, fdb, &cron_cycle);
                    if (cycle_err == HU_OK &&
                        (cron_cycle.findings_actioned > 0 || cron_cycle.lessons_extracted > 0)) {
                        hu_log_info("human", NULL, "post-research cycle: %zu findings, %zu lessons",
                                    cron_cycle.findings_actioned, cron_cycle.lessons_extracted);
                    } else if (cycle_err != HU_OK) {
                        hu_log_error("human", NULL, "post-research cycle failed: %s",
                                     hu_error_string(cycle_err));
                    }
#endif
                }
            }
#endif
        }

        hu_error_t log_err = hu_cron_add_run(agent->scheduler, alloc, jobs[i].id, (int64_t)now,
                                             err == HU_OK ? "success" : "failed", response);
        if (log_err != HU_OK) {
            hu_log_error("human", NULL, "cron add_run failed: %s", hu_error_string(log_err));
        }

        if (response)
            agent->alloc->free(agent->alloc->ctx, response, response_len + 1);
        hu_agent_clear_history(agent);
        if (fresh_prompt)
            alloc->free(alloc->ctx, fresh_prompt, fresh_prompt_len + 1);
    }
    return HU_OK;
}

#else /* !HU_HAS_CRON — stubs */

bool hu_cron_atom_matches(const char *atom, size_t len, int value) {
    (void)atom;
    (void)len;
    (void)value;
    return false;
}

bool hu_cron_field_matches(const char *field, int value) {
    (void)field;
    (void)value;
    return false;
}

bool hu_cron_schedule_matches(const char *schedule, const struct tm *tm) {
    (void)schedule;
    (void)tm;
    return false;
}

void hu_daemon_cron_tick(hu_allocator_t *alloc) {
    (void)alloc;
}

hu_error_t hu_service_run_agent_cron(hu_allocator_t *alloc, hu_agent_t *agent,
                                     hu_service_channel_t *channels, size_t channel_count) {
    (void)alloc;
    (void)agent;
    (void)channels;
    (void)channel_count;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_HAS_CRON */
