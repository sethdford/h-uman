#include "human/persona/somatic.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

static float somatic_clamp01(float x) {
    if (x < 0.f)
        return 0.f;
    if (x > 1.f)
        return 1.f;
    return x;
}

static const char *somatic_physical_name(hu_physical_state_t state) {
    switch (state) {
    case HU_PHYSICAL_NORMAL:
        return "normal";
    case HU_PHYSICAL_TIRED:
        return "tired";
    case HU_PHYSICAL_CAFFEINATED:
        return "caffeinated";
    case HU_PHYSICAL_SORE:
        return "sore";
    case HU_PHYSICAL_HUNGRY:
        return "hungry";
    case HU_PHYSICAL_EATING:
        return "eating";
    case HU_PHYSICAL_SICK:
        return "sick";
    case HU_PHYSICAL_ENERGIZED:
        return "energized";
    case HU_PHYSICAL_COLD:
        return "cold";
    case HU_PHYSICAL_HOT:
        return "hot";
    }
    return "normal";
}

void hu_somatic_init(hu_somatic_state_t *state) {
    if (!state)
        return;
    state->energy = 1.f;
    state->social_battery = 1.f;
    state->focus = 1.f;
    state->arousal = 0.f;
    state->physical = HU_PHYSICAL_NORMAL;
    state->last_interaction_ts = 0;
    state->last_recharge_ts = 0;
    state->conversation_load_accumulated = 0.f;
}

void hu_somatic_update(hu_somatic_state_t *state, uint64_t now_ts, float emotional_intensity,
                       uint32_t topic_switches, hu_physical_state_t scheduled_physical) {
    if (!state)
        return;

    float hours_since = 0.f;
    if (state->last_interaction_ts > 0U && now_ts >= state->last_interaction_ts) {
        hours_since = (float)(now_ts - state->last_interaction_ts) / 3600.f;
    }

    state->energy += hours_since * 0.12f;
    state->energy -= 0.03f;
    state->conversation_load_accumulated += 0.03f;

    if (hours_since > 8.f) {
        state->social_battery = 1.f;
    } else {
        if (hours_since > 2.f)
            state->social_battery += hours_since * 0.1f;
        state->social_battery -= 0.02f;
    }

    if (topic_switches > 0U)
        state->focus -= 0.05f * (float)topic_switches;
    else
        state->focus += 0.02f;

    state->arousal = somatic_clamp01(emotional_intensity);

    state->energy = somatic_clamp01(state->energy);
    state->social_battery = somatic_clamp01(state->social_battery);
    state->focus = somatic_clamp01(state->focus);

    state->physical = scheduled_physical;
    if (state->energy < 0.2f || state->social_battery < 0.15f)
        state->physical = HU_PHYSICAL_TIRED;

    state->last_interaction_ts = now_ts;
}

static const char *somatic_focus_descriptor(float focus) {
    if (focus > 0.7f)
        return "high";
    if (focus > 0.4f)
        return "moderate";
    if (focus > 0.2f)
        return "low";
    return "fragmented";
}

hu_error_t hu_somatic_build_context(hu_allocator_t *alloc, const hu_somatic_state_t *state,
                                    char **out, size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    int epct = (int)(state->energy * 100.f + 0.5f);
    int spct = (int)(state->social_battery * 100.f + 0.5f);
    if (epct < 0)
        epct = 0;
    if (epct > 100)
        epct = 100;
    if (spct < 0)
        spct = 0;
    if (spct > 100)
        spct = 100;

    const char *energy_l = hu_somatic_energy_label(state->energy);
    const char *bat_l = hu_somatic_battery_label(state->social_battery);
    const char *focus_d = somatic_focus_descriptor(state->focus);
    const char *phys = somatic_physical_name(state->physical);
    int apct = (int)(state->arousal * 100.f + 0.5f);
    if (apct < 0)
        apct = 0;
    if (apct > 100)
        apct = 100;

    const char *directive = "Stay present and proportionate to the user's tone.";
    if (state->social_battery < 0.4f)
        directive = "You're a bit drained — be genuine but don't force enthusiasm.";
    else if (state->energy < 0.35f)
        directive = "Pace yourself: shorter clauses, fewer flourishes, same care.";
    else if (state->arousal > 0.75f)
        directive = "The user is highly activated — match steadiness; don't escalate.";
    else if (state->focus < 0.35f)
        directive = "Attention is thin — one clear thread at a time.";

    char *built = hu_sprintf(alloc,
                             "[SOMATIC STATE: Energy %d%% (%s), social battery %d%% (%s), focus "
                             "%s. Physical: %s. Arousal "
                             "~%d%%. %s]",
                             epct, energy_l, spct, bat_l, focus_d, phys, apct, directive);
    if (!built)
        return HU_ERR_OUT_OF_MEMORY;

    *out = built;
    *out_len = strlen(built);
    return HU_OK;
}

