/*
 * Self-update: check GitHub releases, download and replace binary.
 * In HU_IS_TEST mode, returns mock data without network calls.
 */
#include "human/core/log.h"
#include "human/update.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include "human/crypto.h"
#include "human/platform.h"
#include "human/version.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <limits.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <stdlib.h>
#endif

#define GITHUB_API_URL "https://api.github.com/repos/sethdford/h-uman/releases/latest"
#define RELEASE_BASE   "https://github.com/sethdford/h-uman/releases/latest/download/"
#define LAST_CHECK_FILE ".last_update_check"

typedef enum {
    INSTALL_NIX,
    INSTALL_HOMEBREW,
    INSTALL_DOCKER,
    INSTALL_BINARY,
    INSTALL_DEV
} install_method_t;

/* ── semver comparison ──────────────────────────────────────────────── */

static int parse_version_part(const char **p) {
    int val = 0;
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val;
}

int hu_version_compare(const char *a, const char *b) {
    if (!a && !b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;

    if (*a == 'v')
        a++;
    if (*b == 'v')
        b++;

    int a_major = parse_version_part(&a);
    if (*a == '.')
        a++;
    int a_minor = parse_version_part(&a);
    if (*a == '.')
        a++;
    int a_patch = parse_version_part(&a);

    int b_major = parse_version_part(&b);
    if (*b == '.')
        b++;
    int b_minor = parse_version_part(&b);
    if (*b == '.')
        b++;
    int b_patch = parse_version_part(&b);

    if (a_major != b_major)
        return a_major - b_major;
    if (a_minor != b_minor)
        return a_minor - b_minor;
    return a_patch - b_patch;
}

/* ── platform helpers (non-test only) ───────────────────────────────── */

#if !defined(HU_IS_TEST)
static install_method_t detect_install_method(const char *exe_path) {
    if (!exe_path)
        return INSTALL_BINARY;
    if (strstr(exe_path, "/nix/store/"))
        return INSTALL_NIX;
    if (strstr(exe_path, "/homebrew/") || strstr(exe_path, "/Cellar/"))
        return INSTALL_HOMEBREW;
    if (strcmp(exe_path, "/human") == 0)
        return INSTALL_DOCKER;
    if (strstr(exe_path, "/build/"))
        return INSTALL_DEV;
    return INSTALL_BINARY;
}

static char *get_exe_path(hu_allocator_t *alloc) {
    (void)alloc;
#if defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return NULL;
    buf[n] = '\0';
    return hu_strdup(alloc, buf);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return NULL;
    char *resolved = realpath(buf, NULL);
    if (resolved) {
        char *out = hu_strdup(alloc, resolved);
        free(resolved);
        return out;
    }
    return hu_strdup(alloc, buf);
#else
    return NULL;
#endif
}

static const char *get_platform_asset(void) {
#if defined(__linux__) && defined(__x86_64__)
    return "human-linux-x86_64.bin";
#elif defined(__linux__) && defined(__aarch64__)
    return "human-linux-aarch64.bin";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "human-macos-aarch64.bin";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "human-macos-x86_64.bin";
#elif defined(_WIN32) || defined(_WIN64)
    return "human-windows-x86_64.exe";
#else
    return NULL;
#endif
}

static void print_package_instructions(install_method_t method) {
    switch (method) {
    case INSTALL_NIX:
        printf("Detected installation via: Nix\nTo update, run:\n  nix-channel --update && nix-env "
               "-iA nixpkgs.human\n");
        break;
    case INSTALL_HOMEBREW:
        printf("Detected installation via: Homebrew\nTo update, run:\n  brew upgrade human\n");
        break;
    case INSTALL_DOCKER:
        printf("Detected installation via: Docker\nTo update, run:\n  docker pull "
               "ghcr.io/sethdford/h-uman:latest\n");
        break;
    case INSTALL_DEV:
        printf("Development installation detected.\nTo update, run:\n  git pull && cmake --build "
               "build && make -C build\n");
        break;
    default:
        break;
    }
}

/* ── SHA256 verification ────────────────────────────────────────────── */

static void hex_encode(const uint8_t *data, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static hu_error_t verify_sha256(hu_allocator_t *alloc, const char *file_path, const char *asset,
                                const char *tmp_dir) {
    char sums_path[512];
    int n = snprintf(sums_path, sizeof(sums_path), "%s/sha256sums.txt", tmp_dir);
    if (n < 0 || (size_t)n >= sizeof(sums_path))
        return HU_ERR_IO;

    char sums_url[256];
    n = snprintf(sums_url, sizeof(sums_url), "%ssha256sums.txt", RELEASE_BASE);
    if (n < 0 || (size_t)n >= sizeof(sums_url))
        return HU_ERR_IO;

    const char *argv[] = {"curl", "-sfL", "--max-time", "30", "-o", sums_path, sums_url, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 64, &result);
    hu_run_result_free(alloc, &result);
    if (err != HU_OK)
        return HU_ERR_PROVIDER_UNAVAILABLE;

    FILE *sf = fopen(sums_path, "r");
    (void)remove(sums_path);
    if (!sf)
        return HU_ERR_IO;

    char expected_hex[65] = {0};
    char line[512];
    while (fgets(line, (int)sizeof(line), sf)) {
        char *space = strchr(line, ' ');
        if (!space)
            continue;
        /* Format: "<hash>  <filename>" or "<hash> <filename>" */
        const char *fname = space + 1;
        while (*fname == ' ' || *fname == '*')
            fname++;
        size_t flen = strlen(fname);
        while (flen > 0 && (fname[flen - 1] == '\n' || fname[flen - 1] == '\r'))
            flen--;
        if (flen == strlen(asset) && strncmp(fname, asset, flen) == 0) {
            size_t hlen = (size_t)(space - line);
            if (hlen == 64) {
                memcpy(expected_hex, line, 64);
                expected_hex[64] = '\0';
            }
            break;
        }
    }
    fclose(sf);

    if (expected_hex[0] == '\0')
        return HU_ERR_PARSE;

    FILE *bf = fopen(file_path, "rb");
    if (!bf)
        return HU_ERR_IO;
    fseek(bf, 0, SEEK_END);
    long fsize = ftell(bf);
    fseek(bf, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(bf);
        return HU_ERR_IO;
    }

    uint8_t *data = (uint8_t *)alloc->alloc(alloc->ctx, (size_t)fsize);
    if (!data) {
        fclose(bf);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t rd = fread(data, 1, (size_t)fsize, bf);
    fclose(bf);

    uint8_t hash[32];
    hu_sha256(data, rd, hash);
    alloc->free(alloc->ctx, data, (size_t)fsize);

    char actual_hex[65];
    hex_encode(hash, 32, actual_hex);

    for (int i = 0; i < 64; i++) {
        if (expected_hex[i] >= 'A' && expected_hex[i] <= 'F')
            expected_hex[i] = (char)(expected_hex[i] + 32);
    }

    if (strcmp(actual_hex, expected_hex) != 0) {
        hu_log_info("update", NULL, "SHA256 mismatch: expected %s, got %s", expected_hex,
                actual_hex);
        return HU_ERR_INVALID_ARGUMENT;
    }
    return HU_OK;
}
#endif /* !HU_IS_TEST */

/* ── update check ───────────────────────────────────────────────────── */

hu_error_t hu_update_check(char *version_buf, size_t buf_size) {
    if (!version_buf || buf_size == 0)
        return HU_ERR_INVALID_ARGUMENT;
    version_buf[0] = '\0';

#if HU_IS_TEST
    {
        const char *mock = "99.99.99";
        size_t len = strlen(mock);
        if (len >= buf_size)
            len = buf_size - 1;
        memcpy(version_buf, mock, len);
        version_buf[len] = '\0';
        return HU_OK;
    }
#else
    hu_allocator_t alloc = hu_system_allocator();
    const char *argv[] = {"curl", "-sf", "--max-time", "30", GITHUB_API_URL, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 10 * 1024 * 1024, &result);
    if (err != HU_OK) {
        hu_run_result_free(&alloc, &result);
        return HU_ERR_PROVIDER_UNAVAILABLE;
    }
    if (!result.success || !result.stdout_buf || result.stdout_len == 0) {
        hu_run_result_free(&alloc, &result);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(&alloc, result.stdout_buf, result.stdout_len, &parsed);
    hu_run_result_free(&alloc, &result);
    if (err != HU_OK) {
        if (parsed)
            hu_json_free(&alloc, parsed);
        return HU_ERR_PARSE;
    }

    hu_json_value_t *tag_val = hu_json_object_get(parsed, "tag_name");
    if (!tag_val || tag_val->type != HU_JSON_STRING) {
        hu_json_free(&alloc, parsed);
        return HU_ERR_PARSE;
    }

    size_t len = tag_val->data.string.len;
    if (len >= buf_size)
        len = buf_size - 1;
    memcpy(version_buf, tag_val->data.string.ptr, len);
    version_buf[len] = '\0';
    hu_json_free(&alloc, parsed);
    return HU_OK;
#endif
}

/* ── update apply ───────────────────────────────────────────────────── */

hu_error_t hu_update_apply(void) {
#if HU_IS_TEST
    return HU_OK;
#else
    hu_allocator_t alloc = hu_system_allocator();
    char *exe_path = get_exe_path(&alloc);
    if (!exe_path)
        return HU_ERR_IO;

    install_method_t method = detect_install_method(exe_path);
    if (method != INSTALL_BINARY) {
        print_package_instructions(method);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_OK;
    }

    const char *asset = get_platform_asset();
    if (!asset) {
        printf("Unsupported platform for auto-update. Please download manually from:\n  %s\n",
               "https://github.com/sethdford/h-uman/releases/latest");
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_NOT_SUPPORTED;
    }

    char *tmp_dir = hu_platform_get_temp_dir(&alloc);
    if (!tmp_dir) {
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_IO;
    }

    char tmp_path[512];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/human_update.partial", tmp_dir);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_IO;
    }

    char url[256];
    n = snprintf(url, sizeof(url), "%s%s", RELEASE_BASE, asset);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_INVALID_ARGUMENT;
    }

    const char *argv[] = {"curl", "-sfL", "--max-time", "60", "-o", tmp_path, url, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(&alloc, argv, NULL, 64, &result);
    if (err != HU_OK) {
        hu_run_result_free(&alloc, &result);
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_PROVIDER_UNAVAILABLE;
    }
    if (!result.success) {
        hu_run_result_free(&alloc, &result);
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_PROVIDER_RESPONSE;
    }
    hu_run_result_free(&alloc, &result);

    FILE *f = fopen(tmp_path, "rb");
    if (!f) {
        (void)remove(tmp_path);
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_IO;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (size <= 0) {
        (void)remove(tmp_path);
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_PROVIDER_RESPONSE;
    }

    err = verify_sha256(&alloc, tmp_path, asset, tmp_dir);
    if (err != HU_OK) {
        hu_log_error("update", NULL, "Integrity check failed — aborting update.");
        (void)remove(tmp_path);
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return err;
    }

#if defined(__unix__) || defined(__APPLE__)
    chmod(tmp_path, 0755);
#endif
#if defined(__unix__) || defined(__APPLE__)
    if (rename(tmp_path, exe_path) != 0) {
        (void)remove(tmp_path);
        alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return HU_ERR_IO;
    }
#else
    (void)remove(tmp_path);
    alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
    alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
    return HU_ERR_NOT_SUPPORTED;
#endif

    printf("Updated successfully. Restart human to use the new version.\n");
    alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
    alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
    return HU_OK;
#endif
}

/* ── periodic auto-check ────────────────────────────────────────────── */

hu_error_t hu_update_maybe_check(hu_allocator_t *alloc, const hu_config_t *cfg) {
    if (!alloc || !cfg)
        return HU_ERR_INVALID_ARGUMENT;

    if (!cfg->auto_update || strcmp(cfg->auto_update, "off") == 0)
        return HU_OK;

#if HU_IS_TEST
    return HU_OK;
#else
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_OK;

    char ts_path[512];
    int n = snprintf(ts_path, sizeof(ts_path), "%s/.human/%s", home, LAST_CHECK_FILE);
    if (n < 0 || (size_t)n >= sizeof(ts_path))
        return HU_OK;

    uint32_t interval_hours = cfg->update_check_interval_hours;
    if (interval_hours == 0)
        interval_hours = 24;
    time_t now = time(NULL);
    time_t interval_secs = (time_t)interval_hours * 3600;

    FILE *tf = fopen(ts_path, "r");
    if (tf) {
        char tbuf[32];
        if (fgets(tbuf, (int)sizeof(tbuf), tf)) {
            long long ts = 0;
            for (int i = 0; tbuf[i] >= '0' && tbuf[i] <= '9'; i++)
                ts = ts * 10 + (tbuf[i] - '0');
            if (ts > 0 && (now - (time_t)ts) < interval_secs) {
                fclose(tf);
                return HU_OK;
            }
        }
        fclose(tf);
    }

    char latest[64];
    hu_error_t err = hu_update_check(latest, sizeof(latest));

    /* Write timestamp regardless of check success to avoid repeated failures */
    tf = fopen(ts_path, "w");
    if (tf) {
        fprintf(tf, "%lld\n", (long long)now);
        fclose(tf);
    }

    if (err != HU_OK)
        return HU_OK;

    const char *current = hu_version_string();
    const char *remote = latest;
    if (remote[0] == 'v')
        remote++;

    if (hu_version_compare(current, remote) >= 0)
        return HU_OK;

    if (strcmp(cfg->auto_update, "apply") == 0) {
        printf("Update available: %s -> %s. Downloading...\n", current, latest);
        err = hu_update_apply();
        if (err != HU_OK)
            hu_log_error("update", NULL, "Auto-update failed. Run 'human update' manually.");
        return HU_OK;
    }

    /* "check" mode — notify only */
    printf("Update available: %s -> %s. Run 'human update' to install.\n", current, latest);
    return HU_OK;
#endif
}
