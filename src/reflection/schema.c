/* src/reflection/schema.c — Reflection JSON schema parser + stable id.
 *
 * Spec: docs/plans/2026-05-26-reflection-loop/design.md "Components →
 * src/reflection/schema.c".
 *
 * The reflection model emits JSON of shape:
 *
 *   {
 *     "prose_summary": "2-3 sentence prose...",
 *     "patterns": [
 *       {
 *         "type": "topic_recurrence",
 *         "subject": "alice",
 *         "observation": "Alice has mentioned her job stress 3 times in 2 weeks",
 *         "confidence": 0.82,
 *         "evidence_ids": ["turn_123", "turn_456"],
 *         "channels": ["imessage", "sms"]
 *       },
 *       ...
 *     ]
 *   }
 *
 * This file's `hu_reflection_parse()` validates the shape, computes a
 * stable id per pattern, and returns the heap-allocated array. Pure
 * function — no I/O, no provider call, no SQLite. The storage layer
 * (T2) consumes the parse output; orchestration (T5) glues them.
 *
 * Failure modes the parser distinguishes (returned via *out_error):
 *   - Top-level JSON malformed.
 *   - Required field missing (`patterns` array, or any pattern's
 *     `type`/`subject`/`observation`/`confidence`).
 *   - `type` not in the 6-variant enum.
 *   - `confidence` out of [0, 1].
 *
 * NON-fatal validation (parse continues with a warning logged via the
 * out_error string):
 *   - Pattern confidence below the floor — returned but flagged.
 *   - >8 evidence_ids — truncated to 8.
 *   - >8 channels — truncated to 8.
 *   - Unknown extra fields — ignored. */

#include "human/reflection.h"

#include "human/core/allocator.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Stable type<->string mapping (PUBLIC — used in stable-id hash) ── */

static const char *const k_type_strings[HU_REFLECTION_PATTERN_COUNT] = {
    [HU_REFLECTION_PATTERN_TOPIC_RECURRENCE] = "topic_recurrence",
    [HU_REFLECTION_PATTERN_BEHAVIORAL_SHIFT] = "behavioral_shift",
    [HU_REFLECTION_PATTERN_PREFERENCE] = "preference",
    [HU_REFLECTION_PATTERN_EMOTIONAL_STATE] = "emotional_state",
    [HU_REFLECTION_PATTERN_SCHEDULE_PATTERN] = "schedule_pattern",
    [HU_REFLECTION_PATTERN_RELATIONSHIP] = "relationship",
};

const char *hu_reflection_pattern_type_str(hu_reflection_pattern_type_t type) {
    if ((int)type < 0 || (int)type >= HU_REFLECTION_PATTERN_COUNT)
        return "unknown";
    return k_type_strings[(int)type];
}

static int parse_pattern_type(const char *s, hu_reflection_pattern_type_t *out) {
    if (!s)
        return -1;
    for (int i = 0; i < HU_REFLECTION_PATTERN_COUNT; i++) {
        if (strcmp(s, k_type_strings[i]) == 0) {
            *out = (hu_reflection_pattern_type_t)i;
            return 0;
        }
    }
    return -1;
}

/* ── Stable id: first 16 hex of SHA-256(type|subject|observation[:128]) ──
 *
 * Made PUBLIC at T2 (was static compute_pattern_id at T1) so the
 * storage tests can derive pattern IDs without first round-tripping
 * through hu_reflection_parse. The canonicalization rule is locked:
 * changing the input format breaks every existing reflection_patterns
 * row's UPSERT key. */

void hu_reflection_compute_id(hu_reflection_pattern_type_t type, const char *subject,
                              const char *observation, char *out_id, size_t id_cap) {
    if (!out_id || id_cap == 0)
        return;
    if (id_cap < 17) {
        /* Defensive: emit empty string when buffer is too small for
         * 16 hex + NUL, so callers that strlen()==16 fail loudly
         * rather than read past the buffer. */
        out_id[0] = '\0';
        return;
    }

    /* Input: type_str + "|" + subject + "|" + observation[:128].
     * The 128-byte cap on observation keeps the hash input stable
     * even when the LLM emits slightly different prose phrasings of
     * the same insight — e.g. "Alice is stressed" vs "Alice is
     * stressed about work" hash to different ids, but the cap means
     * trailing variation past 128 chars doesn't churn the id. */
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s|%s|%.128s", hu_reflection_pattern_type_str(type),
                     subject ? subject : "", observation ? observation : "");
    if (n < 0) {
        out_id[0] = '\0';
        return;
    }
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;

    uint8_t hash[32];
    hu_sha256((const uint8_t *)buf, (size_t)n, hash);
    /* First 16 hex chars = first 8 bytes of the digest. 64 bits is
     * plenty for collision-resistance at expected scale (thousands
     * of patterns per user). The full 32 bytes is overkill for an
     * ID column. */
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out_id[i * 2 + 0] = hex[(hash[i] >> 4) & 0xF];
        out_id[i * 2 + 1] = hex[hash[i] & 0xF];
    }
    out_id[16] = '\0';
}

