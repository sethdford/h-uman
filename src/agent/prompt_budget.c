/* src/agent/prompt_budget.c
 *
 * Per-field byte accounting + DEAD-field detection for the system prompt
 * builder. See docs/plans/2026-05-25-director-compression/.
 *
 * Pure functions over a file-scope struct — testable in isolation. Wrap-
 * up call sites in src/agent/prompt.c populate the stats array; this
 * module just accumulates and decides. */

#include "human/agent/prompt_budget.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Per-field running totals. File-scope struct (NOT anonymous) so pointers
 * and assignments compose cleanly across functions — the worker's earlier
 * attempt at this used three differently-scoped anonymous structs and
 * got -Wincompatible-pointer-types from clang. */
typedef struct prompt_field_accumulator {
    const char *name;           /* borrowed from stat->name (static string) */
    uint64_t total_bytes;       /* sum of all observed bytes for this field */
    uint64_t observation_count; /* count of observations including zero-byte */
    uint64_t non_empty_count;   /* observations where bytes_contributed > 0 */
} prompt_field_accumulator_t;

struct hu_prompt_budget {
    hu_allocator_t *alloc;
    prompt_field_accumulator_t fields[HU_PROMPT_FIELD_COUNT];
    size_t observation_count; /* total turns observed (across all fields) */
};

/* Stable display names. Indexed by hu_prompt_field_t. The trailing
 * sentinel keeps array bounds explicit so adding a new field forces an
 * update of HU_PROMPT_FIELD_COUNT (build break catches the omission). */
static const char *const s_field_names[HU_PROMPT_FIELD_COUNT] = {
    [HU_PROMPT_FIELD_MEMORY_CONTEXT] = "memory_context",
    [HU_PROMPT_FIELD_PERSONAL_MODEL_CONTEXT] = "personal_model_context",
    [HU_PROMPT_FIELD_MOMENT_CONTEXT] = "moment_context",
    [HU_PROMPT_FIELD_SELF_EXEMPLARS_CONTEXT] = "self_exemplars_context",
    [HU_PROMPT_FIELD_WORLD_MODEL_CONTEXT] = "world_model_context",
    [HU_PROMPT_FIELD_RELATIONAL_EPISODE_CONTEXT] = "relational_episode_context",
    [HU_PROMPT_FIELD_INSTRUCTION_CONTEXT] = "instruction_context",
    [HU_PROMPT_FIELD_STM_CONTEXT] = "stm_context",
    [HU_PROMPT_FIELD_CONTACT_CONTEXT] = "contact_context",
    [HU_PROMPT_FIELD_CONVERSATION_CONTEXT] = "conversation_context",
    [HU_PROMPT_FIELD_AWARENESS_CONTEXT] = "awareness_context",
    [HU_PROMPT_FIELD_OUTCOME_CONTEXT] = "outcome_context",
    [HU_PROMPT_FIELD_INTELLIGENCE_CONTEXT] = "intelligence_context",
    [HU_PROMPT_FIELD_SKILLS_CONTEXT] = "skills_context",
    [HU_PROMPT_FIELD_EMOTIONAL_CONTEXT] = "emotional_context",
    [HU_PROMPT_FIELD_COMMITMENT_CONTEXT] = "commitment_context",
    [HU_PROMPT_FIELD_PATTERN_CONTEXT] = "pattern_context",
    [HU_PROMPT_FIELD_ADAPTIVE_PERSONA_CONTEXT] = "adaptive_persona_context",
    [HU_PROMPT_FIELD_PROACTIVE_CONTEXT] = "proactive_context",
    [HU_PROMPT_FIELD_SUPERHUMAN_CONTEXT] = "superhuman_context",
    [HU_PROMPT_FIELD_PERSONA_PROMPT] = "persona_prompt",
    [HU_PROMPT_FIELD_CUSTOM_INSTRUCTIONS] = "custom_instructions",
    [HU_PROMPT_FIELD_PREFERENCES] = "preferences",
    [HU_PROMPT_FIELD_TONE_HINT] = "tone_hint",
    [HU_PROMPT_FIELD_SOMATIC_CONTEXT] = "somatic_context",
    [HU_PROMPT_FIELD_RUPTURE_CONTEXT] = "rupture_context",
    [HU_PROMPT_FIELD_VOICE_MATURITY_DIRECTIVE] = "voice_maturity_directive",
    [HU_PROMPT_FIELD_GRAPH_CONTEXT] = "graph_context",
};

const char *hu_prompt_field_name(hu_prompt_field_t field) {
    if ((int)field < 0 || (int)field >= HU_PROMPT_FIELD_COUNT)
        return NULL;
    return s_field_names[field];
}

