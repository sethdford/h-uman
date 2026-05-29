/* src/doctor/json_output.c
 *
 * Sprint 54 US-C3.7 (Phase 1) — Doctor JSON v1 emitter.
 *
 * Pure function that emits the v1 schema for a check-result array to
 * a FILE *. The function is callable from tests (via fmemopen) without
 * needing the full doctor binary.
 *
 * Schema v1 is LOCKED for BREAKING changes — renames, type swaps, and
 * removals ship as v2 via explicit opt-in to prevent silent consumer
 * breakage. ADDITIVE changes (new optional fields, omitted when null)
 * stay in v1 because every reasonable JSON consumer ignores unknown
 * keys. Per docs/guides/doctor.md.
 *
 * Phase 1 scope: emitter + 14 contract tests + docs.
 * Phase 2 (deferred): --json CLI flag wired in cmd_doctor() to invoke
 *   this emitter. Gated on registry-driven main() rewrite (separate
 *   sprint story).
 */

#include "human/doctor.h"
#include "human/doctor/check.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Emit a single string field's value with JSON escaping. The schema
 * only contains four string fields (`ts`, `name`, `verdict`, `reason`)
 * and three of them are system-generated. Only `reason` carries
 * potentially-user-derived content, so the escape logic stays simple:
 * escape `"` and `\`, drop control bytes (<0x20). */
static void emit_escaped_string(FILE *out, const char *s) {
    if (!s) {
        fputs("\"\"", out);
        return;
    }
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            fputc('\\', out);
            fputc((int)c, out);
        } else if (c == '\n') {
            fputs("\\n", out);
        } else if (c == '\r') {
            fputs("\\r", out);
        } else if (c == '\t') {
            fputs("\\t", out);
        } else if (c < 0x20) {
            /* Drop other control bytes; JSON requires them to be
             * escaped as \uNNNN but our reason strings shouldn't
             * carry them anyway. */
            continue;
        } else {
            fputc((int)c, out);
        }
    }
    fputc('"', out);
}

/* Render epoch seconds as a fixed-width ISO-8601 UTC timestamp.
 * Format: "YYYY-MM-DDTHH:MM:SSZ" — 20 chars + NUL.
 *
 * The caller passes the time_t so tests can pin a deterministic value
 * rather than calling time(NULL) (which would produce a different
 * timestamp on every run and break golden-output comparisons). */
static void render_iso8601_utc(time_t epoch, char *buf, size_t cap) {
    if (cap < 21) {
        if (cap > 0)
            buf[0] = '\0';
        return;
    }
    struct tm tm_utc;
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (gmtime_s(&tm_utc, &epoch) != 0) {
        buf[0] = '\0';
        return;
    }
#else
    if (!gmtime_r(&epoch, &tm_utc)) {
        buf[0] = '\0';
        return;
    }
#endif
    /* Clamp every field to its valid range before formatting. gmtime_r
     * already produces in-range values, but GCC's -Wformat-truncation pass
     * treats the struct tm int members as unbounded, so without explicit
     * clamps it cannot prove the directives fit within cap (>= 21 from the
     * guard above) and fails the arm64 -Werror build. Clamping both
     * satisfies the analyzer (each directive now has a fixed width, total
     * exactly 20 chars) and hardens against a corrupt tm struct. */
    int year = tm_utc.tm_year + 1900;
    if (year < 0)
        year = 0;
    if (year > 9999)
        year = 9999;
    int mon = tm_utc.tm_mon + 1;
    if (mon < 1)
        mon = 1;
    if (mon > 12)
        mon = 12;
    int mday = tm_utc.tm_mday;
    if (mday < 1)
        mday = 1;
    if (mday > 31)
        mday = 31;
    int hour = tm_utc.tm_hour;
    if (hour < 0)
        hour = 0;
    if (hour > 23)
        hour = 23;
    int minute = tm_utc.tm_min;
    if (minute < 0)
        minute = 0;
    if (minute > 59)
        minute = 59;
    int sec = tm_utc.tm_sec;
    if (sec < 0)
        sec = 0;
    if (sec > 60) /* allow a leap second */
        sec = 60;
    snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, mon, mday, hour, minute, sec);
}

/* Map a verdict enum to the schema's stable lowercase string.
 * NA collapses to "pass" because the aggregate contract is binary:
 * a check that isn't applicable on this platform shouldn't cause the
 * aggregate to fail. */
static const char *verdict_to_string(hu_doctor_verdict_t v) {
    switch (v) {
    case HU_DOCTOR_PASS:
    case HU_DOCTOR_NA:
        return "pass";
    case HU_DOCTOR_FAIL:
        return "fail";
    default:
        /* Unknown verdict → "fail" so the aggregate flags it. */
        return "fail";
    }
}

hu_error_t hu_doctor_emit_json_v1(const hu_doctor_json_entry_t *entries, size_t count,
                                  time_t now_epoch, FILE *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    /* entries may be NULL with count==0 (no checks ran); that's a
     * structurally valid input that emits {checks:[],aggregate:"pass"}. */
    if (count > 0 && !entries)
        return HU_ERR_INVALID_ARGUMENT;

    char ts_buf[32];
    render_iso8601_utc(now_epoch, ts_buf, sizeof(ts_buf));

    /* Compute aggregate: "pass" iff every verdict is PASS or NA. */
    bool any_fail = false;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].verdict == HU_DOCTOR_FAIL) {
            any_fail = true;
            break;
        }
        if (entries[i].verdict != HU_DOCTOR_PASS && entries[i].verdict != HU_DOCTOR_NA) {
            any_fail = true;
            break;
        }
    }
    const char *aggregate = any_fail ? "fail" : "pass";

    fputs("{\"version\":1,\"ts\":", out);
    emit_escaped_string(out, ts_buf);
    fputs(",\"checks\":[", out);
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            fputc(',', out);
        fputs("{\"name\":", out);
        emit_escaped_string(out, entries[i].name ? entries[i].name : "");
        fputs(",\"verdict\":\"", out);
        fputs(verdict_to_string((hu_doctor_verdict_t)entries[i].verdict), out);
        fputs("\",\"reason\":", out);
        emit_escaped_string(out, entries[i].reason ? entries[i].reason : "");
        /* Optional `detail` field — emitted as RAW JSON (not quoted) because
         * detail_json is by-contract a pre-encoded JSON value (object/array)
         * produced by the check. Omitted when NULL or empty so the v1 shape
         * is unchanged for checks that don't set it. */
        if (entries[i].detail_json && entries[i].detail_json[0] != '\0') {
            fputs(",\"detail\":", out);
            fputs(entries[i].detail_json, out);
        }
        fputc('}', out);
    }
    fputs("],\"aggregate\":\"", out);
    fputs(aggregate, out);
    fputs("\"}\n", out);

    return HU_OK;
}