/* ── Helpers ────────────────────────────────────────────────── */

/* Allocate a new heap string copy of `src`, NUL-terminated, length up
 * to max_len chars (truncates the source if longer). Returns NULL on
 * malloc failure. Returns "" (empty heap string) for NULL/empty src
 * — callers in error paths expect a free()-able result. */
static char *heap_str(const char *src) {
    if (!src)
        src = "";
    size_t n = strlen(src);
    char *p = (char *)malloc(n + 1);
    if (!p)
        return NULL;
    memcpy(p, src, n + 1);
    return p;
}

/* Copy `src` into `dst[dst_cap]` with truncation + guaranteed NUL.
 * Returns whether truncation occurred (for the >8-element warning). */
static bool copy_field(char *dst, size_t dst_cap, const char *src) {
    if (dst_cap == 0)
        return false;
    dst[0] = '\0';
    if (!src)
        return false;
    size_t n = strlen(src);
    bool truncated = n >= dst_cap;
    if (truncated)
        n = dst_cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return truncated;
}

/* Build a "key: msg" error string into a heap allocation. Caller
 * frees. Returns NULL on malloc failure (rare; in which case caller
 * just won't have an error message — better than crashing). */
static char *make_error(const char *key, const char *msg) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s: %s", key ? key : "schema", msg ? msg : "unknown error");
    if (n <= 0)
        return NULL;
    return heap_str(buf);
}

/* ── The parser ─────────────────────────────────────────────── */