hu_error_t hu_prompt_budget_init(hu_allocator_t *alloc, hu_prompt_budget_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    hu_prompt_budget_t *b = (hu_prompt_budget_t *)alloc->alloc(alloc->ctx, sizeof(*b));
    if (!b)
        return HU_ERR_OUT_OF_MEMORY;
    memset(b, 0, sizeof(*b));
    b->alloc = alloc;
    /* Pre-populate field name pointers from the static table so even
     * a budget that has never observed a turn can still report names. */
    for (size_t i = 0; i < HU_PROMPT_FIELD_COUNT; i++) {
        b->fields[i].name = s_field_names[i];
    }
    *out = b;
    return HU_OK;
}

void hu_prompt_budget_free(hu_prompt_budget_t *b) {
    if (!b)
        return;
    hu_allocator_t *alloc = b->alloc;
    if (alloc)
        alloc->free(alloc->ctx, b, sizeof(*b));
}

void hu_prompt_budget_observe(hu_prompt_budget_t *b, const hu_prompt_field_stat_t *stats,
                              size_t count) {
    if (!b || !stats || count == 0)
        return;
    size_t n = count < HU_PROMPT_FIELD_COUNT ? count : HU_PROMPT_FIELD_COUNT;
    for (size_t i = 0; i < n; i++) {
        prompt_field_accumulator_t *f = &b->fields[i];
        /* Adopt the name pointer if the stat carries one (in case the
         * caller used a different static string than our default). */
        if (stats[i].name)
            f->name = stats[i].name;
        f->observation_count++;
        f->total_bytes += (uint64_t)stats[i].bytes_contributed;
        if (stats[i].bytes_contributed > 0)
            f->non_empty_count++;
    }
    b->observation_count++;
}

size_t hu_prompt_budget_observation_count(const hu_prompt_budget_t *b) {
    if (!b)
        return 0;
    return b->observation_count;
}

bool hu_prompt_budget_field_is_dead(const hu_prompt_budget_t *b, hu_prompt_field_t field,
                                    size_t min_bytes_threshold, size_t min_sample_count) {
    if (!b)
        return false;
    if ((int)field < 0 || (int)field >= HU_PROMPT_FIELD_COUNT)
        return false;
    const prompt_field_accumulator_t *f = &b->fields[field];
    /* Need enough observations to make a confident claim. Until then
     * the field cannot be DEAD — telemetric value: a brand-new field
     * is "unknown," not "dead." */
    if (f->observation_count < (uint64_t)min_sample_count)
        return false;
    uint64_t mean = f->total_bytes / f->observation_count;
    return mean < (uint64_t)min_bytes_threshold;
}

