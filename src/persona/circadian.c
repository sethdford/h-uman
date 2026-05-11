/*
 * Circadian persona overlay — time-of-day adaptive guidance.
 */
#include "human/persona/circadian.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/data/loader.h"
#include "human/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default fallback values */
static const char *DEFAULT_PHASE_NAMES[] = {
    "early morning", "morning", "afternoon", "evening", "night", "late night",
};

static const char *DEFAULT_PHASE_GUIDANCE[] = {
    "Be gentle and warm. The user is starting their day. Keep responses calm and encouraging.",
    "Be energetic and productive. The user is at peak mental clarity. Be direct and efficient.",
    "Be steady and focused. Energy may be dipping. Keep things clear and structured.",
    "Be relaxed and reflective. The day is winding down. Allow for deeper conversation.",
    "Be calm and intimate. The user is in a quieter headspace. Slow your pace, be thoughtful.",
    ("Be present and unhurried. Late night conversations often carry more weight. Be a quiet "
     "companion."),
};

/* Runtime loaded data */
static const char **s_phase_names = (const char **)DEFAULT_PHASE_NAMES;
static const char **s_phase_guidance = (const char **)DEFAULT_PHASE_GUIDANCE;
static size_t s_phase_count = 6;

hu_error_t hu_circadian_data_init(hu_allocator_t *alloc) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    char *json_data = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_data_load(alloc, "persona/circadian_phases.json", &json_data, &json_len);
    if (err != HU_OK)
        return HU_OK; /* Fail gracefully, keep defaults */

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, json_data, json_len, &root);
    alloc->free(alloc->ctx, json_data, json_len);
    if (err != HU_OK || !root)
        return HU_OK; /* Fail gracefully, keep defaults */

    hu_json_value_t *phases_arr = hu_json_object_get(root, "phases");
    if (!phases_arr || phases_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    size_t phase_count = phases_arr->data.array.len;
    if (phase_count == 0 || phase_count > 6) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    const char **names =
        (const char **)alloc->alloc(alloc->ctx, phase_count * sizeof(const char *));
    const char **guidance =
        (const char **)alloc->alloc(alloc->ctx, phase_count * sizeof(const char *));
    if (!names || !guidance) {
        if (names)
            alloc->free(alloc->ctx, names, phase_count * sizeof(const char *));
        if (guidance)
            alloc->free(alloc->ctx, guidance, phase_count * sizeof(const char *));
        hu_json_free(alloc, root);
        return HU_OK;
    }

    memset(names, 0, phase_count * sizeof(const char *));
    memset(guidance, 0, phase_count * sizeof(const char *));

    for (size_t i = 0; i < phase_count; i++) {
        hu_json_value_t *phase_obj = phases_arr->data.array.items[i];
        if (!phase_obj || phase_obj->type != HU_JSON_OBJECT)
            continue;

        const char *name = hu_json_get_string(phase_obj, "name");
        const char *guide = hu_json_get_string(phase_obj, "guidance");

        if (name)
            names[i] = hu_strndup(alloc, name, strlen(name));
        if (guide)
            guidance[i] = hu_strndup(alloc, guide, strlen(guide));
    }

    /* Atomically swap in new data */
    s_phase_names = names;
    s_phase_guidance = guidance;
    s_phase_count = phase_count;

    hu_json_free(alloc, root);
    return HU_OK;
}

void hu_circadian_data_cleanup(hu_allocator_t *alloc) {
    if (!alloc)
        return;
    /* Only free if they're not the defaults */
    if (s_phase_names != (const char **)DEFAULT_PHASE_NAMES) {
        for (size_t i = 0; i < s_phase_count; i++) {
            if (s_phase_names[i])
                alloc->free(alloc->ctx, (char *)s_phase_names[i], strlen(s_phase_names[i]) + 1);
        }
        alloc->free(alloc->ctx, s_phase_names, s_phase_count * sizeof(const char *));
    }
    if (s_phase_guidance != (const char **)DEFAULT_PHASE_GUIDANCE) {
        for (size_t i = 0; i < s_phase_count; i++) {
            if (s_phase_guidance[i])
                alloc->free(alloc->ctx, (char *)s_phase_guidance[i],
                            strlen(s_phase_guidance[i]) + 1);
        }
        alloc->free(alloc->ctx, s_phase_guidance, s_phase_count * sizeof(const char *));
    }
    s_phase_names = (const char **)DEFAULT_PHASE_NAMES;
    s_phase_guidance = (const char **)DEFAULT_PHASE_GUIDANCE;
    s_phase_count = 6;
}

