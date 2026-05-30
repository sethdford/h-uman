/* src/eval/eval_score.c — `human eval score` core + CLI wrapper.
 *
 * See include/human/eval/eval_score.h and
 * docs/plans/2026-05-29-humanness-north-star-metric/ (Phase 2 / T4).
 *
 * Turns a JSONL of generated replies into per-axis humanness scores using the
 * C scorers as ground truth: A2 anti-AI (hu_shape_classify), A4 relationship
 * (hu_relationship_axis_score), and — when a target style is supplied — A1
 * fidelity (hu_persona_fidelity_score_l1). The Python nightly harness shells
 * out to this and parses the emitted JSON.
 */

#include "human/eval/eval_score.h"

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/eval/cli_eval.h"
#include "human/eval/persona_fidelity.h"
#include "human/eval/register.h"
#include "human/eval/shape.h"
#include "human/memory/personal_model.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── running accumulator ───────────────────────────────────────────────── */

typedef struct {
    double sum;
    double sumsq;
    size_t n;
} acc_t;

static void acc_add(acc_t *a, double x) {
    a->sum += x;
    a->sumsq += x * x;
    a->n++;
}

static double acc_mean(const acc_t *a) {
    return a->n ? a->sum / (double)a->n : 0.0;
}

/* stderr of the mean = sqrt(variance / n). Population variance is fine here;
 * the gate layer applies bootstrap CI for promotion decisions. */
static double acc_stderr(const acc_t *a) {
    if (a->n <= 1)
        return 0.0;
    double m = acc_mean(a);
    double var = (a->sumsq - (double)a->n * m * m) / (double)a->n;
    if (var < 0.0)
        var = 0.0;
    return sqrt(var / (double)a->n);
}

/* ── pure core ─────────────────────────────────────────────────────────── */

hu_error_t hu_eval_score_jsonl(hu_allocator_t *alloc, const char *jsonl, size_t jsonl_len,
                               const hu_communication_style_t *target_style, char **out_json,
                               size_t *out_json_len) {
    if (!alloc || !jsonl || !out_json)
        return HU_ERR_INVALID_ARGUMENT;

    acc_t ai = {0}, rel = {0}, fid = {0};
    size_t total = 0;
    bool fidelity_possible = (target_style != NULL && target_style->sample_count > 0);

    /* Iterate line by line; parse each non-blank line as one JSON object. */
    size_t i = 0;
    while (i < jsonl_len) {
        size_t start = i;
        while (i < jsonl_len && jsonl[i] != '\n')
            i++;
        size_t line_len = i - start;
        if (i < jsonl_len)
            i++; /* skip '\n' */

        /* skip blank / whitespace-only lines */
        bool blank = true;
        for (size_t k = 0; k < line_len; k++) {
            unsigned char c = (unsigned char)jsonl[start + k];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                blank = false;
                break;
            }
        }
        if (blank)
            continue;

        hu_json_value_t *obj = NULL;
        if (hu_json_parse(alloc, jsonl + start, line_len, &obj) != HU_OK || !obj)
            continue; /* malformed line — skip, not fatal */
        if (obj->type != HU_JSON_OBJECT) {
            hu_json_free(alloc, obj);
            continue;
        }

        const char *reply = hu_json_get_string(obj, "reply");
        if (reply) {
            total++;
            size_t rlen = strlen(reply);

            const char *channel = hu_json_get_string(obj, "channel");
            hu_shape_channel_t chan =
                hu_shape_channel_from_string(channel, channel ? strlen(channel) : 0);

            hu_shape_result_t sr;
            memset(&sr, 0, sizeof(sr));
            if (hu_shape_classify(reply, rlen, chan, &sr) == HU_OK)
                acc_add(&ai, sr.score);

            hu_json_value_t *tr = hu_json_object_get(obj, "target_register");
            if (tr && tr->type == HU_JSON_OBJECT) {
                double tf = hu_json_get_number(tr, "formality", -1.0);
                double tw = hu_json_get_number(tr, "warmth", -1.0);
                if (tf >= 0.0 && tw >= 0.0)
                    acc_add(&rel, hu_relationship_axis_score(reply, rlen, tf, tw));
            }

            if (fidelity_possible) {
                const char *responses[1] = {reply};
                size_t lens[1] = {rlen};
                hu_persona_fidelity_score_t fs;
                memset(&fs, 0, sizeof(fs));
                if (hu_persona_fidelity_score_l1(target_style, responses, lens, 1, NULL, 0, NULL, 0,
                                                 NULL, 0, &fs) == HU_OK &&
                    fs.turns_scored > 0)
                    acc_add(&fid, (double)fs.composite);
            }
        }
        hu_json_free(alloc, obj);
    }

    bool fid_available = fidelity_possible && fid.n > 0;

    char tmp[768];
    int m =
        snprintf(tmp, sizeof(tmp),
                 "{\"n\":%zu,\"axes\":{"
                 "\"anti_ai\":{\"mean\":%.6f,\"stderr\":%.6f,\"n\":%zu},"
                 "\"relationship\":{\"mean\":%.6f,\"stderr\":%.6f,\"n\":%zu},"
                 "\"fidelity\":{\"mean\":%.6f,\"stderr\":%.6f,\"n\":%zu,\"available\":%s}"
                 "}}",
                 total, acc_mean(&ai), acc_stderr(&ai), ai.n, acc_mean(&rel), acc_stderr(&rel),
                 rel.n, acc_mean(&fid), acc_stderr(&fid), fid.n, fid_available ? "true" : "false");
    if (m < 0 || (size_t)m >= sizeof(tmp))
        return HU_ERR_INTERNAL;

    char *out = (char *)alloc->alloc(alloc->ctx, (size_t)m + 1);
    if (!out)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(out, tmp, (size_t)m + 1);
    *out_json = out;
    if (out_json_len)
        *out_json_len = (size_t)m;
    return HU_OK;
}

