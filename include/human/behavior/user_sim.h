#ifndef HU_BEHAVIOR_USER_SIM_H
#define HU_BEHAVIOR_USER_SIM_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>
#include <stdint.h>

/* B9: bounded user simulator — regression harness for multi-turn trajectories.
 * Default implementation is a static scripted line feed (no ML).
 */

typedef struct hu_user_sim_turn_ctx {
    const char *assistant_text;
    size_t assistant_text_len;
    uint32_t turn_index;
} hu_user_sim_turn_ctx_t;

typedef struct hu_user_sim_vtable {
    const char *name;
    hu_error_t (*next_user_message)(void *ctx, const hu_user_sim_turn_ctx_t *tctx, char **out,
                                    size_t *out_len);
    void (*deinit)(void *ctx);
} hu_user_sim_vtable_t;

typedef struct hu_user_sim {
    void *ctx;
    const hu_user_sim_vtable_t *vtable;
} hu_user_sim_t;

hu_error_t hu_user_sim_next(hu_user_sim_t *sim, const hu_user_sim_turn_ctx_t *tctx, char **out,
                            size_t *out_len);
void hu_user_sim_deinit(hu_user_sim_t *sim, hu_allocator_t *alloc);

/* Scripted simulator: `lines` must outlive the sim; copies each line into heap output. */
hu_user_sim_t hu_user_sim_scripted(const char *const *lines, size_t line_count);

#endif /* HU_BEHAVIOR_USER_SIM_H */