hu_time_phase_t hu_circadian_phase(uint8_t hour) {
    if (hour < 5)
        return HU_PHASE_LATE_NIGHT;
    if (hour < 8)
        return HU_PHASE_EARLY_MORNING;
    if (hour < 12)
        return HU_PHASE_MORNING;
    if (hour < 17)
        return HU_PHASE_AFTERNOON;
    if (hour < 21)
        return HU_PHASE_EVENING;
    /* 21:00-23:59 */
    return HU_PHASE_NIGHT;
}

hu_error_t hu_circadian_build_prompt(hu_allocator_t *alloc, uint8_t hour, char **out,
                                     size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    hu_time_phase_t phase = hu_circadian_phase(hour);
    const char *name = s_phase_names[(size_t)phase];
    const char *guidance = s_phase_guidance[(size_t)phase];

#define HU_CIRCADIAN_BUF_CAP 256
    char *buf = (char *)alloc->alloc(alloc->ctx, HU_CIRCADIAN_BUF_CAP);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, HU_CIRCADIAN_BUF_CAP, "\n### Time Awareness\nCurrent phase: %s. %s\n",
                     name, guidance);
    if (n <= 0 || (size_t)n >= HU_CIRCADIAN_BUF_CAP) {
        alloc->free(alloc->ctx, buf, HU_CIRCADIAN_BUF_CAP);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, HU_CIRCADIAN_BUF_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, HU_CIRCADIAN_BUF_CAP);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = shrunk;
    *out_len = (size_t)n;
#undef HU_CIRCADIAN_BUF_CAP
    return HU_OK;
}

/* ── Routine-aware circadian prompt ──────────────────────────────── */

/* Parse "HH:MM" from a routine block time string, return the hour. */
static int routine_block_hour(const char *time_str) {
    if (!time_str || time_str[0] == '\0')
        return -1;
    int h = 0;
    for (int i = 0; i < 2 && time_str[i] >= '0' && time_str[i] <= '9'; i++)
        h = h * 10 + (time_str[i] - '0');
    return h;
}

/* Find the routine block whose time falls closest to (but not after) the given hour.
 * Returns NULL if no blocks exist. */
static const hu_routine_block_t *find_routine_block_for_hour(const hu_routine_block_t *blocks,
                                                             size_t count, uint8_t hour) {
    if (!blocks || count == 0)
        return NULL;
    const hu_routine_block_t *best = NULL;
    int best_diff = 25;
    for (size_t i = 0; i < count; i++) {
        int bh = routine_block_hour(blocks[i].time);
        if (bh < 0)
            continue;
        /* How far back is this block from the target hour? */
        int diff = (int)hour - bh;
        if (diff < 0)
            diff += 24; /* wrapped past midnight */
        if (diff < best_diff) {
            best_diff = diff;
            best = &blocks[i];
        }
    }
    return best;
}

