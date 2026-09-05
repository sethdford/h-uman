/* src/doctor/check_blind_ab_gate.c — see include/human/doctor/check_ops.h
 *
 * Two files claim to be "the blind-A/B gate": ~/.human/blind_ab_gate.json
 * (read by the C LoRA promotion gate) and docs/evaluation/blind_ab_gate.json
 * (read by CI's freshness + capability-gate checks). On 2026-09-02 they
 * disagreed (n=40 detection 0.225 vs n=12 detection 0.500, different dates),
 * neither said WHICH adapter was measured, and the adapter actually being
 * served (v6-orpo, promoted 08-02) had never been human-rated at all. A gate
 * that cannot be tied to what is served vouches for nothing. */
#include "human/doctor/check_ops.h"

#include "human/core/allocator.h"
#include "human/core/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char s_reason[768];
static char s_detail[768];

typedef struct human_half {
    bool present;
    char tool[64];
    char verdict[16];
    char timestamp[32];
    char arm_adapter[256];
    double n;
    double detection;
} human_half_t;

static char *read_file(hu_allocator_t *alloc, const char *path, size_t *out_len) {
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return NULL;
    char *buf = (char *)alloc->alloc(alloc->ctx, 65536);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static const char *basename_of(const char *p) {
    if (!p)
        return "";
    const char *s = strrchr(p, '/');
    /* tolerate a trailing slash */
    if (s && s[1] == '\0') {
        size_t n = (size_t)(s - p);
        while (n > 0 && p[n - 1] != '/')
            n--;
        return p + n;
    }
    return s ? s + 1 : p;
}

static void load_half(hu_allocator_t *alloc, const char *path, human_half_t *h) {
    memset(h, 0, sizeof(*h));
    size_t len = 0;
    char *text = read_file(alloc, path, &len);
    if (!text)
        return;
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, text, len, &root) == HU_OK && root) {
        hu_json_value_t *human = hu_json_object_get(root, "human");
        if (human && human->type == HU_JSON_OBJECT) {
            h->present = true;
            const char *s;
            if ((s = hu_json_get_string(human, "tool")))
                snprintf(h->tool, sizeof(h->tool), "%s", s);
            if ((s = hu_json_get_string(human, "verdict")))
                snprintf(h->verdict, sizeof(h->verdict), "%s", s);
            if ((s = hu_json_get_string(human, "timestamp")))
                snprintf(h->timestamp, sizeof(h->timestamp), "%s", s);
            h->n = hu_json_get_number(human, "n", 0);
            h->detection = hu_json_get_number(human, "detection", -1);
            hu_json_value_t *arm = hu_json_object_get(human, "arm");
            if (arm && arm->type == HU_JSON_OBJECT && (s = hu_json_get_string(arm, "adapter")))
                snprintf(h->arm_adapter, sizeof(h->arm_adapter), "%s", basename_of(s));
        }
        hu_json_free(alloc, root);
    }
    alloc->free(alloc->ctx, text, 65536);
}

/* "YYYY-MM-DDTHH:MM:SS" → unix (local); 0 when unparseable. */
int64_t hu_doctor_gate_parse_ts(const char *ts) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!ts || sscanf(ts, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                      &tm.tm_min, &tm.tm_sec) < 3)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t < 0 ? 0 : (int64_t)t;
}

