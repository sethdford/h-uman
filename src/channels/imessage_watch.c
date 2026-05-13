/*
 * imessage_watch.c — imsg subprocess lifecycle + CLI helpers.
 *
 * Step 3 of the iMessage shape refactor — see
 * docs/plans/2026-05-12-imessage-shape-refactor.md.
 *
 * The `imsg` CLI (steipete/imsg) is an optional third-party tool that gives
 * us:
 *
 *   1. **Event-driven polling** — `imsg watch --json` streams new-message
 *      events to stdout, so the daemon doesn't have to wake up on a fixed
 *      interval to query chat.db. Big win for battery / latency.
 *   2. **Tapback / react send** — `imsg react` sends a tapback without
 *      needing the AX context-menu walk in imessage_ax.c. More robust
 *      across macOS versions when available.
 *   3. **Target validation** — `imsg chats --json` confirms the configured
 *      target handle actually has an active chat, so first-message
 *      surprises don't look like silent failures.
 *
 * All entry points take an `hu_imessage_ctx_t *` and read/write the
 * `imsg_*` fields on it (running flag, pid, fd, validation flag).
 *
 * Visibility
 * ----------
 * `imsg_cli_available` was already extern (Step 1.5). The other entry
 * points (`imsg_watch_start`, `imsg_watch_stop`, `imsg_watch_has_data`,
 * `imsg_validate_target`, `imsg_try_react`) are declared in
 * src/channels/imessage_internal.h and called from imessage.c
 * (start / stop / poll paths) and from src/channels/imessage_react.c
 * (after Step 4) for the imsg-CLI tapback fallback.
 */

#include "imessage_internal.h"

#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/process_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

/* Check if the imsg CLI (steipete/imsg) is available on $PATH.
 * Caches the result after first check. Only compiled in non-test macOS builds
 * since all callers are behind !HU_IS_TEST guards. */
