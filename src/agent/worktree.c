#include "seaclaw/agent/worktree.h"
#include "seaclaw/core/string.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_WORKTREE_PATH_FMT "%s/../.worktrees/%s"
#define SC_WORKTREE_BRANCH_FMT "agent/%llu/%s"
#define SC_WORKTREE_CMD_ADD "git worktree add %s -b %s"
#define SC_WORKTREE_CMD_REMOVE "git worktree remove %s --force"
#define SC_WORKTREE_INIT_CAP 8

struct sc_worktree_manager {
    sc_allocator_t *alloc;
    char *repo_root;
    sc_worktree_t *worktrees;
    size_t count;
    size_t capacity;
};

static sc_worktree_t *find_by_agent_id(sc_worktree_manager_t *mgr,
    uint64_t agent_id) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->worktrees[i].agent_id == agent_id && mgr->worktrees[i].active)
            return &mgr->worktrees[i];
    }
    return NULL;
}

static sc_error_t run_git_cmd(const char *repo_root, const char *fmt, ...) {
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(cmd))
        return SC_ERR_INVALID_ARGUMENT;

    char full_cmd[1280];
    n = snprintf(full_cmd, sizeof(full_cmd), "cd %s && %s", repo_root, cmd);
    if (n < 0 || (size_t)n >= sizeof(full_cmd))
        return SC_ERR_INVALID_ARGUMENT;

    int r = system(full_cmd);
    return (r == 0) ? SC_OK : SC_ERR_IO;
}

static sc_error_t grow_if_needed(sc_worktree_manager_t *mgr) {
    if (mgr->count < mgr->capacity)
        return SC_OK;
    size_t new_cap = mgr->capacity == 0 ? SC_WORKTREE_INIT_CAP
                                        : mgr->capacity * 2;
    sc_worktree_t *n =
        (sc_worktree_t *)mgr->alloc->alloc(mgr->alloc->ctx,
            new_cap * sizeof(sc_worktree_t));
    if (!n)
        return SC_ERR_OUT_OF_MEMORY;
    memset(n, 0, new_cap * sizeof(sc_worktree_t));
    if (mgr->worktrees) {
        memcpy(n, mgr->worktrees, mgr->count * sizeof(sc_worktree_t));
        mgr->alloc->free(mgr->alloc->ctx, mgr->worktrees,
            mgr->capacity * sizeof(sc_worktree_t));
    }
    mgr->worktrees = n;
    mgr->capacity = new_cap;
    return SC_OK;
}

static void free_worktree(sc_allocator_t *a, sc_worktree_t *wt) {
    if (!a || !wt)
        return;
    if (wt->path) {
        a->free(a->ctx, wt->path, strlen(wt->path) + 1);
        wt->path = NULL;
    }
    if (wt->branch) {
        a->free(a->ctx, wt->branch, strlen(wt->branch) + 1);
        wt->branch = NULL;
    }
}

sc_worktree_manager_t *sc_worktree_manager_create(sc_allocator_t *alloc,
    const char *repo_root) {
    if (!alloc || !repo_root)
        return NULL;
    sc_worktree_manager_t *mgr =
        (sc_worktree_manager_t *)alloc->alloc(alloc->ctx, sizeof(*mgr));
    if (!mgr)
        return NULL;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    mgr->repo_root = sc_strdup(alloc, repo_root);
    if (!mgr->repo_root) {
        alloc->free(alloc->ctx, mgr, sizeof(*mgr));
        return NULL;
    }
    return mgr;
}

void sc_worktree_manager_destroy(sc_worktree_manager_t *mgr) {
    if (!mgr)
        return;
    sc_allocator_t *a = mgr->alloc;
    for (size_t i = 0; i < mgr->count; i++) {
        free_worktree(a, &mgr->worktrees[i]);
    }
    if (mgr->repo_root)
        a->free(a->ctx, mgr->repo_root, strlen(mgr->repo_root) + 1);
    if (mgr->worktrees)
        a->free(a->ctx, mgr->worktrees, mgr->capacity * sizeof(sc_worktree_t));
    a->free(a->ctx, mgr, sizeof(*mgr));
}

