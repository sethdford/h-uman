/* One-shot: dump the PRODUCTION persona head for blind-A/B arm generation —
 * routed through hu_agent_build_persona_head, so the HU_PERSONA_HEAD gate
 * (off|shadow|live) selects full vs compact-immersive exactly as the daemon
 * does, and hu_agent_apply_relationship_tone appends the per-contact tone
 * note under HU_WARMTH_TONE_VOCAB exactly as both turn paths do.
 *
 * Cycle-3 replacement for tools/dump_prompt.c, whose output was the OLD
 * eval-compact (hu_persona_build_prompt_compact, ~3.6KB) — that tool never
 * touches the gate, so a "compact arm" generated with it measures neither
 * production head. Not part of the build; compiled ad hoc against the dev
 * libhuman_core.a (ASan — link with -fsanitize=address).
 *
 * Usage:  dump_prompt_head <persona> <channel> <contact_id|-> [ignored]
 *   HU_PERSONA_HEAD=off|shadow|live selects the head (default off = full).
 *   HU_WARMTH_TONE_VOCAB gates the warmth vocabulary as in production.
 *   The 4th arg is accepted and ignored for gen_direct.py --dumper
 *   call-shape compatibility.
 */
#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/persona.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "seth";
    const char *channel = argc > 2 ? argv[2] : "imessage";
    const char *contact_id = (argc > 3 && strcmp(argv[3], "-") != 0) ? argv[3] : NULL;

    hu_allocator_t alloc = hu_system_allocator();
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    hu_error_t e = hu_persona_load(&alloc, name, strlen(name), &p);
    if (e != HU_OK) {
        fprintf(stderr, "persona_load failed: %d\n", (int)e);
        return 2;
    }

    /* Minimal agent fixture — the same fields the shared helpers read
     * (mirrors tests/test_persona_head_gate.c + test_relationship_tone.c). */
    hu_agent_t *agent = calloc(1, sizeof(*agent));
    if (!agent) {
        hu_persona_deinit(&alloc, &p);
        return 5;
    }
    agent->alloc = &alloc;
    agent->persona = &p;
    agent->active_channel = channel;
    agent->active_channel_len = strlen(channel);
    if (contact_id) {
        agent->memory_session_id = contact_id;
        agent->memory_session_id_len = strlen(contact_id);
    }

    char *head = NULL;
    size_t head_len = 0;
    e = hu_agent_build_persona_head(agent, NULL, 0, &head, &head_len);
    if (e != HU_OK || !head) {
        fprintf(stderr, "build_persona_head failed: %d\n", (int)e);
        free(agent);
        hu_persona_deinit(&alloc, &p);
        return 3;
    }

    /* Per-contact relationship tone, same shared helper as both turn paths. */
    hu_agent_apply_relationship_tone(agent, &head, &head_len);

    fwrite(head, 1, head_len, stdout);
    alloc.free(alloc.ctx, head, head_len + 1);
    free(agent);
    hu_persona_deinit(&alloc, &p);
    return 0;
}
