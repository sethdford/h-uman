#include "human/memory/personal_model.h"
#include "human/platform.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

void hu_personal_model_init(hu_personal_model_t *model) {
    if (!model)
        return;
    memset(model, 0, sizeof(*model));
    model->version = 1U;
    model->created_at = 0;
}

static size_t append_fmt(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    if (*off >= cap)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap > *off ? cap - *off : 0, fmt, ap);
    va_end(ap);
    if (w < 0)
        return 0;
    if ((size_t)w >= cap - *off) {
        *off = cap > 0 ? cap - 1 : 0;
        if (cap > 0)
            buf[*off] = '\0';
        return (size_t)w;
    }
    *off += (size_t)w;
    return (size_t)w;
}

static const char *formality_desc(float f) {
    if (f < 0.33f)
        return "casual";
    if (f < 0.66f)
        return "balanced";
    return "formal";
}

static const char *verbosity_desc(float v) {
    if (v < 0.33f)
        return "terse";
    if (v < 0.66f)
        return "moderate";
    return "verbose";
}

static void sort_topic_order(const hu_personal_model_t *model, size_t *order) {
    for (size_t i = 0; i < model->topic_count; i++)
        order[i] = i;
    for (size_t i = 0; i + 1 < model->topic_count; i++) {
        for (size_t j = i + 1; j < model->topic_count; j++) {
            float si = model->topics[order[i]].interest_score;
            float sj = model->topics[order[j]].interest_score;
            if (sj > si) {
                size_t t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
}

bool hu_personal_model_has_content(const hu_personal_model_t *model) {
    if (!model)
        return false;
    if (model->fact_count > 0 || model->topic_count > 0 || model->goal_count > 0)
        return true;
    if (model->style.sample_count > 0U)
        return true;
    if (model->core.user_name[0] != '\0' || model->core.user_bio[0] != '\0' ||
        model->core.user_preferences[0] != '\0')
        return true;
    return false;
}

size_t hu_personal_model_build_prompt(const hu_personal_model_t *model, char *buf, size_t cap) {
    if (!buf || cap == 0)
        return 0;
    buf[0] = '\0';
    if (!model)
        return 0;

    size_t n = 0;
    append_fmt(buf, cap, &n, "[Personal Context]\n");
    bool detail = false;

    if (model->core.user_name[0] != '\0') {
        append_fmt(buf, cap, &n, "Name: %s\n", model->core.user_name);
        detail = true;
    }
    if (model->core.user_bio[0] != '\0') {
        append_fmt(buf, cap, &n, "Bio: %s\n", model->core.user_bio);
        detail = true;
    }
    if (model->core.user_preferences[0] != '\0') {
        append_fmt(buf, cap, &n, "Preferences: %s\n", model->core.user_preferences);
        detail = true;
    }

    if (model->style.sample_count > 0U) {
        append_fmt(buf, cap, &n, "Communication style: %s, %s, avg %u chars\n",
                   formality_desc(model->style.formality), verbosity_desc(model->style.verbosity),
                   (unsigned)model->style.avg_message_length);
        detail = true;
    }

    bool any_goal = false;
    for (size_t g = 0; g < model->goal_count; g++) {
        if (model->goals[g].active && model->goals[g].description[0] != '\0') {
            if (!any_goal) {
                append_fmt(buf, cap, &n, "Active goals: ");
                any_goal = true;
            } else {
                append_fmt(buf, cap, &n, ", ");
            }
            append_fmt(buf, cap, &n, "%s", model->goals[g].description);
        }
    }
    if (any_goal) {
        append_fmt(buf, cap, &n, "\n");
        detail = true;
    }

    if (model->fact_count > 0) {
        append_fmt(buf, cap, &n, "Key facts: ");
        size_t max_facts = model->fact_count > 8U ? 8U : model->fact_count;
        for (size_t i = 0; i < max_facts; i++) {
            const hu_heuristic_fact_t *f = &model->facts[i];
            if (i > 0)
                append_fmt(buf, cap, &n, ", ");
            append_fmt(buf, cap, &n, "%s %s %s", f->subject, f->predicate, f->object);
        }
        append_fmt(buf, cap, &n, "\n");
        detail = true;
    }

    if (model->topic_count > 0) {
        size_t order[HU_PM_MAX_TOPICS];
        sort_topic_order(model, order);
        append_fmt(buf, cap, &n, "Top interests: ");
        size_t max_t = model->topic_count > 6U ? 6U : model->topic_count;
        for (size_t i = 0; i < max_t; i++) {
            const hu_personal_topic_t *t = &model->topics[order[i]];
            if (i > 0)
                append_fmt(buf, cap, &n, ", ");
            append_fmt(buf, cap, &n, "%s (%.2f)", t->name, (double)t->interest_score);
        }
        append_fmt(buf, cap, &n, "\n");
        detail = true;
    }

    if (!detail)
        append_fmt(buf, cap, &n, "(No detailed personal data yet.)\n");

    return n;
}

static bool ci_haystack_contains(const char *hay, const char *needle, size_t needle_len) {
    if (!hay || !needle || needle_len == 0)
        return false;
    for (const char *p = hay; *p != '\0'; p++) {
        size_t k;
        for (k = 0; k < needle_len && p[k] != '\0'; k++) {
            if (tolower((unsigned char)p[k]) != tolower((unsigned char)needle[k]))
                break;
        }
        if (k == needle_len)
            return true;
    }
    return false;
}

static void bump_topic(hu_personal_model_t *model, const char *name, int64_t ts) {
    if (!name || name[0] == '\0')
        return;

    for (size_t i = 0; i < model->topic_count; i++) {
        if (strcasecmp(model->topics[i].name, name) == 0) {
            model->topics[i].mention_count++;
            model->topics[i].last_mentioned = ts;
            if (model->topics[i].interest_score < 1.0f - 0.05f)
                model->topics[i].interest_score += 0.05f;
            else
                model->topics[i].interest_score = 1.0f;
            return;
        }
    }
    if (model->topic_count >= HU_PM_MAX_TOPICS)
        return;
    hu_personal_topic_t *t = &model->topics[model->topic_count++];
    memset(t, 0, sizeof(*t));
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->interest_score = 0.3f;
    t->mention_count = 1U;
    t->last_mentioned = ts;
}

static bool fact_key_dup(const hu_heuristic_fact_t *a, const hu_heuristic_fact_t *b) {
    return strcmp(a->subject, b->subject) == 0 && strcmp(a->predicate, b->predicate) == 0;
}

static void update_style_from_message(hu_communication_style_t *style, const char *message,
                                      size_t message_len) {
    if (!style || !message || message_len == 0)
        return;

    uint32_t prev_n = style->sample_count;
    style->sample_count++;

    uint32_t len = (uint32_t)message_len;
    if (prev_n == 0U) {
        style->avg_message_length = len;
    } else {
        style->avg_message_length =
            (uint32_t)(((uint64_t)style->avg_message_length * (uint64_t)prev_n + len) /
                       (uint64_t)style->sample_count);
    }

    /* Verbosity: map length to 0..1 (500+ chars treated as fully verbose). */
    float verb = fminf(1.0f, (float)len / 500.0f);
    style->verbosity = style->verbosity * 0.85f + verb * 0.15f;

    /* Rough emoji proxy: UTF-8 leading bytes 0xF0.. often start emoji sequences. */
    size_t emoji_hits = 0;
    for (size_t i = 0; i < message_len; i++) {
        unsigned char c = (unsigned char)message[i];
        if (c >= 240U)
            emoji_hits++;
    }
    float emoji_slice =
        message_len > 0 ? fminf(1.0f, (float)emoji_hits * 4.0f / (float)message_len) : 0.0f;
    style->emoji_frequency = style->emoji_frequency * 0.9f + emoji_slice * 0.1f;

    /* Formality cues */
    float form_adj = 0.0f;
    if (ci_haystack_contains(message, "please", 6) ||
        ci_haystack_contains(message, "thank you", 9) ||
        ci_haystack_contains(message, "would you", 9))
        form_adj += 0.08f;
    if (ci_haystack_contains(message, "lol", 3) || ci_haystack_contains(message, "haha", 4) ||
        ci_haystack_contains(message, "btw", 3))
        form_adj -= 0.06f;
    style->formality = fmaxf(0.0f, fminf(1.0f, style->formality * 0.9f + (0.5f + form_adj) * 0.1f));

    if (ci_haystack_contains(message, "lol", 3) || ci_haystack_contains(message, "haha", 4))
        style->humor_receptivity = fminf(1.0f, style->humor_receptivity * 0.9f + 0.1f * 0.8f);
}

static void bump_temporal(hu_personal_model_t *model, int64_t timestamp) {
    if (timestamp <= 0)
        return;
    time_t t = (time_t)timestamp;
    struct tm tm_buf;
    struct tm *tm = hu_platform_localtime_r(&t, &tm_buf);
    if (!tm)
        return;
    int h = tm->tm_hour;
    int d = tm->tm_wday;
    if (h >= 0 && h < 24 && model->active_hours[h] < 255)
        model->active_hours[h]++;
    if (d >= 0 && d < 7 && model->active_days[d] < 255)
        model->active_days[d]++;
}

hu_error_t hu_personal_model_ingest(hu_personal_model_t *model, const char *message,
                                    size_t message_len, bool from_user, int64_t timestamp) {
    if (!model)
        return HU_ERR_INVALID_ARGUMENT;
    if (message_len > 0 && !message)
        return HU_ERR_INVALID_ARGUMENT;

    model->interaction_count++;

    if (timestamp > 0) {
        if (model->created_at == 0)
            model->created_at = timestamp;
        model->updated_at = timestamp;
    }

    if (!from_user)
        return HU_OK;

    if (message_len == 0)
        return HU_OK;

    bump_temporal(model, timestamp);
    update_style_from_message(&model->style, message, message_len);

    hu_fact_extract_result_t extracted;
    hu_error_t err = hu_fact_extract(message, message_len, &extracted);
    if (err != HU_OK)
        return err;

    return hu_personal_model_merge_facts(model, &extracted);
}

hu_error_t hu_personal_model_merge_facts(hu_personal_model_t *model,
                                         const hu_fact_extract_result_t *facts) {
    if (!model || !facts)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t ts = model->updated_at;

    for (size_t i = 0; i < facts->fact_count; i++) {
        const hu_heuristic_fact_t *nf = &facts->facts[i];
        bool dup = false;
        for (size_t j = 0; j < model->fact_count; j++) {
            if (fact_key_dup(&model->facts[j], nf)) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (model->fact_count >= HU_PM_MAX_FACTS)
            break;
        model->facts[model->fact_count] = *nf;
        model->fact_count++;
        bump_topic(model, nf->object, ts);
    }
    return HU_OK;
}

const hu_heuristic_fact_t *hu_personal_model_query_preference(const hu_personal_model_t *model,
                                                              const char *topic, size_t topic_len) {
    if (!model || !topic || topic_len == 0)
        return NULL;

    for (size_t i = 0; i < model->fact_count; i++) {
        const hu_heuristic_fact_t *f = &model->facts[i];
        if (ci_haystack_contains(f->object, topic, topic_len) ||
            ci_haystack_contains(f->predicate, topic, topic_len))
            return f;
    }
    return NULL;
}

/* ── M2 P1: Persistence ─────────────────────────────────────────────────
 *
 * Binary format:
 *   magic   uint32_t  "HUPM" (0x4D505548 LE)
 *   version uint32_t  matches `model->version` at write time
 *   reserved uint64_t zero, room for future framing without breaking
 *   body    hu_personal_model_t  raw struct bytes (POD)
 *
 * Choice of binary over JSON: ~6KB on disk vs ~25KB, zero dependencies,
 * and the struct is POD with fixed-size buffers so memcpy is safe.
 * The version field forces a fresh state on schema bumps; this is
 * intentional because old facts/topics may be incompatible with new
 * code (e.g. a tightened type discriminator).
 *
 * Path is created if needed; intermediate directories are NOT created
 * by this code — callers should pre-create `~/.human/personal_model/`
 * (the daemon's onboard wizard does this for the user). */

#define HU_PM_MAGIC    0x4D505548u   /* "HUPM" little-endian */
#define HU_PM_VERSION  1u

typedef struct hu_pm_header {
    uint32_t magic;
    uint32_t version;
    uint64_t reserved;  /* always 0; reserved for future framing */
} hu_pm_header_t;

/* Best-effort `mkdir -p` for the parent of `path`.
 *
 * Walks the path string from the front, calling `mkdir(0700)` at every '/'
 * boundary so the personal model can land cleanly on first run even when
 * `~/.human/` does not exist yet. 0700 mode is deliberate — the personal
 * model is sensitive user data; only the owner should read it. We swallow
 * EEXIST and ignore failures on intermediate components so a pre-existing
 * directory tree (the common case) is a no-op. The final mkdir result is
 * also ignored — the subsequent `fopen` will surface any real failure as
 * HU_ERR_IO. */
static void hu_pm_ensure_parent_dir(const char *path) {
    if (!path || !*path) {
        return;
    }
    const char *last_slash = strrchr(path, '/');
    if (!last_slash || last_slash == path) {
        return;
    }
    size_t parent_len = (size_t)(last_slash - path);
    char buf[1024];
    if (parent_len + 1 >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, parent_len);
    buf[parent_len] = '\0';
    /* Walk from the start, creating each component. Skip leading '/'. */
    for (size_t i = 1; i < parent_len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0700);
            buf[i] = '/';
        }
    }
    (void)mkdir(buf, 0700);
}

const char *hu_personal_model_resolve_default_path(char *buf, size_t cap) {
    if (!buf || cap == 0) {
        return NULL;
    }
    buf[0] = '\0';
    const char *override = getenv("HUMAN_PERSONAL_MODEL_PATH");
    if (override && override[0]) {
        size_t len = strlen(override);
        if (len + 1 > cap) {
            return NULL;
        }
        memcpy(buf, override, len + 1);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        return NULL;
    }
    int n = snprintf(buf, cap, "%s/.human/personal_model.bin", home);
    if (n <= 0 || (size_t)n >= cap) {
        return NULL;
    }
    return buf;
}

hu_error_t hu_personal_model_save(const hu_personal_model_t *model, const char *path) {
    if (!model || !path || !*path) return HU_ERR_INVALID_ARGUMENT;
    hu_pm_ensure_parent_dir(path);
    FILE *fp = fopen(path, "wb");
    if (!fp) return HU_ERR_IO;
    hu_pm_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = HU_PM_MAGIC;
    hdr.version = HU_PM_VERSION;
    hdr.reserved = 0;
    if (fwrite(&hdr, sizeof(hdr), 1, fp) != 1 ||
        fwrite(model, sizeof(*model), 1, fp) != 1) {
        fclose(fp);
        return HU_ERR_IO;
    }
    if (fflush(fp) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    fclose(fp);
    return HU_OK;
}

hu_error_t hu_personal_model_load(hu_personal_model_t *out, const char *path) {
    if (!out || !path || !*path) return HU_ERR_INVALID_ARGUMENT;
    hu_personal_model_init(out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return HU_ERR_NOT_FOUND;
    hu_pm_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        fclose(fp);
        return HU_ERR_PARSE;
    }
    if (hdr.magic != HU_PM_MAGIC || hdr.version != HU_PM_VERSION) {
        fclose(fp);
        /* Out is already re-initialized to defaults so the caller can
         * keep walking on a schema mismatch. */
        return HU_ERR_PARSE;
    }
    hu_personal_model_t tmp;
    if (fread(&tmp, sizeof(tmp), 1, fp) != 1) {
        fclose(fp);
        hu_personal_model_init(out);
        return HU_ERR_PARSE;
    }
    fclose(fp);
    /* Defensive: clamp counts that could overflow on a corrupted file. */
    if (tmp.fact_count > HU_PM_MAX_FACTS) tmp.fact_count = HU_PM_MAX_FACTS;
    if (tmp.topic_count > HU_PM_MAX_TOPICS) tmp.topic_count = HU_PM_MAX_TOPICS;
    if (tmp.goal_count > HU_PM_MAX_GOALS) tmp.goal_count = HU_PM_MAX_GOALS;
    *out = tmp;
    return HU_OK;
}
