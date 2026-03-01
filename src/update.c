/*
 * Self-update: check GitHub releases, download and replace binary.
 * In SC_IS_TEST mode, returns mock data without network calls.
 */
#include "seaclaw/update.h"
#include "seaclaw/version.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/process_util.h"
#include "seaclaw/core/string.h"
#include "seaclaw/platform.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define GITHUB_API_URL "https://api.github.com/repos/seaclaw/seaclaw/releases/latest"
#define RELEASE_BASE "https://github.com/seaclaw/seaclaw/releases/latest/download/"

typedef enum { INSTALL_NIX, INSTALL_HOMEBREW, INSTALL_DOCKER, INSTALL_BINARY, INSTALL_DEV } install_method_t;

static install_method_t detect_install_method(const char *exe_path) {
    if (!exe_path) return INSTALL_BINARY;
    if (strstr(exe_path, "/nix/store/")) return INSTALL_NIX;
    if (strstr(exe_path, "/homebrew/") || strstr(exe_path, "/Cellar/")) return INSTALL_HOMEBREW;
    if (strcmp(exe_path, "/seaclaw") == 0) return INSTALL_DOCKER;
    if (strstr(exe_path, "/build/")) return INSTALL_DEV;
    return INSTALL_BINARY;
}

static char *get_exe_path(sc_allocator_t *alloc) {
#if defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    return sc_strdup(alloc, buf);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
    char *resolved = realpath(buf, NULL);
    if (resolved) {
        char *out = sc_strdup(alloc, resolved);
        free(resolved);
        return out;
    }
    return sc_strdup(alloc, buf);
#else
    (void)alloc;
    return NULL;
#endif
}

static const char *get_platform_asset(void) {
#if defined(__linux__) && defined(__x86_64__)
    return "seaclaw-linux-x86_64.bin";
#elif defined(__linux__) && defined(__aarch64__)
    return "seaclaw-linux-aarch64.bin";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "seaclaw-macos-aarch64.bin";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "seaclaw-macos-x86_64.bin";
#elif defined(_WIN32) || defined(_WIN64)
    return "seaclaw-windows-x86_64.exe";
#else
    return NULL;
#endif
}

static void print_package_instructions(install_method_t method) {
    switch (method) {
        case INSTALL_NIX:
            printf("Detected installation via: Nix\nTo update, run:\n  nix-channel --update && nix-env -iA nixpkgs.seaclaw\n");
            break;
        case INSTALL_HOMEBREW:
            printf("Detected installation via: Homebrew\nTo update, run:\n  brew upgrade seaclaw\n");
            break;
        case INSTALL_DOCKER:
            printf("Detected installation via: Docker\nTo update, run:\n  docker pull ghcr.io/seaclaw/seaclaw:latest\n");
            break;
        case INSTALL_DEV:
            printf("Development installation detected.\nTo update, run:\n  git pull && cmake --build build && make -C build\n");
            break;
        default:
            break;
    }
}

sc_error_t sc_update_check(char *version_buf, size_t buf_size) {
    if (!version_buf || buf_size == 0) return SC_ERR_INVALID_ARGUMENT;
    version_buf[0] = '\0';

#if SC_IS_TEST
    {
        const char *mock = "99.99.99-mock";
        size_t len = strlen(mock);
        if (len >= buf_size) len = buf_size - 1;
        memcpy(version_buf, mock, len);
        version_buf[len] = '\0';
        return SC_OK;
    }
#else
    sc_allocator_t alloc = sc_system_allocator();
    const char *argv[] = {"curl", "-sf", "--max-time", "30", GITHUB_API_URL, NULL};
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run(&alloc, argv, NULL, 10 * 1024 * 1024, &result);
    if (err != SC_OK) {
        sc_run_result_free(&alloc, &result);
        return SC_ERR_PROVIDER_UNAVAILABLE;
    }
    if (!result.success || !result.stdout_buf || result.stdout_len == 0) {
        sc_run_result_free(&alloc, &result);
        return SC_ERR_PROVIDER_RESPONSE;
    }

    sc_json_value_t *parsed = NULL;
    err = sc_json_parse(&alloc, result.stdout_buf, result.stdout_len, &parsed);
    sc_run_result_free(&alloc, &result);
    if (err != SC_OK) {
        if (parsed) sc_json_free(&alloc, parsed);
        return SC_ERR_PARSE;
    }

    sc_json_value_t *tag_val = sc_json_object_get(parsed, "tag_name");
    if (!tag_val || tag_val->type != SC_JSON_STRING) {
        sc_json_free(&alloc, parsed);
        return SC_ERR_PARSE;
    }

    size_t len = tag_val->data.string.len;
    if (len >= buf_size) len = buf_size - 1;
    memcpy(version_buf, tag_val->data.string.ptr, len);
    version_buf[len] = '\0';
    sc_json_free(&alloc, parsed);
    return SC_OK;
#endif
}

sc_error_t sc_update_apply(void) {
#if SC_IS_TEST
    return SC_OK;
#else
    sc_allocator_t alloc = sc_system_allocator();
    char *exe_path = get_exe_path(&alloc);
    if (!exe_path) return SC_ERR_IO;

    install_method_t method = detect_install_method(exe_path);
    if (method != INSTALL_BINARY) {
        print_package_instructions(method);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_OK;
    }

    const char *asset = get_platform_asset();
    if (!asset) {
        printf("Unsupported platform for auto-update. Please download manually from:\n  %s\n", "https://github.com/seaclaw/seaclaw/releases/latest");
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_NOT_SUPPORTED;
    }

    char *tmp_dir = sc_platform_get_temp_dir(&alloc);
    if (!tmp_dir) {
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_IO;
    }

    char tmp_path[512];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s/seaclaw_update.partial", tmp_dir);
    alloc.free(alloc.ctx, tmp_dir, strlen(tmp_dir) + 1);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_IO;
    }

    char url[256];
    n = snprintf(url, sizeof(url), "%s%s", RELEASE_BASE, asset);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_INVALID_ARGUMENT;
    }

    const char *argv[] = {"curl", "-sfL", "--max-time", "60", "-o", tmp_path, url, NULL};
    sc_run_result_t result = {0};
    sc_error_t err = sc_process_run(&alloc, argv, NULL, 64, &result);
    if (err != SC_OK) {
        sc_run_result_free(&alloc, &result);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_PROVIDER_UNAVAILABLE;
    }
    if (!result.success) {
        sc_run_result_free(&alloc, &result);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_PROVIDER_RESPONSE;
    }
    sc_run_result_free(&alloc, &result);

    FILE *f = fopen(tmp_path, "rb");
    if (!f) {
        (void)remove(tmp_path);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_IO;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (size <= 0) {
        (void)remove(tmp_path);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_PROVIDER_RESPONSE;
    }

#if defined(__unix__) || defined(__APPLE__)
    chmod(tmp_path, 0755);  /* best-effort, ignore failure */
#endif
#if defined(__unix__) || defined(__APPLE__)
    if (rename(tmp_path, exe_path) != 0) {
        (void)remove(tmp_path);
        alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
        return SC_ERR_IO;
    }
#else
    (void)remove(tmp_path);
    alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
    return SC_ERR_NOT_SUPPORTED;
#endif

    printf("Updated successfully. Restart seaclaw to use the new version.\n");
    alloc.free(alloc.ctx, exe_path, strlen(exe_path) + 1);
    return SC_OK;
#endif
}
