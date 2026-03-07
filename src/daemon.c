#include "seaclaw/daemon.h"
#include "seaclaw/agent.h"
#include "seaclaw/config.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#ifdef SC_HAS_CRON
#include "seaclaw/crontab.h"
#endif
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define SC_DAEMON_PID_DIR  ".seaclaw"
#define SC_DAEMON_PID_FILE "seaclaw.pid"
#define SC_MAX_PATH        1024

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

#endif /* SC_HAS_CRON */

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
#endif
    return SC_OK;
#else

#if !defined(_WIN32) && !defined(__CYGWIN__)
    g_stop_flag = 0;
    signal(SIGTERM, service_signal_handler);
    signal(SIGINT, service_signal_handler);
#endif

#ifdef SC_HAS_CRON
    time_t last_cron_minute = 0;
#endif

    {
        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t start = (int64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        for (size_t i = 0; i < channel_count; i++)
            channels[i].last_poll_ms = start;
    }

#if !defined(_WIN32) && !defined(__CYGWIN__)
#define SC_STOP_FLAG g_stop_flag
#else
    volatile int local_stop = 0;
#define SC_STOP_FLAG local_stop
#endif

    while (!SC_STOP_FLAG) {
#ifdef SC_HAS_CRON
        {
            time_t t = time(NULL);
            time_t current_minute = t / 60;
            if (current_minute > last_cron_minute) {
                run_cron_tick(alloc);
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
            size_t count = 0;
            ch->poll_fn(ch->channel_ctx, alloc, msgs, 16, &count);
            ch->last_poll_ms = tick_now;

            if (!agent || !ch->channel || !ch->channel->vtable || !ch->channel->vtable->send ||
                count == 0)
                continue;

            for (size_t m = 0; m < count; m++) {
                char *response = NULL;
                size_t response_len = 0;
                size_t content_len = strlen(msgs[m].content);
                if (content_len == 0)
                    continue;

                size_t key_len = strlen(msgs[m].session_key);

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
                /* Apply per-channel persona override if configured */
                if (config && agent->active_channel) {
                    const char *channel_persona =
                        sc_config_persona_for_channel(config, agent->active_channel);
                    const char *current = agent->persona_name ? agent->persona_name : "";
                    if (channel_persona && channel_persona[0] &&
                        strcmp(channel_persona, current) != 0) {
                        sc_error_t perr =
                            sc_agent_set_persona(agent, channel_persona, strlen(channel_persona));
                        if (perr != SC_OK) {
#ifndef SC_IS_TEST
                            fprintf(stderr,
                                    "[seaclaw] warning: failed to switch persona to '%s' for "
                                    "channel '%s'\n",
                                    channel_persona,
                                    agent->active_channel ? agent->active_channel : "?");
#endif
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
                            agent->session_store->ctx, alloc, msgs[m].session_key, key_len,
                            &entries, &entry_count) == SC_OK &&
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

                sc_error_t err =
                    sc_agent_turn(agent, msgs[m].content, content_len, &response, &response_len);

                /* Persist this exchange */
                if (agent->session_store && agent->session_store->vtable &&
                    agent->session_store->vtable->save_message) {
                    agent->session_store->vtable->save_message(agent->session_store->ctx,
                                                               msgs[m].session_key, key_len, "user",
                                                               4, msgs[m].content, content_len);
                    if (err == SC_OK && response && response_len > 0) {
                        agent->session_store->vtable->save_message(
                            agent->session_store->ctx, msgs[m].session_key, key_len, "assistant", 9,
                            response, response_len);
                    }
                }

                if (err == SC_OK && response && response_len > 0) {
                    ch->channel->vtable->send(ch->channel->ctx, msgs[m].session_key, key_len,
                                              response, response_len, NULL, 0);
                }
                if (response) {
                    alloc->free(alloc->ctx, response, response_len + 1);
                }
            }
        }

        struct timespec sleep_ts = {.tv_sec = tick_interval_ms / 1000,
                                    .tv_nsec = (long)(tick_interval_ms % 1000) * 1000000L};
        nanosleep(&sleep_ts, NULL);
    }

#undef SC_STOP_FLAG
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
    if (n > 0 && (size_t)n < sizeof(cmd))
        (void)system(cmd);

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

    char mkdir_cmd[SC_MAX_PATH + 16];
    n = snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", dir);
    if (n <= 0 || (size_t)n >= sizeof(mkdir_cmd))
        return SC_ERR_IO;
    (void)system(mkdir_cmd);

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

    (void)system("systemctl --user daemon-reload");
    if (system("systemctl --user enable --now " SC_SYSTEMD_UNIT) != 0)
        return SC_ERR_IO;

    return SC_OK;
}

sc_error_t sc_daemon_uninstall(void) {
    (void)system("systemctl --user disable --now " SC_SYSTEMD_UNIT);

    const char *home = getenv("HOME");
    if (!home)
        return SC_ERR_INVALID_ARGUMENT;

    char unit_path[SC_MAX_PATH];
    int n =
        snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home, SC_SYSTEMD_UNIT);
    if (n > 0 && (size_t)n < sizeof(unit_path))
        remove(unit_path);

    (void)system("systemctl --user daemon-reload");
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
