#include "human/agent/worktree.h"
#include "human/core/process_util.h"
#include "human/core/string.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_WORKTREE_PATH_FMT   "%s/../.worktrees/%s"
#define HU_WORKTREE_BRANCH_FMT "agent/%llu/%s"
#define HU_WORKTREE_INIT_CAP   8

struct hu_worktree_manager {
    hu_allocator_t *alloc;
    char *repo_root;
    hu_worktree_t *worktrees;
    size_t count;
    size_t capacity;
};

#if !(defined(HU_IS_TEST) && HU_IS_TEST == 1)
static bool is_safe_path(const char *path) {
    if (!path || !path[0])
        return false;
    for (const char *p = path; *p; p++) {
        char c = *p;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '.' && c != '_' && c != '/' && c != '-' && c != '~')
            return false;
    }
    return true;
}
#endif

static bool is_safe_label(const char *label) {
    if (!label || !label[0])
        return false;
    if (strstr(label, ".."))
        return false;
    for (const char *p = label; *p; p++) {
        if (*p == '/' || *p == '\\')
            return false;
    }
    return true;
}

static hu_worktree_t *find_by_agent_id(hu_worktree_manager_t *mgr, uint64_t agent_id) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->worktrees[i].agent_id == agent_id && mgr->worktrees[i].active)
            return &mgr->worktrees[i];
    }
    return NULL;
}

#if !(defined(HU_IS_TEST) && HU_IS_TEST == 1)
static hu_error_t run_git_worktree_add(hu_allocator_t *alloc, const char *repo_root,
                                       const char *path, const char *branch) {
    if (!is_safe_path(repo_root) || !is_safe_path(path) || !is_safe_path(branch))
        return HU_ERR_INVALID_ARGUMENT;
    const char *argv[] = {"git", "worktree", "add", path, "-b", branch, NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, repo_root, 4096, &result);
    bool ok = (err == HU_OK && result.success);
    hu_run_result_free(alloc, &result);
    return ok ? HU_OK : HU_ERR_IO;
}

static hu_error_t run_git_worktree_remove(hu_allocator_t *alloc, const char *repo_root,
                                          const char *path) {
    if (!is_safe_path(repo_root) || !is_safe_path(path))
        return HU_ERR_INVALID_ARGUMENT;
    const char *argv[] = {"git", "worktree", "remove", path, "--force", NULL};
    hu_run_result_t result = {0};
    hu_error_t err = hu_process_run(alloc, argv, repo_root, 4096, &result);
    bool ok = (err == HU_OK && result.success);
    hu_run_result_free(alloc, &result);
    return ok ? HU_OK : HU_ERR_IO;
}
#endif /* !HU_IS_TEST */

static hu_error_t grow_if_needed(hu_worktree_manager_t *mgr) {
    if (mgr->count < mgr->capacity)
        return HU_OK;
    size_t new_cap = mgr->capacity == 0 ? HU_WORKTREE_INIT_CAP : mgr->capacity * 2;
    hu_worktree_t *n =
        (hu_worktree_t *)mgr->alloc->alloc(mgr->alloc->ctx, new_cap * sizeof(hu_worktree_t));
    if (!n)
        return HU_ERR_OUT_OF_MEMORY;
    memset(n, 0, new_cap * sizeof(hu_worktree_t));
    if (mgr->worktrees) {
        memcpy(n, mgr->worktrees, mgr->count * sizeof(hu_worktree_t));
        mgr->alloc->free(mgr->alloc->ctx, mgr->worktrees, mgr->capacity * sizeof(hu_worktree_t));
    }
    mgr->worktrees = n;
    mgr->capacity = new_cap;
    return HU_OK;
}

