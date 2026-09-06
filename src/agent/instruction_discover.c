/*
 * Instruction file discovery — .human.md / HUMAN.md / instructions.md
 *
 * Discovers project-level and user-level instruction files, reads them
 * with per-file and total character limits, and merges in priority order.
 */
#include "human/agent/instruction_discover.h"
#include "human/core/paths.h"
#include "human/core/string.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

hu_error_t hu_instruction_validate_path(hu_allocator_t *alloc, const char *path, size_t path_len,
                                        char **out_canonical, size_t *out_canonical_len) {
    if (!alloc || !path || !out_canonical || !out_canonical_len)
        return HU_ERR_INVALID_ARGUMENT;

    /* Reject null bytes embedded in path */
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] == '\0')
            return HU_ERR_SECURITY_COMMAND_NOT_ALLOWED;
    }

    /* Need a NUL-terminated copy for realpath */
    char *tmp = hu_strndup(alloc, path, path_len);
    if (!tmp)
        return HU_ERR_OUT_OF_MEMORY;

    char resolved[PATH_MAX];
    char *rp = realpath(tmp, resolved);
    alloc->free(alloc->ctx, tmp, path_len + 1);

    if (!rp)
        return HU_ERR_NOT_FOUND;

    size_t rlen = strlen(resolved);
    char *canon = hu_strndup(alloc, resolved, rlen);
    if (!canon)
        return HU_ERR_OUT_OF_MEMORY;

    *out_canonical = canon;
    *out_canonical_len = rlen;
    return HU_OK;
}

/* Check if an inode has been visited (symlink cycle detection). */
static bool inode_visited(const ino_t *visited, size_t count, ino_t ino) {
    for (size_t i = 0; i < count; i++) {
        if (visited[i] == ino)
            return true;
    }
    return false;
}