bool imsg_cli_available(hu_imessage_ctx_t *c) {
    if (!c || !c->alloc)
        return false;
    if (c->imsg_cli_checked)
        return c->has_imsg_cli;
    c->imsg_cli_checked = true;
    c->has_imsg_cli = false;
    const char *argv[] = {"which", "imsg", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(c->alloc, argv, NULL, 65536, &result);
    if (err == HU_OK && result.success && result.exit_code == 0)
        c->has_imsg_cli = true;
    hu_run_result_free(c->alloc, &result);
    if (c->has_imsg_cli && getenv("HU_DEBUG"))
        hu_log_info("imessage", NULL, "imsg CLI detected on $PATH");
    return c->has_imsg_cli;
}

/* ── imsg watch subprocess (event-driven poll trigger) ─────────────── */

/* SIGCHLD reaper: tell the kernel to auto-reap any of OUR children that
 * exit, so a long-running `imsg watch` process that dies between our
 * waitpid() calls in has_data/stop doesn't accumulate as a zombie.
 *
 * Installed lazily on first imsg_watch_start (one-shot guard) rather than
 * unconditionally at daemon init, because the daemon also forks via
 * hu_process_run with explicit waitpid; this would cause those waitpids
 * to return -1/ECHILD. Those callers tolerate that gracefully (they
 * ignore the return value past the "did it exit" check), but the change
 * is still less invasive when applied just-in-time here.
 *
 * SIGCHLD=SIG_IGN is the canonical POSIX pattern for "fire-and-forget
 * children with no reap-loop". After it's installed, our explicit
 * waitpid(WNOHANG) in imsg_watch_has_data / imsg_watch_stop returns
 * -1/ECHILD (child auto-reaped); both call sites treat non-zero as
 * "child gone" → goto reaped, which is the correct behavior. */
static void imsg_watch_install_sigchld_reaper(void) {
    static bool installed = false;
    if (installed)
        return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP; /* don't deliver SIGCHLD for stopped children */
    (void)sigaction(SIGCHLD, &sa, NULL);
    installed = true;
}

void imsg_watch_start(hu_imessage_ctx_t *c) {
    if (!c || c->imsg_watch_running || !c->use_imsg_cli || !imsg_cli_available(c))
        return;
    if (c->circuit_breaker_tripped) {
        /* Refuse to respawn while breaker is tripped; the watch process would
         * just exit immediately on its own AUTH read of chat.db. */
        return;
    }

    imsg_watch_install_sigchld_reaper();

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return;

    char rowid_str[32];
    snprintf(rowid_str, sizeof(rowid_str), "%lld", (long long)c->last_rowid);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("imsg", "imsg", "watch", "--json", "--since-rowid", rowid_str, NULL);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    c->imsg_watch_pid = pid;
    c->imsg_watch_fd = pipefd[0];
    c->imsg_watch_running = true;
    hu_log_info("imessage", NULL, "started imsg watch (pid=%d, since-rowid=%s)", (int)pid,
                rowid_str);

    /* Watch path is the steady-state happy path; once it spawns, the SQL poll
     * function returns OK early without saving status. Without this update,
     * the persisted status file stays in whatever state the LAST SQL poll
     * left it (e.g. TRIPPED from a previous run before FDA was granted),
     * and `human doctor imessage` lies about daemon health. Record this
     * spawn as a successful poll so the status file reflects "watch active,
     * healthy" — this also auto-resets the breaker on recovery from a
     * previously-tripped state. */
    imessage_record_poll_success(c, (int64_t)time(NULL));
    imessage_save_poll_status(c);
}

/* Only called from the SQLite/Apple poll path; without those flags the
 * function is dead code and -Werror=unused-function fires on minimal Linux
 * builds (HU_ENABLE_SQLITE=OFF + non-Apple host). */
__attribute__((unused)) bool imsg_watch_has_data(hu_imessage_ctx_t *c) {
    if (!c->imsg_watch_running || c->imsg_watch_fd < 0)
        return false;

    char drain[4096];
    bool got_data = false;
    for (;;) {
        ssize_t n = read(c->imsg_watch_fd, drain, sizeof(drain));
        if (n > 0) {
            got_data = true;
            continue;
        }
        if (n == 0) {
            c->imsg_watch_running = false;
            close(c->imsg_watch_fd);
            c->imsg_watch_fd = -1;
            waitpid(c->imsg_watch_pid, NULL, WNOHANG);
            hu_log_info("imessage", NULL, "imsg watch exited, falling back to timer-based polling");
            return got_data;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return got_data;
        /* Unexpected read error — tear down watch to avoid zombie state */
        hu_log_error("imessage", NULL, "imsg watch pipe read error (errno=%d), tearing down",
                     errno);
        c->imsg_watch_running = false;
        close(c->imsg_watch_fd);
        c->imsg_watch_fd = -1;
        waitpid(c->imsg_watch_pid, NULL, WNOHANG);
        return got_data;
    }
}

void imsg_watch_stop(hu_imessage_ctx_t *c) {
    if (!c || !c->imsg_watch_running)
        return;
    kill(c->imsg_watch_pid, SIGTERM);
    /* Non-blocking wait with retries to avoid hanging if child ignores SIGTERM */
    for (int i = 0; i < 10; i++) {
        if (waitpid(c->imsg_watch_pid, NULL, WNOHANG) != 0)
            goto reaped;
        usleep(100000);
    }
    kill(c->imsg_watch_pid, SIGKILL);
    waitpid(c->imsg_watch_pid, NULL, 0);
reaped:
    if (c->imsg_watch_fd >= 0) {
        close(c->imsg_watch_fd);
        c->imsg_watch_fd = -1;
    }
    c->imsg_watch_running = false;
    hu_log_info("imessage", NULL, "stopped imsg watch");
}

/* ── imsg chats target validation ─────────────────────────────────── */

bool imsg_validate_target(hu_imessage_ctx_t *c) {
    if (!c || !c->default_target || c->default_target_len == 0)
        return false;
    if (c->imsg_target_validated)
        return true;

    const char *argv[] = {"imsg", "chats", "--json", "--limit", "100", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run_with_timeout(c->alloc, argv, NULL, 262144, 10, &result);
    if (err != HU_OK || !result.success || result.exit_code != 0) {
        hu_run_result_free(c->alloc, &result);
        hu_log_info("imessage", NULL,
                    "imsg chats failed — cannot validate target (will retry next poll)");
        return false;
    }
    c->imsg_target_validated = true;

    bool found = false;
    if (result.stdout_buf && result.stdout_len >= c->default_target_len) {
        for (size_t i = 0; i <= result.stdout_len - c->default_target_len; i++) {
            if (memcmp(result.stdout_buf + i, c->default_target, c->default_target_len) == 0) {
                found = true;
                break;
            }
        }
    }
    hu_run_result_free(c->alloc, &result);

    if (!found)
        hu_log_info("imessage", NULL,
                    "target '%.*s' not found in active chats (imsg chats); "
                    "first message may create a new conversation",
                    (int)c->default_target_len, c->default_target);
    else if (getenv("HU_DEBUG"))
        hu_log_info("imessage", NULL, "target '%.*s' validated via imsg chats",
                    (int)c->default_target_len, c->default_target);
    return found;
}

/* ── imsg react helper (Tier-3 tapback via CLI, used by imessage_react.c) ─ */

bool imsg_try_react(hu_imessage_ctx_t *c, int64_t message_id, hu_reaction_type_t reaction) {
    if (!c || !c->alloc || message_id <= 0)
        return false;
    const char *tapback_name = hu_imessage_reaction_to_tapback_name(reaction);
    if (!tapback_name)
        return false;

    char chat_rowid_str[32] = {0};
#if defined(HU_ENABLE_SQLITE)
    const char *home_env = getenv("HOME");
    if (home_env) {
        char db_p[512];
        int dp = snprintf(db_p, sizeof(db_p), "%s/Library/Messages/chat.db", home_env);
        if (dp > 0 && (size_t)dp < sizeof(db_p)) {
            sqlite3 *db = NULL;
            if (imessage_open_chatdb(db_p, &db) == SQLITE_OK) {
                sqlite3_stmt *cs = NULL;
                if (sqlite3_prepare_v2(db,
                                       "SELECT cmj.chat_id FROM chat_message_join cmj "
                                       "WHERE cmj.message_id = ? LIMIT 1",
                                       -1, &cs, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(cs, 1, message_id);
                    if (sqlite3_step(cs) == SQLITE_ROW) {
                        int64_t rowid = sqlite3_column_int64(cs, 0);
                        snprintf(chat_rowid_str, sizeof(chat_rowid_str), "%lld", (long long)rowid);
                    }
                    sqlite3_finalize(cs);
                }
                sqlite3_close(db);
            }
        }
    }
#endif
    if (!chat_rowid_str[0])
        return false;

    const char *react_argv[] = {"imsg",       "react",      "--chat-id", chat_rowid_str,
                                "--reaction", tapback_name, NULL};
    hu_run_result_t rr = {0};
    hu_error_t re = hu_process_run_with_timeout(c->alloc, react_argv, NULL, 65536, 15, &rr);
    bool rok = (re == HU_OK && rr.success && rr.exit_code == 0);
    if (!rok)
        hu_log_info(
            "imessage", NULL, "imsg react failed (exit=%d stdout=%.*s stderr=%.*s)", rr.exit_code,
            (int)(rr.stdout_len < 200 ? rr.stdout_len : 200), rr.stdout_buf ? rr.stdout_buf : "",
            (int)(rr.stderr_len < 200 ? rr.stderr_len : 200), rr.stderr_buf ? rr.stderr_buf : "");
    hu_run_result_free(c->alloc, &rr);
    return rok;
}

#endif /* !HU_IS_TEST && __APPLE__ && __MACH__ */
