#ifndef HU_CORE_STATE_FILE_H
#define HU_CORE_STATE_FILE_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Small-state persistence helpers shared by modules that keep a JSON state
 * file under ~/.human (somatic state, humanization bandit arms, ...). */

/* $HOME/.human/<filename>; returns buf on success, NULL when unavailable —
 * and always NULL under HU_IS_TEST so tests never touch the real home dir. */
const char *hu_state_file_default_path(const char *filename, char *buf, size_t cap);

/* Stage an atomic replace of path: open <path>.tmp for writing. Returns the
 * stream (tmp receives the staging path), or NULL on failure. */
FILE *hu_state_file_write_begin(const char *path, char *tmp, size_t tmp_cap);

/* Finish the staged write: fclose + atomic rename over path, so a crash
 * mid-write can never leave a torn file. write_ok=false (or any failure)
 * removes the staging file and returns HU_ERR_IO. */
hu_error_t hu_state_file_write_commit(FILE *f, bool write_ok, const char *tmp, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* HU_CORE_STATE_FILE_H */