/* ── CLI wrapper ───────────────────────────────────────────────────────── */

static bool is_flag(const char *a, const char *want) {
    return a && want && strcmp(a, want) == 0;
}

static char *read_all(hu_allocator_t *alloc, const char *path, size_t *out_len) {
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f)
        return NULL;
    size_t cap = 4096, len = 0;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        if (path)
            fclose(f);
        return NULL;
    }
    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)alloc->realloc(alloc->ctx, buf, cap, ncap);
            if (!nb) {
                alloc->free(alloc->ctx, buf, cap);
                if (path)
                    fclose(f);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }
        size_t rd = fread(buf + len, 1, 4096, f);
        len += rd;
        if (rd < 4096)
            break;
    }
    if (path)
        fclose(f);
    buf[len < cap ? len : cap - 1] = '\0';
    if (out_len)
        *out_len = len;
    return buf;
}

static void print_score_help(void) {
    printf("human eval score -- per-axis humanness scores for a JSONL of replies\n\n");
    printf("Reads JSONL (one object/line) with fields: reply (required), channel,\n");
    printf("contact_id, target_register:{formality,warmth}. Emits axis JSON.\n\n");
    printf("Options:\n");
    printf("  --in <path>          Input JSONL (default: stdin)\n");
    printf("  --out-json <path>    Output JSON (default: stdout)\n");
    printf("  --target-style <json>  Inline communication-style fingerprint for the\n");
    printf("                         fidelity (A1) axis. Omit to skip fidelity.\n");
    printf("  --help               Show this help\n");
}

hu_error_t hu_eval_cli_score(hu_allocator_t *alloc, int argc, char **argv) {
    (void)argc;
    const char *in_path = NULL, *out_path = NULL, *target_json = NULL;
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i], "--help") || is_flag(argv[i], "-h")) {
            print_score_help();
            return HU_OK;
        }
        if (is_flag(argv[i], "--in") && i + 1 < argc) {
            in_path = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--out-json") && i + 1 < argc) {
            out_path = argv[++i];
            continue;
        }
        if (is_flag(argv[i], "--target-style") && i + 1 < argc) {
            target_json = argv[++i];
            continue;
        }
    }

    hu_allocator_t sys = hu_system_allocator();
    if (!alloc)
        alloc = &sys;

    /* Optional A1 target style. */
    hu_communication_style_t style;
    memset(&style, 0, sizeof(style));
    const hu_communication_style_t *style_ptr = NULL;
    if (target_json) {
        hu_json_value_t *tv = NULL;
        if (hu_json_parse(alloc, target_json, strlen(target_json), &tv) == HU_OK && tv &&
            tv->type == HU_JSON_OBJECT) {
            style.formality = (float)hu_json_get_number(tv, "formality", 0.5);
            style.verbosity = (float)hu_json_get_number(tv, "verbosity", 0.5);
            style.emoji_frequency = (float)hu_json_get_number(tv, "emoji_frequency", 0.0);
            style.humor_receptivity = (float)hu_json_get_number(tv, "humor_receptivity", 0.5);
            style.lowercase_ratio = (float)hu_json_get_number(tv, "lowercase_ratio", 0.0);
            style.abbreviation_ratio = (float)hu_json_get_number(tv, "abbreviation_ratio", 0.0);
            style.avg_message_length = (uint32_t)hu_json_get_number(tv, "avg_message_length", 60.0);
            style.sample_count = (uint32_t)hu_json_get_number(tv, "sample_count", 1.0);
            if (style.sample_count == 0)
                style.sample_count = 1;
            style_ptr = &style;
        }
        if (tv)
            hu_json_free(alloc, tv);
    }

    size_t in_len = 0;
    char *input = read_all(alloc, in_path, &in_len);
    if (!input) {
        fprintf(stderr, "[eval score] failed to read input %s\n", in_path ? in_path : "(stdin)");
        return HU_ERR_IO;
    }

    char *out_json = NULL;
    size_t out_len = 0;
    hu_error_t e = hu_eval_score_jsonl(alloc, input, in_len, style_ptr, &out_json, &out_len);
    alloc->free(alloc->ctx, input, 0);
    if (e != HU_OK) {
        fprintf(stderr, "[eval score] scoring failed: %s\n", hu_error_string(e));
        return e;
    }

    if (out_path) {
        FILE *of = fopen(out_path, "wb");
        if (!of) {
            alloc->free(alloc->ctx, out_json, 0);
            fprintf(stderr, "[eval score] cannot write %s\n", out_path);
            return HU_ERR_IO;
        }
        fwrite(out_json, 1, out_len, of);
        fputc('\n', of);
        fclose(of);
        printf("human eval score -> %s\n", out_path);
    } else {
        printf("%s\n", out_json);
    }
    alloc->free(alloc->ctx, out_json, 0);
    return HU_OK;
}