hu_error_t hu_circadian_build_prompt_with_routine(hu_allocator_t *alloc, uint8_t hour,
                                                  const struct hu_daily_routine *routine,
                                                  char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    /* No routine — fall back to default */
    if (!routine || routine->weekday_count == 0)
        return hu_circadian_build_prompt(alloc, hour, out, out_len);

    /* Find the routine block active at this hour (weekday for now) */
    const hu_routine_block_t *block =
        find_routine_block_for_hour(routine->weekday, routine->weekday_count, hour);

    /* No matching block or empty mood_modifier — fall back */
    if (!block || block->mood_modifier[0] == '\0')
        return hu_circadian_build_prompt(alloc, hour, out, out_len);

    hu_time_phase_t phase = hu_circadian_phase(hour);
    const char *name = s_phase_names[(size_t)phase];
    const char *default_guidance = s_phase_guidance[(size_t)phase];

#define HU_CIRCADIAN_ROUTINE_BUF_CAP 512
    char *buf = (char *)alloc->alloc(alloc->ctx, HU_CIRCADIAN_ROUTINE_BUF_CAP);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, HU_CIRCADIAN_ROUTINE_BUF_CAP,
                     "\n### Time Awareness\nCurrent phase: %s. "
                     "Persona energy: %s (activity: %s). %s\n",
                     name, block->mood_modifier, block->activity, default_guidance);
    if (n <= 0 || (size_t)n >= HU_CIRCADIAN_ROUTINE_BUF_CAP) {
        alloc->free(alloc->ctx, buf, HU_CIRCADIAN_ROUTINE_BUF_CAP);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, HU_CIRCADIAN_ROUTINE_BUF_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, HU_CIRCADIAN_ROUTINE_BUF_CAP);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = shrunk;
    *out_len = (size_t)n;
#undef HU_CIRCADIAN_ROUTINE_BUF_CAP
    return HU_OK;
}

/* ── Persona-aware circadian prompt ──────────────────────────────── */

const char *hu_circadian_persona_overlay(const struct hu_persona *persona, hu_time_phase_t phase) {
    if (!persona)
        return NULL;
    const hu_persona_t *p = (const hu_persona_t *)persona;
    switch (phase) {
    case HU_PHASE_EARLY_MORNING:
        return (p->time_overlay_early_morning && p->time_overlay_early_morning[0] != '\0')
                   ? p->time_overlay_early_morning
                   : NULL;
    case HU_PHASE_AFTERNOON:
        return (p->time_overlay_afternoon && p->time_overlay_afternoon[0] != '\0')
                   ? p->time_overlay_afternoon
                   : NULL;
    case HU_PHASE_EVENING:
    case HU_PHASE_NIGHT:
        return (p->time_overlay_evening && p->time_overlay_evening[0] != '\0')
                   ? p->time_overlay_evening
                   : NULL;
    case HU_PHASE_LATE_NIGHT:
        return (p->time_overlay_late_night && p->time_overlay_late_night[0] != '\0')
                   ? p->time_overlay_late_night
                   : NULL;
    case HU_PHASE_MORNING:
    default:
        return NULL;
    }
}

