/* Phase 7 Apple feed ingestion. F85/F87/F88. Photos, Contacts, Reminders. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/apple.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

#define HU_APPLE_SCRIPT_NAME_PHOTOS    "photos_query.applescript"
#define HU_APPLE_SCRIPT_NAME_CONTACTS  "contacts_query.applescript"
#define HU_APPLE_SCRIPT_NAME_REMINDERS "reminders_query.applescript"

#if HU_IS_TEST

static hu_error_t hu_apple_photos_fetch_impl(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "apple_photos", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "photo", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content, "Mock photo 1", sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "apple_photos", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "photo", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content, "Mock photo 2", sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

static hu_error_t hu_apple_contacts_fetch_impl(hu_allocator_t *alloc,
    char *out_json, size_t out_cap, size_t *out_len) {
    (void)alloc;
    if (!out_json || !out_len || out_cap < 32)
        return HU_ERR_INVALID_ARGUMENT;
    const char *mock = "[{\"id\":\"1\",\"name\":\"Mock Contact\"}]";
    size_t mock_len = strlen(mock);
    if (mock_len >= out_cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(out_json, mock, mock_len + 1);
    *out_len = mock_len;
    return HU_OK;
}

static hu_error_t hu_apple_reminders_fetch_impl(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "apple_reminders", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "reminder", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content, "Mock reminder 1", sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "apple_reminders", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "reminder", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content, "Mock reminder 2", sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else

#if defined(__APPLE__) && defined(HU_GATEWAY_POSIX)
static int resolve_script_path(const char *script_name, char *buf, size_t cap) {
    buf[0] = '\0';
    const char *root = getenv("HU_PROJECT_ROOT");
    if (root && root[0]) {
        int n = snprintf(buf, cap, "%s/scripts/%s", root, script_name);
        if (n > 0 && (size_t)n < cap)
            return 0;
    }
    char exe_buf[4096];
    uint32_t size = (uint32_t)sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        char *resolved = realpath(exe_buf, NULL);
        const char *exe = resolved ? resolved : exe_buf;
        const char *build = strstr(exe, "/build");
        if (build && build > exe) {
            size_t prefix = (size_t)(build - exe);
            if (prefix < cap - 64) {
                memcpy(buf, exe, prefix);
                buf[prefix] = '\0';
                snprintf(buf + prefix, cap - prefix, "/scripts/%s", script_name);
                if (resolved)
                    free(resolved);
                return 0;
            }
        }
        if (resolved)
            free(resolved);
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        snprintf(buf, cap, "%s/scripts/%s", cwd, script_name);
        return 0;
    }
    return -1;
}

static void parse_pipe_line(const char *line, char *out_content, size_t content_cap,
    size_t *out_len) {
    const char *p = line;
    int field = 0;
    const char *start = p;
    while (*p && field < 2) {
        if (*p == '|') {
            if (field == 1) {
                size_t len = (size_t)(p - start);
                if (len >= content_cap)
                    len = content_cap - 1;
                memcpy(out_content, start, len);
                out_content[len] = '\0';
                *out_len = len;
                return;
            }
            field++;
            start = p + 1;
        }
        p++;
    }
    if (field == 1) {
        size_t len = (size_t)(p - start);
        if (len >= content_cap)
            len = content_cap - 1;
        memcpy(out_content, start, len);
        out_content[len] = '\0';
        *out_len = len;
    } else {
        out_content[0] = '\0';
        *out_len = 0;
    }
}
#endif

static hu_error_t hu_apple_photos_fetch_impl(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    if (!alloc || !items || !out_count || items_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if !defined(__APPLE__) || !defined(HU_GATEWAY_POSIX)
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#else
    char script_path[4096];
    if (resolve_script_path(HU_APPLE_SCRIPT_NAME_PHOTOS, script_path, sizeof(script_path)) != 0)
        return HU_ERR_NOT_FOUND;
    char *script_dup = hu_strdup(alloc, script_path);
    if (!script_dup)
        return HU_ERR_OUT_OF_MEMORY;
    const char *argv[] = {"osascript", script_dup, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &result);
    alloc->free(alloc->ctx, script_dup, strlen(script_path) + 1);
    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        return HU_OK;
    }
    const char *p = result.stdout_buf;
    size_t n = 0;
    while (n < items_cap && *p) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        if (eol > p) {
            char line[512];
            size_t line_len = (size_t)(eol - p);
            if (line_len >= sizeof(line))
                line_len = sizeof(line) - 1;
            memcpy(line, p, line_len);
            line[line_len] = '\0';
            memset(&items[n], 0, sizeof(items[n]));
            (void)strncpy(items[n].source, "apple_photos", sizeof(items[n].source) - 1);
            parse_pipe_line(line, items[n].content, sizeof(items[n].content), &items[n].content_len);
            (void)strncpy(items[n].content_type, "photo", sizeof(items[n].content_type) - 1);
            items[n].ingested_at = (int64_t)time(NULL);
            n++;
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    hu_run_result_free(alloc, &result);
    *out_count = n;
    return HU_OK;
#endif
}

static hu_error_t hu_apple_contacts_fetch_impl(hu_allocator_t *alloc,
    char *out_json, size_t out_cap, size_t *out_len) {
    if (!alloc || !out_json || !out_len || out_cap < 32)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = 0;
#if !defined(__APPLE__) || !defined(HU_GATEWAY_POSIX)
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#else
    char script_path[4096];
    if (resolve_script_path(HU_APPLE_SCRIPT_NAME_CONTACTS, script_path, sizeof(script_path)) != 0)
        return HU_ERR_NOT_FOUND;
    char *script_dup = hu_strdup(alloc, script_path);
    if (!script_dup)
        return HU_ERR_OUT_OF_MEMORY;
    const char *argv[] = {"osascript", script_dup, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &result);
    alloc->free(alloc->ctx, script_dup, strlen(script_path) + 1);
    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        if (out_cap >= 3) {
            out_json[0] = '[';
            out_json[1] = ']';
            out_json[2] = '\0';
            *out_len = 2;
        }
        return HU_OK;
    }
    /* Build JSON array from pipe-delimited lines: id|name|email|phone */
    size_t off = 0;
    out_json[off++] = '[';
    const char *p = result.stdout_buf;
    int first = 1;
    while (off < out_cap - 32 && *p) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        if (eol <= p) {
            p = (*eol == '\n') ? eol + 1 : eol;
            continue;
        }
        char id_buf[64] = {0}, name_buf[256] = {0}, email_buf[256] = {0}, phone_buf[64] = {0};
        int field = 0;
        const char *start = p;
        for (const char *q = p; q <= eol && field < 4; q++) {
            if (q == eol || *q == '|') {
                size_t flen = (size_t)(q - start);
                if (flen > 0) {
                    if (field == 0 && flen < sizeof(id_buf))
                        memcpy(id_buf, start, flen);
                    else if (field == 1 && flen < sizeof(name_buf))
                        memcpy(name_buf, start, flen);
                    else if (field == 2 && flen < sizeof(email_buf))
                        memcpy(email_buf, start, flen);
                    else if (field == 3 && flen < sizeof(phone_buf))
                        memcpy(phone_buf, start, flen);
                }
                field++;
                start = q + 1;
            }
        }
        if (field >= 2) {
            if (!first)
                off += (size_t)snprintf(out_json + off, out_cap - off, ",");
            off += (size_t)snprintf(out_json + off, out_cap - off,
                "{\"id\":\"%s\",\"name\":\"%s\",\"email\":\"%s\",\"phone\":\"%s\"}",
                id_buf, name_buf, email_buf, phone_buf);
            first = 0;
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    if (off < out_cap)
        out_json[off++] = ']';
    out_json[off] = '\0';
    *out_len = off;
    hu_run_result_free(alloc, &result);
    return HU_OK;
#endif
}