size_t hu_prompt_budget_snapshot(const hu_prompt_budget_t *b, hu_prompt_field_stat_t *out_array,
                                 size_t array_cap) {
    if (!b || !out_array || array_cap == 0)
        return 0;
    size_t n = array_cap < HU_PROMPT_FIELD_COUNT ? array_cap : HU_PROMPT_FIELD_COUNT;
    for (size_t i = 0; i < n; i++) {
        const prompt_field_accumulator_t *f = &b->fields[i];
        out_array[i].name = f->name;
        /* Snapshot reports the MEAN bytes per observation — that's
         * what dead-field detection compares against the threshold. */
        out_array[i].bytes_contributed =
            f->observation_count > 0 ? (size_t)(f->total_bytes / f->observation_count) : 0;
    }
    return n;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Snapshot persistence — JSON file at ~/.human/prompt_budget.snapshot.json.
 *
 * Schema:
 *   {
 *     "schema": "prompt_budget_snapshot_v1",
 *     "observation_count": <int>,
 *     "field_count": <int>,
 *     "fields": [
 *       {"name":"...","mean_bytes":N,"samples":N,"non_empty_count":N},
 *       ...
 *     ]
 *   }
 *
 * Write discipline: tmp + fwrite + fflush + fsync + rename (atomic on
 * POSIX). Mirrors hu_personal_model_save's pattern — required so a
 * concurrent reader (the doctor) cannot land mid-flush and see torn
 * JSON. The verifier_metrics writer uses non-atomic fopen("w") and is
 * a documented weakness; we explicitly do NOT propagate that here.
 * ────────────────────────────────────────────────────────────────────── */

#ifdef HU_IS_TEST
static char s_snapshot_path_override[1024] = {0};
#endif

void hu_prompt_budget_snapshot_set_path_for_test(const char *path) {
#ifdef HU_IS_TEST
    if (path == NULL) {
        s_snapshot_path_override[0] = '\0';
        return;
    }
    snprintf(s_snapshot_path_override, sizeof(s_snapshot_path_override), "%s", path);
#else
    (void)path;
#endif
}

size_t hu_prompt_budget_snapshot_path(char *out_buf, size_t out_cap) {
    if (!out_buf || out_cap == 0)
        return 0;
#ifdef HU_IS_TEST
    if (s_snapshot_path_override[0]) {
        int n = snprintf(out_buf, out_cap, "%s", s_snapshot_path_override);
        return (n > 0 && (size_t)n < out_cap) ? (size_t)n : 0;
    }
#endif
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return 0;
    int n = snprintf(out_buf, out_cap, "%s/.human/prompt_budget.snapshot.json", home);
    return (n > 0 && (size_t)n < out_cap) ? (size_t)n : 0;
}

/* Best-effort mkdir of $HOME and $HOME/.human. Idempotent; failures are
 * deliberately silent — fopen below will surface a real error if the
 * directory is genuinely missing. */
static void pb_ensure_parent_dir(const char *path) {
    if (!path)
        return;
    const char *last = strrchr(path, '/');
    if (!last || last == path)
        return;
    size_t parent_len = (size_t)(last - path);
    if (parent_len >= 1024)
        return;
    char parent[1024];
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';
    /* mkdir grandparent first so a fresh tmp HOME works in tests. */
    const char *prev = strrchr(parent, '/');
    if (prev && prev != parent) {
        char gp[1024];
        size_t gp_len = (size_t)(prev - parent);
        memcpy(gp, parent, gp_len);
        gp[gp_len] = '\0';
        (void)mkdir(gp, 0700);
    }
    (void)mkdir(parent, 0700);
}

hu_error_t hu_prompt_budget_save_snapshot(const hu_prompt_budget_t *b) {
    if (!b)
        return HU_ERR_INVALID_ARGUMENT;
    char path[1024];
    if (hu_prompt_budget_snapshot_path(path, sizeof(path)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    pb_ensure_parent_dir(path);

    char tmp[1100];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *fp = fopen(tmp, "w");
    if (!fp)
        return HU_ERR_IO;

    if (fprintf(fp,
                "{\n"
                "  \"schema\": \"prompt_budget_snapshot_v1\",\n"
                "  \"observation_count\": %llu,\n"
                "  \"field_count\": %d,\n"
                "  \"fields\": [\n",
                (unsigned long long)b->observation_count, (int)HU_PROMPT_FIELD_COUNT) < 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    for (size_t i = 0; i < HU_PROMPT_FIELD_COUNT; i++) {
        const prompt_field_accumulator_t *f = &b->fields[i];
        const char *name = f->name ? f->name : (s_field_names[i] ? s_field_names[i] : "");
        uint64_t mean = f->observation_count > 0 ? (f->total_bytes / f->observation_count) : 0;
        if (fprintf(fp,
                    "    {\"name\":\"%s\",\"mean_bytes\":%llu,\"samples\":%llu,"
                    "\"non_empty_count\":%llu}%s\n",
                    name, (unsigned long long)mean, (unsigned long long)f->observation_count,
                    (unsigned long long)f->non_empty_count,
                    (i + 1 == HU_PROMPT_FIELD_COUNT) ? "" : ",") < 0) {
            fclose(fp);
            (void)unlink(tmp);
            return HU_ERR_IO;
        }
    }
    if (fprintf(fp, "  ]\n}\n") < 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }

    if (fflush(fp) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    int fd = fileno(fp);
    if (fd >= 0 && fsync(fd) != 0) {
        fclose(fp);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (fclose(fp) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    return HU_OK;
}

/* ── Minimal JSON parser tailored to the snapshot schema ─────────────── */

static const char *pb_skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Returns the byte AFTER the matched `"key"`, or NULL. */
static const char *pb_find_key(const char *p, const char *end, const char *key) {
    size_t klen = strlen(key);
    while (p < end) {
        const char *q = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!q)
            return NULL;
        const char *r = q + 1;
        if (r + klen <= end && memcmp(r, key, klen) == 0 && r[klen] == '"')
            return r + klen + 1;
        p = q + 1;
    }
    return NULL;
}

static int pb_parse_u64_after_colon(const char *start, const char *end, uint64_t *out) {
    const char *p = pb_skip_ws(start, end);
    if (p >= end || *p != ':')
        return -1;
    p = pb_skip_ws(p + 1, end);
    uint64_t v = 0;
    int seen = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        v = v * 10 + (uint64_t)(*p - '0');
        p++;
        seen = 1;
    }
    if (!seen)
        return -1;
    *out = v;
    return 0;
}

hu_error_t hu_prompt_budget_load_snapshot(hu_allocator_t *alloc,
                                          hu_prompt_budget_snapshot_load_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;

    char path[1024];
    if (hu_prompt_budget_snapshot_path(path, sizeof(path)) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return HU_ERR_NOT_FOUND;
        return HU_ERR_IO;
    }
    out->mtime_unix = (int64_t)st.st_mtime;

    FILE *fp = fopen(path, "r");
    if (!fp)
        return HU_ERR_IO;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    long sz = ftell(fp);
    /* 64 KB cap — snapshot is bounded at ~3 KB for 27 fields × ~110 bytes;
     * 64 KB allows generous growth without uncontrolled allocations. */
    if (sz < 0 || sz > (long)(64 * 1024)) {
        fclose(fp);
        return HU_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return HU_ERR_IO;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        fclose(fp);
        return HU_ERR_IO;
    }
    buf[sz] = '\0';
    fclose(fp);

    const char *end = buf + sz;

    const char *p = pb_find_key(buf, end, "observation_count");
    if (!p || pb_parse_u64_after_colon(p, end, &out->observation_count) != 0) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_PARSE;
    }
    uint64_t declared = 0;
    p = pb_find_key(buf, end, "field_count");
    if (!p || pb_parse_u64_after_colon(p, end, &declared) != 0) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_PARSE;
    }
    size_t cap =
        declared < HU_PROMPT_FIELD_COUNT ? (size_t)declared : (size_t)HU_PROMPT_FIELD_COUNT;
    if (cap == 0) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_OK;
    }
    hu_prompt_budget_field_stat_ext_t *fields = (hu_prompt_budget_field_stat_ext_t *)alloc->alloc(
        alloc->ctx, cap * sizeof(hu_prompt_budget_field_stat_ext_t));
    if (!fields) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(fields, 0, cap * sizeof(hu_prompt_budget_field_stat_ext_t));

    /* Walk objects in the "fields" array. Each object begins with
     * "name":"<value>" — we use that key as a sync point and pull the
     * three numeric fields that follow. Schema-drift fields are skipped. */
    size_t parsed = 0;
    const char *scan = buf;
    while (parsed < cap) {
        const char *name_key = pb_find_key(scan, end, "name");
        if (!name_key)
            break;
        const char *nq = pb_skip_ws(name_key, end);
        if (nq >= end || *nq != ':')
            break;
        nq = pb_skip_ws(nq + 1, end);
        if (nq >= end || *nq != '"')
            break;
        const char *name_start = nq + 1;
        const char *name_end = (const char *)memchr(name_start, '"', (size_t)(end - name_start));
        if (!name_end)
            break;
        size_t name_len = (size_t)(name_end - name_start);

        /* Match against the static name table so we store the canonical
         * pointer, not one into the freed buf. */
        const char *canonical = NULL;
        for (size_t i = 0; i < HU_PROMPT_FIELD_COUNT; i++) {
            if (s_field_names[i] && strlen(s_field_names[i]) == name_len &&
                memcmp(s_field_names[i], name_start, name_len) == 0) {
                canonical = s_field_names[i];
                break;
            }
        }
        if (!canonical) {
            /* Skip unknown — schema drift, not fatal. */
            scan = name_end + 1;
            continue;
        }
        fields[parsed].name = canonical;

        const char *after = name_end + 1;
        const char *kp = pb_find_key(after, end, "mean_bytes");
        if (!kp || pb_parse_u64_after_colon(kp, end, &fields[parsed].mean_bytes) != 0)
            break;
        kp = pb_find_key(after, end, "samples");
        if (!kp || pb_parse_u64_after_colon(kp, end, &fields[parsed].samples) != 0)
            break;
        kp = pb_find_key(after, end, "non_empty_count");
        if (!kp || pb_parse_u64_after_colon(kp, end, &fields[parsed].non_empty_count) != 0)
            break;
        parsed++;
        const char *close = (const char *)memchr(after, '}', (size_t)(end - after));
        if (!close)
            break;
        scan = close + 1;
    }

    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    out->fields = fields;
    out->field_count = parsed;
    return HU_OK;
}

void hu_prompt_budget_snapshot_load_free(hu_prompt_budget_snapshot_load_t *load) {
    if (!load)
        return;
    if (load->fields && load->alloc && load->field_count > 0) {
        load->alloc->free(load->alloc->ctx, load->fields,
                          load->field_count * sizeof(hu_prompt_budget_field_stat_ext_t));
    }
    memset(load, 0, sizeof(*load));
}