static void free_worktree(hu_allocator_t *a, hu_worktree_t *wt) {
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

hu_worktree_manager_t *hu_worktree_manager_create(hu_allocator_t *alloc, const char *repo_root) {
    if (!alloc || !repo_root)
        return NULL;
    hu_worktree_manager_t *mgr = (hu_worktree_manager_t *)alloc->alloc(alloc->ctx, sizeof(*mgr));
    if (!mgr)
        return NULL;
    memset(mgr, 0, sizeof(*mgr));
    mgr->alloc = alloc;
    mgr->repo_root = hu_strdup(alloc, repo_root);
    if (!mgr->repo_root) {
        alloc->free(alloc->ctx, mgr, sizeof(*mgr));
        return NULL;
    }
    return mgr;
}

void hu_worktree_manager_destroy(hu_worktree_manager_t *mgr) {
    if (!mgr)
        return;
    hu_allocator_t *a = mgr->alloc;
    for (size_t i = 0; i < mgr->count; i++) {
        free_worktree(a, &mgr->worktrees[i]);
    }
    if (mgr->repo_root)
        a->free(a->ctx, mgr->repo_root, strlen(mgr->repo_root) + 1);
    if (mgr->worktrees)
        a->free(a->ctx, mgr->worktrees, mgr->capacity * sizeof(hu_worktree_t));
    a->free(a->ctx, mgr, sizeof(*mgr));
}

hu_error_t hu_worktree_create(hu_worktree_manager_t *mgr, uint64_t agent_id, const char *label,
                              const char **out_path) {
    if (!mgr || !out_path)
        return HU_ERR_INVALID_ARGUMENT;
    if (find_by_agent_id(mgr, agent_id))
        return HU_ERR_ALREADY_EXISTS;

    const char *lbl = (label && label[0]) ? label : "agent";
    if (!is_safe_label(lbl))
        return HU_ERR_INVALID_ARGUMENT;
    char path[1024];
    char branch[256];
    int n = snprintf(path, sizeof(path), HU_WORKTREE_PATH_FMT, mgr->repo_root, lbl);
    if (n < 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    n = snprintf(branch, sizeof(branch), HU_WORKTREE_BRANCH_FMT, (unsigned long long)agent_id, lbl);
    if (n < 0 || (size_t)n >= sizeof(branch))
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST == 1
    (void)path;
    (void)branch;
#else
    if (run_git_worktree_add(mgr->alloc, mgr->repo_root, path, branch) != HU_OK)
        return HU_ERR_IO;
#endif

    hu_error_t err = grow_if_needed(mgr);
    if (err != HU_OK)
        return err;

    hu_worktree_t *wt = &mgr->worktrees[mgr->count++];
    wt->path = hu_strdup(mgr->alloc, path);
    wt->branch = hu_strdup(mgr->alloc, branch);
    wt->agent_id = agent_id;
    wt->active = true;

    *out_path = wt->path;
    return HU_OK;
}

hu_error_t hu_worktree_remove(hu_worktree_manager_t *mgr, uint64_t agent_id) {
    if (!mgr)
        return HU_ERR_INVALID_ARGUMENT;
    hu_worktree_t *wt = find_by_agent_id(mgr, agent_id);
    if (!wt)
        return HU_ERR_NOT_FOUND;

#if defined(HU_IS_TEST) && HU_IS_TEST == 1
    (void)wt;
#else
    if (run_git_worktree_remove(mgr->alloc, mgr->repo_root, wt->path) != HU_OK)
        return HU_ERR_IO;
#endif

    wt->active = false;
    return HU_OK;
}

hu_error_t hu_worktree_list(hu_worktree_manager_t *mgr, hu_worktree_t **out, size_t *count) {
    if (!mgr || !out || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *count = 0;

    size_t n = 0;
    for (size_t i = 0; i < mgr->count; i++)
        if (mgr->worktrees[i].active)
            n++;
    if (n == 0)
        return HU_OK;

    hu_worktree_t *arr =
        (hu_worktree_t *)mgr->alloc->alloc(mgr->alloc->ctx, n * sizeof(hu_worktree_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;

    size_t j = 0;
    for (size_t i = 0; i < mgr->count && j < n; i++) {
        if (!mgr->worktrees[i].active)
            continue;
        hu_worktree_t *src = &mgr->worktrees[i];
        arr[j].path = src->path ? hu_strdup(mgr->alloc, src->path) : NULL;
        arr[j].branch = src->branch ? hu_strdup(mgr->alloc, src->branch) : NULL;
        arr[j].agent_id = src->agent_id;
        arr[j].active = src->active;
        j++;
    }
    *out = arr;
    *count = j;
    return HU_OK;
}

const char *hu_worktree_path_for_agent(hu_worktree_manager_t *mgr, uint64_t agent_id) {
    if (!mgr)
        return NULL;
    hu_worktree_t *wt = find_by_agent_id(mgr, agent_id);
    return wt ? wt->path : NULL;
}

void hu_worktree_list_free(hu_allocator_t *alloc, hu_worktree_t *list, size_t count) {
    if (!alloc || !list)
        return;
    for (size_t i = 0; i < count; i++)
        free_worktree(alloc, &list[i]);
    alloc->free(alloc->ctx, list, count * sizeof(hu_worktree_t));
}
