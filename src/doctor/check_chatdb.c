#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/doctor/check.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <sys/stat.h>
#endif

/* Static buffers for reason strings to return as borrowed pointers */
static char s_reason_missing[256];
static char s_reason_permission[512];

/**
 * hu_doctor_check_chatdb_readable — Check if ~/Library/Messages/chat.db is readable
 *
 * PASS: File exists and read access succeeds
 * FAIL: "missing" (file doesn't exist) or "permission denied" (EACCES)
 *
 * Platform-specific: macOS only. On non-Apple platforms, returns HU_DOCTOR_NA.
 */
static hu_doctor_check_result_t check_chatdb_readable(hu_doctor_check_t *self, void *ctx) {
    (void)self;
    (void)ctx;

#ifndef __APPLE__
    /* Platform not applicable on non-macOS systems */
    return (hu_doctor_check_result_t){HU_DOCTOR_NA, "", NULL};
#else
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp"; /* fallback — unlikely to have chat.db there, but better than crash */
    }

    /* Build path: ~/Library/Messages/chat.db */
    char path_buf[1024];
    int n = snprintf(path_buf, sizeof(path_buf), "%s/Library/Messages/chat.db", home);
    if (n < 0 || n >= (int)sizeof(path_buf)) {
        snprintf(s_reason_missing, sizeof(s_reason_missing), "path too long");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_missing, NULL};
    }

    /* Try to open file for reading */
    FILE *f = fopen(path_buf, "rb");
    if (f) {
        /* Success: file exists and is readable */
        fclose(f);
        return (hu_doctor_check_result_t){HU_DOCTOR_PASS, "", NULL};
    }

    /* Failed to open. Check errno to distinguish missing vs permission denied */
    int err = errno;
    if (err == ENOENT) {
        /* File doesn't exist */
        snprintf(s_reason_missing, sizeof(s_reason_missing), "missing");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_missing, NULL};
    } else if (err == EACCES) {
        /* Permission denied — likely FDA issue on macOS */
        snprintf(s_reason_permission, sizeof(s_reason_permission),
                 "permission denied\n\n"
                 "Open: System Settings → Privacy & Security → Full Disk Access, "
                 "then enable \"human\"\n\n"
                 "Or click: x-apple.systempreferences:?path=/System/Library/PreferencePanes/"
                 "Security.prefPane?Privacy&Privacy_AllFiles\n");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_permission, NULL};
    } else {
        /* Other error (maybe permission denied under a different errno on this platform) */
        snprintf(s_reason_permission, sizeof(s_reason_permission),
                 "permission denied\n\n"
                 "Open: System Settings → Privacy & Security → Full Disk Access, "
                 "then enable \"human\"\n\n"
                 "Or click: x-apple.systempreferences:?path=/System/Library/PreferencePanes/"
                 "Security.prefPane?Privacy&Privacy_AllFiles\n");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason_permission, NULL};
    }
#endif
}

/* Vtable entry for the check */
hu_doctor_check_t hu_doctor_check_chatdb = {
    .name = "chatdb_readable",
    .description = "Verifies ~/Library/Messages/chat.db is readable (FDA check)",
    .run = check_chatdb_readable,
    .fix = NULL, /* No autofix — user must grant FDA permission in System Settings */
    .user_data = NULL,
};
