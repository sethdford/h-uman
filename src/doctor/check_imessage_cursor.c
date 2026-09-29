/* src/doctor/check_imessage_cursor.c — see include/human/doctor/check_ops.h
 *
 * The 2026-09-01 replay: ~/.human/imessage.rowid sat at 69288 (Aug 17) while
 * chat.db's max ROWID was 70484. Nothing compared the two until a reboot made
 * the daemon "recover" 1,196 messages and answer them. The resume cap now
 * bounds the damage (HU_IMESSAGE_MAX_REPLAY); this check makes the gap
 * visible BEFORE a restart. */
#include "human/doctor/check_ops.h"
#include "human/memory/chatdb_cursor_repo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_reason[512];
static char s_detail[256];

static bool read_int64_file(const char *path, int64_t *out) {
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return false;
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (end == buf)
        return false;
    *out = (int64_t)v;
    return true;
}

/* NA with the cursor/max_gap detail; cursor < 0 renders as null. */
static hu_doctor_check_result_t na_result(int64_t cursor, int64_t max_gap) {
    if (cursor < 0)
        snprintf(s_detail, sizeof(s_detail),
                 "{\"cursor\":null,\"chatdb_max\":null,\"max_gap\":%lld}", (long long)max_gap);
    else
        snprintf(s_detail, sizeof(s_detail),
                 "{\"cursor\":%lld,\"chatdb_max\":null,\"max_gap\":%lld}", (long long)cursor,
                 (long long)max_gap);
    return hu_doctor_ops_result(HU_DOCTOR_NA, s_reason, s_detail);
}

static hu_doctor_check_result_t run(hu_doctor_check_t *self, void *vctx) {
    (void)self;
    const hu_doctor_imessage_cursor_ctx_t *ctx = (const hu_doctor_imessage_cursor_ctx_t *)vctx;
    char rb[512], cb[512];
    const char *rowid_path = hu_doctor_ops_home_path(ctx ? ctx->rowid_path : NULL, rb, sizeof(rb),
                                                     ".human/imessage.rowid");
    const char *chatdb = hu_doctor_ops_home_path(ctx ? ctx->chatdb_path : NULL, cb, sizeof(cb),
                                                 "Library/Messages/chat.db");
    int64_t max_gap = (ctx && ctx->max_gap) ? ctx->max_gap : 50;
    int64_t cursor = 0;
    if (!read_int64_file(rowid_path, &cursor)) {
        snprintf(s_reason, sizeof(s_reason), "no persisted cursor at %s",
                 rowid_path ? rowid_path : "?");
        return na_result(-1, max_gap);
    }
    int64_t dbmax = 0;
    hu_error_t rerr = hu_chatdb_max_rowid(chatdb, &dbmax);
    if (rerr == HU_ERR_NOT_SUPPORTED) {
        snprintf(s_reason, sizeof(s_reason), "SQLite not compiled in; cannot read chat.db");
        return na_result(cursor, max_gap);
    }
    if (rerr != HU_OK) {
        snprintf(s_reason, sizeof(s_reason), "chat.db not readable or has no message table (%s)",
                 chatdb ? chatdb : "?");
        return na_result(cursor, max_gap);
    }
    int64_t gap = dbmax - cursor;
    snprintf(s_detail, sizeof(s_detail),
             "{\"cursor\":%lld,\"chatdb_max\":%lld,\"gap\":%lld,\"max_gap\":%lld}",
             (long long)cursor, (long long)dbmax, (long long)gap, (long long)max_gap);
    if (gap > max_gap) {
        snprintf(
            s_reason, sizeof(s_reason),
            "iMessage cursor lags chat.db by %lld rows (cursor %lld, db max %lld, limit %lld) — "
            "a daemon restart would treat those as fresh inbound (2026-09-01 replay shape); "
            "the resume cap bounds it, but find out why the poll is not advancing",
            (long long)gap, (long long)cursor, (long long)dbmax, (long long)max_gap);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    snprintf(s_reason, sizeof(s_reason), "cursor %lld, chat.db max %lld, gap %lld",
             (long long)cursor, (long long)dbmax, (long long)gap);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
}

const hu_doctor_check_t hu_doctor_check_imessage_cursor = {
    .name = "imessage_cursor",
    .description =
        "Persisted iMessage cursor is close to chat.db's max ROWID (no replay on restart)",
    .run = run,
    .fix = NULL,
    .user_data = NULL,
};