hu_error_t hu_instruction_file_read(hu_allocator_t *alloc, const char *path,
                                    hu_instruction_source_t source, hu_instruction_file_t *out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    struct stat st;
    if (stat(path, &st) != 0)
        return HU_ERR_NOT_FOUND;

    /* Only read regular files */
    if (!S_ISREG(st.st_mode))
        return HU_ERR_NOT_FOUND;

    FILE *f = fopen(path, "r");
    if (!f)
        return HU_ERR_IO;

    /* Read up to per-file limit + 1 to detect truncation */
    size_t max_read = HU_INSTRUCTION_MAX_CHARS_PER_FILE + 1;
    char *buf = (char *)alloc->alloc(alloc->ctx, max_read);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t nread = fread(buf, 1, max_read, f);
    fclose(f);

    bool truncated = false;
    if (nread > HU_INSTRUCTION_MAX_CHARS_PER_FILE) {
        nread = HU_INSTRUCTION_MAX_CHARS_PER_FILE;
        truncated = true;
    }

    /* Shrink buffer to exact size */
    char *content = hu_strndup(alloc, buf, nread);
    alloc->free(alloc->ctx, buf, max_read);
    if (!content)
        return HU_ERR_OUT_OF_MEMORY;

    size_t plen = strlen(path);
    out->source = source;
    out->path = hu_strndup(alloc, path, plen);
    if (!out->path) {
        alloc->free(alloc->ctx, content, nread + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    out->path_len = plen;
    out->content = content;
    out->content_len = nread;
    out->truncated = truncated;
    out->mtime = (int64_t)st.st_mtime;

    return HU_OK;
}

/* ── merge ───────────────────────────────────────────────────────────────── */

hu_error_t hu_instruction_merge(hu_allocator_t *alloc, const hu_instruction_file_t *files,
                                size_t file_count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    if (file_count == 0 || !files) {
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    /* Calculate total size needed (with separators) */
    size_t total = 0;
    for (size_t i = 0; i < file_count; i++) {
        if (!files[i].content)
            continue;
        /* Header: "# <source> instructions (<path>)\n" + content + "\n\n" */
        total += files[i].content_len + files[i].path_len + 80;
    }
    if (total > HU_INSTRUCTION_MAX_CHARS_TOTAL + 512)
        total = HU_INSTRUCTION_MAX_CHARS_TOTAL + 512;

    char *buf = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    size_t chars_used = 0;

    /* Files are expected in priority order (highest first: workspace, project, user) */
    for (size_t i = 0; i < file_count; i++) {
        if (!files[i].content || files[i].content_len == 0)
            continue;

        size_t remaining = HU_INSTRUCTION_MAX_CHARS_TOTAL - chars_used;
        if (remaining == 0)
            break;

        /* Section header */
        const char *source_label;
        switch (files[i].source) {
        case HU_INSTRUCTION_SOURCE_WORKSPACE:
            source_label = "Workspace";
            break;
        case HU_INSTRUCTION_SOURCE_PROJECT_ROOT:
            source_label = "Project";
            break;
        case HU_INSTRUCTION_SOURCE_USER_HOME:
            source_label = "User";
            break;
        default:
            source_label = "Unknown";
            break;
        }

        int hdr_len = snprintf(buf + pos, total + 1 - pos, "# %s instructions (%s)\n", source_label,
                               files[i].path);
        if (hdr_len > 0) {
            pos += (size_t)hdr_len;
            chars_used += (size_t)hdr_len;
        }

        /* Recalculate remaining after header */
        remaining = HU_INSTRUCTION_MAX_CHARS_TOTAL - chars_used;
        if (remaining == 0)
            continue;

        /* Copy content, respecting total limit */
        size_t copy_len = files[i].content_len;
        if (copy_len > remaining)
            copy_len = remaining;

        memcpy(buf + pos, files[i].content, copy_len);
        pos += copy_len;
        chars_used += copy_len;

        /* Separator */
        if (chars_used + 2 <= HU_INSTRUCTION_MAX_CHARS_TOTAL && pos + 2 <= total) {
            buf[pos++] = '\n';
            buf[pos++] = '\n';
            chars_used += 2;
        }
    }

    buf[pos] = '\0';

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

/* ── discovery ───────────────────────────────────────────────────────────── */

hu_error_t hu_instruction_discovery_run(hu_allocator_t *alloc, const char *workspace_dir,
                                        size_t workspace_dir_len,
                                        hu_instruction_discovery_t **out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    /* Allocate result */
    hu_instruction_discovery_t *disc =
        (hu_instruction_discovery_t *)alloc->alloc(alloc->ctx, sizeof(*disc));
    if (!disc)
        return HU_ERR_OUT_OF_MEMORY;
    memset(disc, 0, sizeof(*disc));

    /* Pre-allocate file array (max: 1 workspace + 10 walk + 1 user = 12) */
    size_t max_files = HU_INSTRUCTION_MAX_WALK_LEVELS + 2;
    disc->files = (hu_instruction_file_t *)alloc->alloc(alloc->ctx,
                                                        sizeof(hu_instruction_file_t) * max_files);
    if (!disc->files) {
        alloc->free(alloc->ctx, disc, sizeof(*disc));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(disc->files, 0, sizeof(hu_instruction_file_t) * max_files);
    disc->file_count = 0;

    ino_t visited_inodes[HU_INSTRUCTION_MAX_VISITED_INODES];
    size_t visited_count = 0;

    hu_error_t err;

    /* 1. Workspace-local: <workspace_dir>/.human.md */
    if (workspace_dir && workspace_dir_len > 0) {
        char ws_path[PATH_MAX];
        int n = snprintf(ws_path, sizeof(ws_path), "%.*s/.human.md", (int)workspace_dir_len,
                         workspace_dir);
        if (n > 0 && (size_t)n < sizeof(ws_path)) {
            char *canon = NULL;
            size_t canon_len = 0;
            err = hu_instruction_validate_path(alloc, ws_path, (size_t)n, &canon, &canon_len);
            if (err == HU_OK && canon) {
                struct stat st;
                if (stat(canon, &st) == 0 && S_ISREG(st.st_mode)) {
                    if (visited_count < HU_INSTRUCTION_MAX_VISITED_INODES) {
                        visited_inodes[visited_count++] = st.st_ino;
                    }
                    hu_instruction_file_t file;
                    err = hu_instruction_file_read(alloc, canon, HU_INSTRUCTION_SOURCE_WORKSPACE,
                                                   &file);
                    if (err == HU_OK) {
                        disc->files[disc->file_count++] = file;
                    }
                }
                alloc->free(alloc->ctx, canon, canon_len + 1);
            }
        }
    }

    /* 2. Walk upward from workspace_dir for HUMAN.md (max 10 levels) */
    if (workspace_dir && workspace_dir_len > 0) {
        char current[PATH_MAX];
        if (workspace_dir_len < sizeof(current)) {
            memcpy(current, workspace_dir, workspace_dir_len);
            current[workspace_dir_len] = '\0';

            /* Canonicalize starting directory */
            char resolved_start[PATH_MAX];
            if (realpath(current, resolved_start)) {
                strncpy(current, resolved_start, sizeof(current) - 1);
                current[sizeof(current) - 1] = '\0';
            }

            for (int level = 0; level < HU_INSTRUCTION_MAX_WALK_LEVELS; level++) {
                char check_path[PATH_MAX];
                int pn = snprintf(check_path, sizeof(check_path), "%s/HUMAN.md", current);
                if (pn <= 0 || (size_t)pn >= sizeof(check_path))
                    break;

                struct stat st;
                if (stat(check_path, &st) == 0 && S_ISREG(st.st_mode)) {
                    /* Symlink cycle detection */
                    if (!inode_visited(visited_inodes, visited_count, st.st_ino)) {
                        if (visited_count < HU_INSTRUCTION_MAX_VISITED_INODES) {
                            visited_inodes[visited_count++] = st.st_ino;
                        }
                        hu_instruction_file_t file;
                        err = hu_instruction_file_read(alloc, check_path,
                                                       HU_INSTRUCTION_SOURCE_PROJECT_ROOT, &file);
                        if (err == HU_OK) {
                            disc->files[disc->file_count++] = file;
                        }
                    }
                }

                /* Move to parent directory */
                char *last_slash = strrchr(current, '/');
                if (!last_slash || last_slash == current)
                    break; /* reached root */
                *last_slash = '\0';
            }
        }
    }

    /* 3. User-level: ~/.human/instructions.md */
    {
        char user_path[PATH_MAX];
        int n = hu_paths_state(user_path, sizeof(user_path), "instructions.md");
        if (n > 0 && (size_t)n < sizeof(user_path)) {
            char *canon = NULL;
            size_t canon_len = 0;
            err = hu_instruction_validate_path(alloc, user_path, (size_t)n, &canon, &canon_len);
            if (err == HU_OK && canon) {
                struct stat st;
                if (stat(canon, &st) == 0 && S_ISREG(st.st_mode)) {
                    if (!inode_visited(visited_inodes, visited_count, st.st_ino)) {
                        hu_instruction_file_t file;
                        err = hu_instruction_file_read(alloc, canon,
                                                       HU_INSTRUCTION_SOURCE_USER_HOME, &file);
                        if (err == HU_OK) {
                            disc->files[disc->file_count++] = file;
                        }
                    }
                }
                alloc->free(alloc->ctx, canon, canon_len + 1);
            }
        }
    }

    /* Merge all discovered files */
    err = hu_instruction_merge(alloc, disc->files, disc->file_count, &disc->merged_content,
                               &disc->merged_content_len);
    if (err != HU_OK) {
        hu_instruction_discovery_destroy(alloc, disc);
        *out = NULL;
        return err;
    }

    disc->last_check_time = (int64_t)time(NULL);
    *out = disc;
    return HU_OK;
}

bool hu_instruction_discovery_is_fresh(const hu_instruction_discovery_t *disc) {
    if (!disc)
        return false;

    int64_t now = (int64_t)time(NULL);
    if (now - disc->last_check_time > HU_INSTRUCTION_CACHE_TTL_SEC)
        return false;

    /* Check all file mtimes */
    for (size_t i = 0; i < disc->file_count; i++) {
        if (!disc->files[i].path)
            continue;
        struct stat st;
        if (stat(disc->files[i].path, &st) != 0)
            return false; /* file disappeared */
        if ((int64_t)st.st_mtime != disc->files[i].mtime)
            return false; /* file changed */
    }

    return true;
}

void hu_instruction_discovery_destroy(hu_allocator_t *alloc, hu_instruction_discovery_t *disc) {
    if (!alloc || !disc)
        return;

    for (size_t i = 0; i < disc->file_count; i++) {
        hu_instruction_file_t *f = &disc->files[i];
        if (f->path)
            alloc->free(alloc->ctx, f->path, f->path_len + 1);
        if (f->content)
            alloc->free(alloc->ctx, f->content, f->content_len + 1);
    }

    if (disc->files) {
        size_t max_files = HU_INSTRUCTION_MAX_WALK_LEVELS + 2;
        alloc->free(alloc->ctx, disc->files, sizeof(hu_instruction_file_t) * max_files);
    }

    if (disc->merged_content)
        alloc->free(alloc->ctx, disc->merged_content, disc->merged_content_len + 1);

    alloc->free(alloc->ctx, disc, sizeof(*disc));
}