hu_error_t hu_circadian_build_persona_prompt(hu_allocator_t *alloc, uint8_t hour,
                                             const struct hu_persona *persona, char **out,
                                             size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    /* No persona — fall back to routine-aware or default prompt */
    if (!persona) {
        return hu_circadian_build_prompt(alloc, hour, out, out_len);
    }

    const hu_persona_t *p = (const hu_persona_t *)persona;

    /* Try routine-aware first if persona has a daily routine */
    if (p->daily_routine.weekday_count > 0) {
        /* Find routine block for mood_modifier */
        const hu_routine_block_t *block = find_routine_block_for_hour(
            p->daily_routine.weekday, p->daily_routine.weekday_count, hour);
        hu_time_phase_t phase = hu_circadian_phase(hour);
        const char *name = s_phase_names[(size_t)phase];
        const char *default_guidance = s_phase_guidance[(size_t)phase];
        const char *overlay = hu_circadian_persona_overlay(persona, phase);
        const char *mood = (block && block->mood_modifier[0] != '\0') ? block->mood_modifier : NULL;
        const char *activity = (block && block->activity[0] != '\0') ? block->activity : NULL;

#define HU_CIRCADIAN_PERSONA_BUF_CAP 768
        char *buf = (char *)alloc->alloc(alloc->ctx, HU_CIRCADIAN_PERSONA_BUF_CAP);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;

        int n;
        if (overlay && mood) {
            n = snprintf(buf, HU_CIRCADIAN_PERSONA_BUF_CAP,
                         "\n### Time Awareness\nCurrent phase: %s. "
                         "Persona energy: %s (activity: %s). "
                         "Persona guidance: %s. %s\n",
                         name, mood, activity ? activity : "unknown", overlay, default_guidance);
        } else if (overlay) {
            n = snprintf(buf, HU_CIRCADIAN_PERSONA_BUF_CAP,
                         "\n### Time Awareness\nCurrent phase: %s. "
                         "Persona guidance: %s. %s\n",
                         name, overlay, default_guidance);
        } else if (mood) {
            n = snprintf(buf, HU_CIRCADIAN_PERSONA_BUF_CAP,
                         "\n### Time Awareness\nCurrent phase: %s. "
                         "Persona energy: %s (activity: %s). %s\n",
                         name, mood, activity ? activity : "unknown", default_guidance);
        } else {
            n = snprintf(buf, HU_CIRCADIAN_PERSONA_BUF_CAP,
                         "\n### Time Awareness\nCurrent phase: %s. %s\n", name, default_guidance);
        }

        if (n <= 0 || (size_t)n >= HU_CIRCADIAN_PERSONA_BUF_CAP) {
            alloc->free(alloc->ctx, buf, HU_CIRCADIAN_PERSONA_BUF_CAP);
            return HU_ERR_INVALID_ARGUMENT;
        }

        size_t need = (size_t)n + 1;
        char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, HU_CIRCADIAN_PERSONA_BUF_CAP, need);
        if (!shrunk) {
            alloc->free(alloc->ctx, buf, HU_CIRCADIAN_PERSONA_BUF_CAP);
            return HU_ERR_OUT_OF_MEMORY;
        }
        *out = shrunk;
        *out_len = (size_t)n;
#undef HU_CIRCADIAN_PERSONA_BUF_CAP
        return HU_OK;
    }

    /* No daily routine — check for persona time overlay only */
    hu_time_phase_t phase = hu_circadian_phase(hour);
    const char *overlay = hu_circadian_persona_overlay(persona, phase);
    if (!overlay)
        return hu_circadian_build_prompt(alloc, hour, out, out_len);

    const char *name = s_phase_names[(size_t)phase];
    const char *default_guidance = s_phase_guidance[(size_t)phase];

#define HU_CIRCADIAN_OVERLAY_BUF_CAP 512
    char *buf = (char *)alloc->alloc(alloc->ctx, HU_CIRCADIAN_OVERLAY_BUF_CAP);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, HU_CIRCADIAN_OVERLAY_BUF_CAP,
                     "\n### Time Awareness\nCurrent phase: %s. "
                     "Persona guidance: %s. %s\n",
                     name, overlay, default_guidance);
    if (n <= 0 || (size_t)n >= HU_CIRCADIAN_OVERLAY_BUF_CAP) {
        alloc->free(alloc->ctx, buf, HU_CIRCADIAN_OVERLAY_BUF_CAP);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, HU_CIRCADIAN_OVERLAY_BUF_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, HU_CIRCADIAN_OVERLAY_BUF_CAP);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = shrunk;
    *out_len = (size_t)n;
#undef HU_CIRCADIAN_OVERLAY_BUF_CAP
    return HU_OK;
}

/* B16: chronotype helpers — coarse JITAI gating bands. */

static const char *const HU_CHRONO_NAMES[HU_CHRONO_COUNT] = {
    "unknown",
    "morning_lark",
    "intermediate",
    "evening_owl",
};

const char *hu_chronotype_name(hu_chronotype_t c) {
    if (c < 0 || c >= HU_CHRONO_COUNT) {
        return "unknown";
    }
    return HU_CHRONO_NAMES[c];
}

int hu_chronotype_is_active_hour(hu_chronotype_t c, uint8_t hour) {
    uint8_t h = hour > 23 ? 23 : hour;
    switch (c) {
    case HU_CHRONO_MORNING_LARK:
        return (h >= 6 && h <= 21) ? 1 : 0;
    case HU_CHRONO_EVENING_OWL:
        /* 09–23 active band; tolerant of 00–01 to support late owls. */
        return ((h >= 9 && h <= 23) || h <= 1) ? 1 : 0;
    case HU_CHRONO_INTERMEDIATE:
    case HU_CHRONO_UNKNOWN:
    default:
        return (h >= 7 && h <= 22) ? 1 : 0;
    }
}