static hu_doctor_check_result_t run(hu_doctor_check_t *self, void *vctx) {
    (void)self;
    const hu_doctor_blind_ab_gate_ctx_t *ctx = (const hu_doctor_blind_ab_gate_ctx_t *)vctx;
    hu_allocator_t alloc = hu_system_allocator();
    char hb[512];
    const char *home_gate = hu_doctor_ops_home_path(ctx ? ctx->home_gate : NULL, hb, sizeof(hb),
                                                    ".human/blind_ab_gate.json");
    const char *repo_gate = ctx ? ctx->repo_gate : NULL;
    const char *served = (ctx && ctx->served_adapter) ? basename_of(ctx->served_adapter) : NULL;
    int64_t now = (ctx && ctx->now_unix) ? ctx->now_unix : (int64_t)time(NULL);
    int max_days = (ctx && ctx->max_age_days) ? ctx->max_age_days : 45;

    human_half_t hh, rh;
    load_half(&alloc, home_gate, &hh);
    load_half(&alloc, repo_gate, &rh);

    snprintf(s_detail, sizeof(s_detail),
             "{\"home\":{\"present\":%s,\"verdict\":\"%s\",\"n\":%.0f,\"detection\":%.3f,"
             "\"timestamp\":\"%s\",\"tool\":\"%s\",\"arm_adapter\":\"%s\"},"
             "\"repo\":{\"present\":%s,\"verdict\":\"%s\",\"n\":%.0f,\"timestamp\":\"%s\"},"
             "\"served_adapter\":\"%s\",\"max_age_days\":%d}",
             hh.present ? "true" : "false", hh.verdict, hh.n, hh.detection, hh.timestamp, hh.tool,
             hh.arm_adapter, rh.present ? "true" : "false", rh.verdict, rh.n, rh.timestamp,
             served ? served : "", max_days);

    if (!hh.present) {
        snprintf(s_reason, sizeof(s_reason),
                 "no human blind-A/B verdict at %s — the LoRA promotion gate reads only this file; "
                 "an unrated adapter must be HELD, never LIVE",
                 home_gate ? home_gate : "?");
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    if (hh.tool[0] == '\0') {
        snprintf(s_reason, sizeof(s_reason),
                 "human verdict in %s carries no `tool` stamp — unattributable evidence is treated "
                 "as ABSENT (see blind_ab_gate.human_is_attributable)",
                 home_gate);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    if (rh.present && (strcmp(rh.verdict, hh.verdict) != 0 ||
                       strcmp(rh.timestamp, hh.timestamp) != 0 || rh.n != hh.n)) {
        snprintf(
            s_reason, sizeof(s_reason),
            "the two gate files disagree: %s says %s n=%.0f @%s but %s says %s n=%.0f @%s — CI and "
            "the promotion gate are reading different evidence; copy the newer human half across "
            "(scripts/blind_ab_gate.py write_human_half)",
            basename_of(home_gate), hh.verdict, hh.n, hh.timestamp, repo_gate ? repo_gate : "repo",
            rh.verdict, rh.n, rh.timestamp);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    int64_t ts = hu_doctor_gate_parse_ts(hh.timestamp);
    double age_days = ts ? (double)(now - ts) / 86400.0 : -1;
    if (ts && age_days > (double)max_days) {
        snprintf(
            s_reason, sizeof(s_reason),
            "human verdict is %.0f days old (limit %d) — the served model has drifted past the "
            "evidence; run a rating cycle (scripts/blind_ab/rating_drip.py)",
            age_days, max_days);
        return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
    }
    if (served && served[0]) {
        if (hh.arm_adapter[0] == '\0') {
            snprintf(
                s_reason, sizeof(s_reason),
                "human verdict (%s n=%.0f @%s) does not record which adapter it measured, so it "
                "cannot vouch for the served adapter '%s' — record `human.arm.adapter` when "
                "scoring, and rate the served arm",
                hh.verdict, hh.n, hh.timestamp, served);
            return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
        }
        if (strcmp(hh.arm_adapter, served) != 0) {
            snprintf(
                s_reason, sizeof(s_reason),
                "human verdict measured adapter '%s' but the server is serving '%s' — the served "
                "adapter has not been human-rated (promotion without measurement)",
                hh.arm_adapter, served);
            return (hu_doctor_check_result_t){HU_DOCTOR_FAIL, s_reason, s_detail};
        }
    }
    snprintf(s_reason, sizeof(s_reason), "human %s n=%.0f detection=%.3f @%s%s%s", hh.verdict, hh.n,
             hh.detection, hh.timestamp, hh.arm_adapter[0] ? " arm=" : "", hh.arm_adapter);
    return (hu_doctor_check_result_t){HU_DOCTOR_PASS, s_reason, s_detail};
}

const hu_doctor_check_t hu_doctor_check_blind_ab_gate = {
    .name = "blind_ab_gate",
    .description = "Human blind-A/B verdict is attributable, consistent across gate files, fresh, "
                   "and measured the served adapter",
    .run = run,
    .fix = NULL,
    .user_data = NULL,
};