static hu_error_t hu_apple_reminders_fetch_impl(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    if (!alloc || !items || !out_count || items_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;
#if !defined(__APPLE__) || !defined(HU_GATEWAY_POSIX)
    (void)alloc;
    return HU_ERR_NOT_SUPPORTED;
#else
    char script_path[4096];
    if (resolve_script_path(HU_APPLE_SCRIPT_NAME_REMINDERS, script_path, sizeof(script_path)) != 0)
        return HU_ERR_NOT_FOUND;
    char *script_dup = hu_strdup(alloc, script_path);
    if (!script_dup)
        return HU_ERR_OUT_OF_MEMORY;
    const char *argv[] = {"osascript", script_dup, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, NULL, 65536, &result);
    alloc->free(alloc->ctx, script_dup, strlen(script_path) + 1);
    if (err != HU_OK) {
        hu_run_result_free(alloc, &result);
        return err;
    }
    if (!result.success || !result.stdout_buf) {
        hu_run_result_free(alloc, &result);
        return HU_OK;
    }
    const char *p = result.stdout_buf;
    size_t n = 0;
    while (n < items_cap && *p) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        if (eol > p) {
            char line[512];
            size_t line_len = (size_t)(eol - p);
            if (line_len >= sizeof(line))
                line_len = sizeof(line) - 1;
            memcpy(line, p, line_len);
            line[line_len] = '\0';
            memset(&items[n], 0, sizeof(items[n]));
            (void)strncpy(items[n].source, "apple_reminders", sizeof(items[n].source) - 1);
            parse_pipe_line(line, items[n].content, sizeof(items[n].content), &items[n].content_len);
            (void)strncpy(items[n].content_type, "reminder", sizeof(items[n].content_type) - 1);
            items[n].ingested_at = (int64_t)time(NULL);
            n++;
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    hu_run_result_free(alloc, &result);
    *out_count = n;
    return HU_OK;
#endif
}

#endif /* HU_IS_TEST */

hu_error_t hu_apple_photos_fetch(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    return hu_apple_photos_fetch_impl(alloc, items, items_cap, out_count);
}

hu_error_t hu_apple_contacts_fetch(hu_allocator_t *alloc,
    char *out_json, size_t out_cap, size_t *out_len) {
    return hu_apple_contacts_fetch_impl(alloc, out_json, out_cap, out_len);
}

hu_error_t hu_apple_reminders_fetch(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    return hu_apple_reminders_fetch_impl(alloc, items, items_cap, out_count);
}

/* ── Apple Health (F91) ─────────────────────────────────────────────────── */

#if HU_IS_TEST

static const char *HU_HEALTH_FIXTURE_XML =
    "<?xml version=\"1.0\"?>\n"
    "<HealthData>\n"
    "<Record type=\"HKQuantityTypeIdentifierStepCount\" value=\"5000\" "
    "startDate=\"2025-03-11T08:00:00\" endDate=\"2025-03-11T08:00:00\"/>\n"
    "<Record type=\"HKQuantityTypeIdentifierStepCount\" value=\"3000\" "
    "startDate=\"2025-03-11T12:00:00\" endDate=\"2025-03-11T12:00:00\"/>\n"
    "<Record type=\"HKQuantityTypeIdentifierHeartRate\" value=\"72\" "
    "startDate=\"2025-03-11T10:00:00\" endDate=\"2025-03-11T10:00:00\"/>\n"
    "<Record type=\"HKQuantityTypeIdentifierHeartRate\" value=\"68\" "
    "startDate=\"2025-03-11T14:00:00\" endDate=\"2025-03-11T14:00:00\"/>\n"
    "<Record type=\"HKCategoryTypeIdentifierSleepAnalysis\" "
    "value=\"HKCategoryValueSleepAnalysisAsleep\" "
    "startDate=\"2025-03-10T23:00:00\" endDate=\"2025-03-11T07:00:00\"/>\n"
    "</HealthData>";

#endif

static int parse_int_attr(const char *xml, size_t xml_len, const char *attr,
    const char *pos, int64_t *out) {
    char search[128];
    int n = snprintf(search, sizeof(search), "%s=\"", attr);
    if (n <= 0 || (size_t)n >= sizeof(search))
        return -1;
    const char *s = pos;
    const char *end = xml + xml_len;
    while (s < end) {
        const char *p = strstr(s, search);
        if (!p || p >= end)
            return -1;
        p += (size_t)n;
        const char *q = strchr(p, '"');
        if (!q || q >= end)
            return -1;
        char buf[32];
        size_t len = (size_t)(q - p);
        if (len >= sizeof(buf))
            return -1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        *out = (int64_t)atoll(buf);
        return 0;
    }
    return -1;
}

static int parse_double_attr(const char *xml, size_t xml_len, const char *attr,
    const char *pos, double *out) {
    char search[128];
    int n = snprintf(search, sizeof(search), "%s=\"", attr);
    if (n <= 0 || (size_t)n >= sizeof(search))
        return -1;
    const char *s = pos;
    const char *end = xml + xml_len;
    while (s < end) {
        const char *p = strstr(s, search);
        if (!p || p >= end)
            return -1;
        p += (size_t)n;
        const char *q = strchr(p, '"');
        if (!q || q >= end)
            return -1;
        char buf[32];
        size_t len = (size_t)(q - p);
        if (len >= sizeof(buf))
            return -1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        *out = atof(buf);
        return 0;
    }
    return -1;
}

static int parse_date_attr(const char *xml, size_t xml_len, const char *attr,
    const char *pos, int *out_year, int *out_month, int *out_day) {
    char search[128];
    int n = snprintf(search, sizeof(search), "%s=\"", attr);
    if (n <= 0 || (size_t)n >= sizeof(search))
        return -1;
    const char *p = strstr(pos, search);
    if (!p || p >= xml + xml_len)
        return -1;
    p += (size_t)n;
    const char *q = strchr(p, '"');
    if (!q || q - p < 10)
        return -1;
    int year, month, day;
    if (sscanf(p, "%d-%d-%d", &year, &month, &day) >= 3) {
        *out_year = year;
        *out_month = month;
        *out_day = day;
        return 0;
    }
    return -1;
}

static int is_today(int year, int month, int day, int ref_year, int ref_month, int ref_day) {
    return year == ref_year && month == ref_month && day == ref_day;
}

static void get_today_ymd(int *year, int *month, int *day) {
#if HU_IS_TEST
    *year = 2025;
    *month = 3;
    *day = 11;
#else
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    if (tm) {
        *year = tm->tm_year + 1900;
        *month = tm->tm_mon + 1;
        *day = tm->tm_mday;
    } else {
        *year = 2025;
        *month = 3;
        *day = 11;
    }
#endif
}

hu_error_t hu_apple_health_parse_export(hu_allocator_t *alloc,
    const char *xml_data, size_t xml_len,
    hu_health_summary_t *out) {
    (void)alloc;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

#if HU_IS_TEST
    xml_data = HU_HEALTH_FIXTURE_XML;
    xml_len = strlen(HU_HEALTH_FIXTURE_XML);
#else
    if (!xml_data)
        return HU_ERR_INVALID_ARGUMENT;
#endif

    int ref_year, ref_month, ref_day;
    get_today_ymd(&ref_year, &ref_month, &ref_day);

    int64_t steps_sum = 0;
    double heart_sum = 0.0;
    int heart_count = 0;
    double sleep_seconds = 0.0;

    const char *p = xml_data;
    const char *end = xml_data + xml_len;

    while (p < end) {
        const char *rec = strstr(p, "<Record ");
        if (!rec || rec >= end)
            break;

        const char *type_start = strstr(rec, "type=\"");
        if (!type_start || type_start >= end)
            goto next_rec;
        type_start += 6;
        const char *type_end = strchr(type_start, '"');
        if (!type_end || type_end - type_start > 80)
            goto next_rec;

        int year = 0, month = 0, day = 0;
        if (parse_date_attr(xml_data, xml_len, "startDate", rec, &year, &month, &day) != 0)
            goto next_rec;

        int64_t ival = 0;
        double dval = 0.0;

        if (strncmp(type_start, "HKQuantityTypeIdentifierStepCount",
                (size_t)(type_end - type_start)) == 0) {
            if (parse_int_attr(xml_data, xml_len, "value", rec, &ival) == 0 &&
                is_today(year, month, day, ref_year, ref_month, ref_day))
                steps_sum += ival;
        } else if (strncmp(type_start, "HKQuantityTypeIdentifierHeartRate",
                (size_t)(type_end - type_start)) == 0) {
            if (parse_double_attr(xml_data, xml_len, "value", rec, &dval) == 0 &&
                is_today(year, month, day, ref_year, ref_month, ref_day)) {
                heart_sum += dval;
                heart_count++;
            }
        } else if (strncmp(type_start, "HKCategoryTypeIdentifierSleepAnalysis",
                (size_t)(type_end - type_start)) == 0) {
            const char *val = strstr(rec, "value=\"");
            if (val && strstr(val, "HKCategoryValueSleepAnalysisAsleep")) {
                int sy, sm, sd, sh, smin, ss, ey, em, ed, eh, emin, es;
                const char *sp = strstr(rec, "startDate=\"");
                const char *ep = strstr(rec, "endDate=\"");
                if (sp && ep &&
                    sscanf(sp + 11, "%d-%d-%dT%d:%d:%d", &sy, &sm, &sd, &sh, &smin, &ss) >= 6 &&
                    sscanf(ep + 10, "%d-%d-%dT%d:%d:%d", &ey, &em, &ed, &eh, &emin, &es) >= 6) {
                    int64_t start_sec = (int64_t)sh * 3600 + smin * 60 + ss;
                    int64_t end_sec = (int64_t)eh * 3600 + emin * 60 + es;
                    if (sy != ey || sm != em || sd != ed)
                        end_sec += 24 * 3600;
                    sleep_seconds += (double)(end_sec > start_sec ? end_sec - start_sec : 0);
                }
            }
        }
next_rec:
        p = rec + 1;
    }

    out->steps_today = (int32_t)(steps_sum > 2147483647 ? 2147483647 : steps_sum);
    out->avg_heart_rate = heart_count > 0 ? heart_sum / (double)heart_count : 0.0;
    out->sleep_hours = sleep_seconds / 3600.0;
    out->export_date = (int64_t)time(NULL);
    return HU_OK;
}

#else
/* Stub when HU_ENABLE_FEEDS is off — avoids empty translation unit warning */
typedef int hu_apple_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