hu_error_t hu_reflection_parse(const char *json, hu_reflection_pattern_t **out_patterns,
                               int *out_count, char **out_prose_summary, char **out_error) {
    if (!json || !out_patterns || !out_count || !out_prose_summary || !out_error) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *out_patterns = NULL;
    *out_count = 0;
    *out_prose_summary = NULL;
    *out_error = NULL;

    /* Use the system allocator for the JSON tree — we don't have an
     * agent's arena to borrow from. The tree is freed before
     * return. */
    hu_allocator_t alloc = hu_system_allocator();

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &root);
    if (err != HU_OK || !root) {
        *out_error = make_error("top-level", "JSON parse failed");
        if (root)
            hu_json_free(&alloc, root);
        return HU_ERR_JSON_PARSE;
    }
    if (root->type != HU_JSON_OBJECT) {
        *out_error = make_error("top-level", "expected JSON object");
        hu_json_free(&alloc, root);
        return HU_ERR_JSON_PARSE;
    }

    /* Prose summary is optional; missing → empty string is fine. */
    const char *prose = hu_json_get_string(root, "prose_summary");
    *out_prose_summary = heap_str(prose ? prose : "");
    if (!*out_prose_summary) {
        hu_json_free(&alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_json_value_t *patterns_val = hu_json_object_get(root, "patterns");
    if (!patterns_val || patterns_val->type != HU_JSON_ARRAY) {
        /* Empty/missing patterns is valid — a reflection run can
         * produce zero patterns (model concluded nothing new). Free
         * the prose accordingly and return success. */
        hu_json_free(&alloc, root);
        return HU_OK;
    }

    size_t n_patterns = patterns_val->data.array.len;
    if (n_patterns == 0) {
        hu_json_free(&alloc, root);
        return HU_OK;
    }

    hu_reflection_pattern_t *arr =
        (hu_reflection_pattern_t *)calloc(n_patterns, sizeof(hu_reflection_pattern_t));
    if (!arr) {
        free(*out_prose_summary);
        *out_prose_summary = NULL;
        hu_json_free(&alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }

    int valid_count = 0;
    bool any_truncation_warning = false;
    for (size_t i = 0; i < n_patterns; i++) {
        hu_json_value_t *p_val = patterns_val->data.array.items[i];
        if (!p_val || p_val->type != HU_JSON_OBJECT) {
            free(*out_error);
            *out_error = make_error("patterns[?]", "expected object");
            continue;
        }

        const char *type_str = hu_json_get_string(p_val, "type");
        const char *subject = hu_json_get_string(p_val, "subject");
        const char *observation = hu_json_get_string(p_val, "observation");
        if (!type_str || !subject || !observation) {
            free(*out_error);
            *out_error =
                make_error("patterns[?]", "missing required field (type, subject, or observation)");
            continue;
        }

        hu_reflection_pattern_type_t ptype;
        if (parse_pattern_type(type_str, &ptype) != 0) {
            free(*out_error);
            *out_error = make_error("patterns[?].type", "value not in 6-variant enum");
            continue;
        }

        double confidence = hu_json_get_number(p_val, "confidence", -1.0);
        if (confidence < 0.0 || confidence > 1.0) {
            free(*out_error);
            *out_error = make_error("patterns[?].confidence", "must be in [0, 1]");
            continue;
        }

        hu_reflection_pattern_t *p = &arr[valid_count];
        p->type = ptype;
        copy_field(p->subject, sizeof(p->subject), subject);
        copy_field(p->observation, sizeof(p->observation), observation);
        p->confidence = confidence;

        /* evidence_ids: optional array, capped at 8. */
        hu_json_value_t *ev = hu_json_object_get(p_val, "evidence_ids");
        if (ev && ev->type == HU_JSON_ARRAY) {
            size_t ev_n = ev->data.array.len;
            if (ev_n > 8) {
                ev_n = 8;
                any_truncation_warning = true;
            }
            for (size_t k = 0; k < ev_n; k++) {
                hu_json_value_t *item = ev->data.array.items[k];
                if (item && item->type == HU_JSON_STRING && item->data.string.ptr) {
                    copy_field(p->evidence_ids[p->evidence_count], sizeof(p->evidence_ids[0]),
                               item->data.string.ptr);
                    p->evidence_count++;
                }
            }
        }

        /* channels: optional array, capped at 8. */
        hu_json_value_t *ch = hu_json_object_get(p_val, "channels");
        if (ch && ch->type == HU_JSON_ARRAY) {
            size_t ch_n = ch->data.array.len;
            if (ch_n > 8) {
                ch_n = 8;
                any_truncation_warning = true;
            }
            for (size_t k = 0; k < ch_n; k++) {
                hu_json_value_t *item = ch->data.array.items[k];
                if (item && item->type == HU_JSON_STRING && item->data.string.ptr) {
                    copy_field(p->channels[p->channel_count], sizeof(p->channels[0]),
                               item->data.string.ptr);
                    p->channel_count++;
                }
            }
        }

        /* Stable id is computed FROM the canonicalized inputs (post
         * truncation copy_field above), so observation truncation by
         * 511 doesn't affect the id (which only looks at first 128
         * chars). */
        hu_reflection_compute_id(p->type, p->subject, p->observation, p->id, sizeof(p->id));

        valid_count++;
    }

    if (valid_count == 0) {
        /* Every pattern in the array failed validation. The error
         * string already documents the LAST one — caller can log it.
         * Return success with zero patterns; the caller decides
         * whether zero-patterns is a problem. */
        free(arr);
        hu_json_free(&alloc, root);
        return HU_OK;
    }

    /* Shrink the array if some patterns were rejected. realloc on a
     * smaller size is allowed to return NULL; if it does, just keep
     * the larger allocation and use valid_count for the API
     * contract. */
    if (valid_count < (int)n_patterns) {
        hu_reflection_pattern_t *shrunk = (hu_reflection_pattern_t *)realloc(
            arr, (size_t)valid_count * sizeof(hu_reflection_pattern_t));
        if (shrunk)
            arr = shrunk;
    }
    *out_patterns = arr;
    *out_count = valid_count;

    if (any_truncation_warning && !*out_error) {
        *out_error =
            make_error("patterns[?]", "one or more arrays (evidence_ids/channels) truncated to 8");
    }

    hu_json_free(&alloc, root);
    return HU_OK;
}
