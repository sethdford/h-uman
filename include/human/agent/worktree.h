#ifndef HU_WORKTREE_H
#define HU_WORKTREE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_worktree {
    char *path;        /* absolute path to worktree directory */
    char *branch;      /* branch name */
    uint64_t agent_id; /* owning agent */
    bool active;
} hu_worktree_t;

typedef struct hu_worktree_manager hu_worktree_manager_t;

/* Create manager. repo_root is the main git repo path. */
hu_worktree_manager_t *hu_worktree_manager_create(hu_allocator_t *alloc, const char *repo_root);
void hu_worktree_manager_destroy(hu_worktree_manager_t *mgr);

/* Create a new worktree for an agent. Branch name auto-generated: "agent/<agent_id>/<label>"
 * worktree_path is set on success (owned by manager, valid until destroy) */
hu_error_t hu_worktree_create(hu_worktree_manager_t *mgr, uint64_t agent_id, const char *label,
                              const char **out_path);

/* Remove a worktree when agent is done */
hu_error_t hu_worktree_remove(hu_worktree_manager_t *mgr, uint64_t agent_id);

/* List active worktrees */
hu_error_t hu_worktree_list(hu_worktree_manager_t *mgr, hu_worktree_t **out, size_t *count);

/* Get worktree path for an agent (NULL if none) */
const char *hu_worktree_path_for_agent(hu_worktree_manager_t *mgr, uint64_t agent_id);

void hu_worktree_list_free(hu_allocator_t *alloc, hu_worktree_t *list, size_t count);

#endif /* HU_WORKTREE_H */
