#ifndef SC_WORKTREE_H
#define SC_WORKTREE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sc_worktree {
    char *path;          /* absolute path to worktree directory */
    char *branch;        /* branch name */
    uint64_t agent_id;   /* owning agent */
    bool active;
} sc_worktree_t;

typedef struct sc_worktree_manager sc_worktree_manager_t;

/* Create manager. repo_root is the main git repo path. */
sc_worktree_manager_t *sc_worktree_manager_create(sc_allocator_t *alloc,
    const char *repo_root);
void sc_worktree_manager_destroy(sc_worktree_manager_t *mgr);

/* Create a new worktree for an agent. Branch name auto-generated: "agent/<agent_id>/<label>"
 * worktree_path is set on success (owned by manager, valid until destroy) */
sc_error_t sc_worktree_create(sc_worktree_manager_t *mgr, uint64_t agent_id,
    const char *label, const char **out_path);

/* Remove a worktree when agent is done */
sc_error_t sc_worktree_remove(sc_worktree_manager_t *mgr, uint64_t agent_id);

/* List active worktrees */
sc_error_t sc_worktree_list(sc_worktree_manager_t *mgr, sc_worktree_t **out,
    size_t *count);

/* Get worktree path for an agent (NULL if none) */
const char *sc_worktree_path_for_agent(sc_worktree_manager_t *mgr,
    uint64_t agent_id);

void sc_worktree_list_free(sc_allocator_t *alloc, sc_worktree_t *list,
    size_t count);

#endif /* SC_WORKTREE_H */