sc_error_t sc_worktree_create(sc_worktree_manager_t *mgr, uint64_t agent_id,
    const char *label, const char **out_path) {
    if (!mgr || !out_path)
        return SC_ERR_INVALID_ARGUMENT;
    if (find_by_agent_id(mgr, agent_id))
        return SC_ERR_ALREADY_EXISTS;

    const char *lbl = (label && label[0]) ? label : "agent";
    char path[1024];
    char branch[256];
    int n = snprintf(path, sizeof(path), SC_WORKTREE_PATH_FMT, mgr->repo_root,
        lbl);
    if (n < 0 || (size_t)n >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;
    n = snprintf(branch, sizeof(branch), SC_WORKTREE_BRANCH_FMT,
        (unsigned long long)agent_id, lbl);
    if (n < 0 || (size_t)n >= sizeof(branch))
        return SC_ERR_INVALID_ARGUMENT;

#if defined(SC_IS_TEST) && SC_IS_TEST == 1
    (void)path;
    (void)branch;
#else
    if (run_git_cmd(mgr->repo_root, SC_WORKTREE_CMD_ADD, path, branch) != SC_OK)
        return SC_ERR_IO;
#endif

    sc_error_t err = grow_if_needed(mgr);
    if (err != SC_OK)
        return err;

    sc_worktree_t *wt = &mgr->worktrees[mgr->count++];
    wt->path = sc_strdup(mgr->alloc, path);
    wt->branch = sc_strdup(mgr->alloc, branch);
    wt->agent_id = agent_id;
    wt->active = true;

    *out_path = wt->path;
    return SC_OK;
}

sc_error_t sc_worktree_remove(sc_worktree_manager_t *mgr, uint64_t agent_id) {
    if (!mgr)
        return SC_ERR_INVALID_ARGUMENT;
    sc_worktree_t *wt = find_by_agent_id(mgr, agent_id);
    if (!wt)
        return SC_ERR_NOT_FOUND;

#if defined(SC_IS_TEST) && SC_IS_TEST == 1
    (void)wt;
#else
    if (run_git_cmd(mgr->repo_root, SC_WORKTREE_CMD_REMOVE, wt->path) != SC_OK)
        return SC_ERR_IO;
#endif

    wt->active = false;
    return SC_OK;
}

sc_error_t sc_worktree_list(sc_worktree_manager_t *mgr, sc_worktree_t **out,
    size_t *count) {
    if (!mgr || !out || !count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *count = 0;

    size_t n = 0;
    for (size_t i = 0; i < mgr->count; i++)
        if (mgr->worktrees[i].active)
            n++;
    if (n == 0)
        return SC_OK;

    sc_worktree_t *arr =
        (sc_worktree_t *)mgr->alloc->alloc(mgr->alloc->ctx,
            n * sizeof(sc_worktree_t));
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;

    size_t j = 0;
    for (size_t i = 0; i < mgr->count && j < n; i++) {
        if (!mgr->worktrees[i].active)
            continue;
        sc_worktree_t *src = &mgr->worktrees[i];
        arr[j].path = src->path ? sc_strdup(mgr->alloc, src->path) : NULL;
        arr[j].branch = src->branch ? sc_strdup(mgr->alloc, src->branch) : NULL;
        arr[j].agent_id = src->agent_id;
        arr[j].active = src->active;
        j++;
    }
    *out = arr;
    *count = j;
    return SC_OK;
}

const char *sc_worktree_path_for_agent(sc_worktree_manager_t *mgr,
    uint64_t agent_id) {
    if (!mgr)
        return NULL;
    sc_worktree_t *wt = find_by_agent_id(mgr, agent_id);
    return wt ? wt->path : NULL;
}

void sc_worktree_list_free(sc_allocator_t *alloc, sc_worktree_t *list,
    size_t count) {
    if (!alloc || !list)
        return;
    for (size_t i = 0; i < count; i++)
        free_worktree(alloc, &list[i]);
    alloc->free(alloc->ctx, list, count * sizeof(sc_worktree_t));
}