const char *hu_somatic_energy_label(float energy) {
    if (energy > 0.7f)
        return "high";
    if (energy > 0.4f)
        return "moderate";
    if (energy > 0.2f)
        return "low";
    return "depleted";
}

const char *hu_somatic_battery_label(float battery) {
    if (battery > 0.7f)
        return "charged";
    if (battery > 0.4f)
        return "winding down";
    if (battery > 0.2f)
        return "drained";
    return "empty";
}

/* ── Persistence (SOTA roadmap #11) ─────────────────────────────────── */

#include "human/core/json.h"
#include "human/core/state_file.h"
#include <stdlib.h>

hu_error_t hu_somatic_save_file(const hu_somatic_state_t *state, const char *path) {
    if (!state || !path || !path[0])
        return HU_ERR_INVALID_ARGUMENT;

    /* Staged write + atomic rename so a crash mid-write can never leave a
     * torn file (same-inode overwrite hazards:
     * .claude/rules/never-cp-over-running-binary.md — different artifact,
     * same atomic-replace principle). */
    char tmp[512];
    FILE *f = hu_state_file_write_begin(path, tmp, sizeof(tmp));
    if (!f)
        return HU_ERR_IO;
    int w = fprintf(f,
                    "{\"energy\": %.4f, \"social_battery\": %.4f, \"focus\": %.4f, "
                    "\"arousal\": %.4f, \"physical\": %d, \"last_interaction_ts\": %llu, "
                    "\"last_recharge_ts\": %llu, \"conversation_load_accumulated\": %.4f}\n",
                    (double)state->energy, (double)state->social_battery, (double)state->focus,
                    (double)state->arousal, (int)state->physical,
                    (unsigned long long)state->last_interaction_ts,
                    (unsigned long long)state->last_recharge_ts,
                    (double)state->conversation_load_accumulated);
    return hu_state_file_write_commit(f, w > 0, tmp, path);
}

hu_error_t hu_somatic_load_file(hu_somatic_state_t *state, const char *path) {
    if (!state || !path || !path[0])
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_NOT_FOUND;
    char buf[1024];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (got == 0)
        return HU_ERR_IO;
    buf[got] = '\0';

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    if (hu_json_parse(&alloc, buf, got, &root) != HU_OK || !root)
        return HU_ERR_PARSE;
    if (root->type != HU_JSON_OBJECT) {
        hu_json_free(&alloc, root);
        return HU_ERR_PARSE;
    }

    /* Parse into a local, then clamp, then commit — the caller's state is
     * untouched on any failure and never receives out-of-range values.
     * hu_json_get_number returns the default when a key is absent, so
     * missing fields simply keep their current values. */
    hu_somatic_state_t s = *state;
    s.energy = somatic_clamp01((float)hu_json_get_number(root, "energy", (double)s.energy));
    s.social_battery = somatic_clamp01(
        (float)hu_json_get_number(root, "social_battery", (double)s.social_battery));
    s.focus = somatic_clamp01((float)hu_json_get_number(root, "focus", (double)s.focus));
    s.arousal = somatic_clamp01((float)hu_json_get_number(root, "arousal", (double)s.arousal));
    int p = (int)hu_json_get_number(root, "physical", (double)(int)s.physical);
    if (p >= 0 && p <= (int)HU_PHYSICAL_ENERGIZED)
        s.physical = (hu_physical_state_t)p;
    double d = hu_json_get_number(root, "last_interaction_ts", (double)s.last_interaction_ts);
    if (d >= 0)
        s.last_interaction_ts = (uint64_t)d;
    d = hu_json_get_number(root, "last_recharge_ts", (double)s.last_recharge_ts);
    if (d >= 0)
        s.last_recharge_ts = (uint64_t)d;
    d = hu_json_get_number(root, "conversation_load_accumulated",
                           (double)s.conversation_load_accumulated);
    if (d >= 0 && d <= 1000.0)
        s.conversation_load_accumulated = (float)d;

    hu_json_free(&alloc, root);
    *state = s;
    return HU_OK;
}

const char *hu_somatic_default_path(char *buf, size_t cap) {
    return hu_state_file_default_path("somatic_state.json", buf, cap);
}
