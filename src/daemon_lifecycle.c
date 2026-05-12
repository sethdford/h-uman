/**
 * daemon_lifecycle.c — Daemon start/stop/status, PID management, platform install.
 *
 * Extracted from daemon.c. Implements:
 *   - hu_daemon_start/stop/status (fork+setsid on Unix, stubs on Windows/test)
 *   - hu_daemon_write_pid/remove_pid (foreground service-loop PID tracking)
 *   - hu_daemon_install/uninstall/logs (launchd on macOS, systemd on Linux)
 *   - Internal helpers: validate_home, get_pid_path
 */
#include "human/core/log.h"
#include "human/daemon_lifecycle.h"
#include "human/daemon.h"
#include "human/core/error.h"
#include "human/core/io_secure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__CYGWIN__)
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define HU_DAEMON_PID_DIR  ".human"
#define HU_DAEMON_PID_FILE "human.pid"
#define HU_MAX_PATH        1024

/* ── Path validation ─────────────────────────────────────────────────── */

hu_error_t hu_daemon_validate_home(const char *home) {
    if (!home || !home[0]) {
        hu_log_info("daemon", NULL, "HOME not set");
        return HU_ERR_INVALID_ARGUMENT;
    }
    for (const char *p = home; *p; p++) {
        char c = *p;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '/' && c != '.' && c != '_' && c != '-' && c != ' ') {
            hu_log_info("daemon", NULL, "HOME contains unsafe characters");
            return HU_ERR_INVALID_ARGUMENT;
        }
    }
    return HU_OK;
}

int hu_daemon_get_pid_path(char *buf, size_t buf_size) {
    const char *home = getenv("HOME");
    if (!home)
        home = ".";
    if (hu_daemon_validate_home(home) != HU_OK)
        return -1;
    return snprintf(buf, buf_size, "%s/%s/%s", home, HU_DAEMON_PID_DIR, HU_DAEMON_PID_FILE);
}

/* ── Daemon management ───────────────────────────────────────────────── */

#ifdef HU_IS_TEST
hu_error_t hu_daemon_start(void) {
    char path[HU_MAX_PATH];
    int n = hu_daemon_get_pid_path(path, sizeof(path));
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
    int n = hu_daemon_get_pid_path(path, sizeof(path));
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
        /* PID file lives at ~/.human/human.pid. The path is built from
         * $HOME (validated above by validate_home) so CodeQL flags it
         * as cpp/path-injection + cpp/world-writable-file-creation.
         * hu_io_secure_open replaces fopen("w") with explicit 0644 and
         * a traversal-rejecting open(). */
        FILE *f = NULL;
        if (hu_io_secure_open(path, HU_IO_PERM_USER, "w", &f) == HU_OK && f) {
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
    int n = hu_daemon_get_pid_path(path, sizeof(path));
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
    int n = hu_daemon_get_pid_path(path, sizeof(path));
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
    int n = hu_daemon_get_pid_path(path, sizeof(path));
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

    /* Service-loop PID at ~/.human/human.pid — same env-derived path
     * as hu_daemon_start above; route through hu_io_secure_open for
     * the explicit 0644 mode and the traversal guard. */
    FILE *f = NULL;
    hu_error_t open_err = hu_io_secure_open(path, HU_IO_PERM_USER, "w", &f);
    if (open_err != HU_OK || !f)
        return HU_ERR_IO;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
    return HU_OK;
}

void hu_daemon_remove_pid(void) {
    char path[HU_MAX_PATH];
    int n = hu_daemon_get_pid_path(path, sizeof(path));
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
    /* Check $HOME/bin/human first (common user-local install path) */
    const char *home = getenv("HOME");
    if (home && home[0] == '/') {
        char user_bin[HU_MAX_PATH];
        int n = snprintf(user_bin, sizeof(user_bin), "%s/bin/human", home);
        if (n > 0 && (size_t)n < sizeof(user_bin) && access(user_bin, X_OK) == 0) {
            if ((size_t)n < buf_size) {
                memcpy(buf, user_bin, (size_t)n + 1);
                return n;
            }
        }
    }
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
    if (hu_daemon_validate_home(home) != HU_OK)
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

    /* launchd plist at ~/Library/LaunchAgents/com.human.app.plist —
     * path derived from $HOME; route through hu_io_secure_open.
     * launchd only loads plists whose mode allows owner-read, 0644
     * keeps the existing semantics and pacifies CodeQL. */
    FILE *f = NULL;
    if (hu_io_secure_open(plist, HU_IO_PERM_USER, "w", &f) != HU_OK || !f)
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

#ifndef HU_IS_TEST
    /* Ad-hoc sign the binary so macOS code signing enforcement doesn't
     * SIGKILL it on launch.  This is a no-op if already validly signed. */
    char sign_cmd[HU_MAX_PATH + 32];
    n = snprintf(sign_cmd, sizeof(sign_cmd), "codesign -s - -f \"%s\" 2>/dev/null", bin);
    if (n > 0 && (size_t)n < sizeof(sign_cmd))
        (void)system(sign_cmd);
#endif

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
    if (hu_daemon_validate_home(home) != HU_OK)
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
            hu_log_error("daemon", NULL, "launchctl unload failed (best-effort)");
    }

    remove(plist);
    return HU_OK;
}

hu_error_t hu_daemon_logs(void) {
    const char *home = getenv("HOME");
    if (hu_daemon_validate_home(home) != HU_OK)
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
    if (hu_daemon_validate_home(home) != HU_OK)
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

    /* systemd unit at ~/.config/systemd/user/human.service — path
     * derived from $XDG_CONFIG_HOME / $HOME; route through
     * hu_io_secure_open. systemd requires the unit file be readable;
     * 0644 is the systemd convention for user-level units. */
    FILE *f = NULL;
    if (hu_io_secure_open(unit_path, HU_IO_PERM_USER, "w", &f) != HU_OK || !f)
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
        hu_log_error("daemon", NULL, "systemctl daemon-reload failed");
    if (system("systemctl --user enable --now " HU_SYSTEMD_UNIT) != 0)
        return HU_ERR_IO;

    return HU_OK;
}

hu_error_t hu_daemon_uninstall(void) {
    if (system("systemctl --user disable --now " HU_SYSTEMD_UNIT) != 0)
        hu_log_error("daemon", NULL, "systemctl disable failed (best-effort)");

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_INVALID_ARGUMENT;

    char unit_path[HU_MAX_PATH];
    int n =
        snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/%s", home, HU_SYSTEMD_UNIT);
    if (n > 0 && (size_t)n < sizeof(unit_path))
        remove(unit_path);

    if (system("systemctl --user daemon-reload") != 0)
        hu_log_error("daemon", NULL, "systemctl daemon-reload failed (best-effort)");
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
