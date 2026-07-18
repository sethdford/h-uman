#ifndef HU_CORE_FILE_H
#define HU_CORE_FILE_H

/*
 * hu_file — shared whole-file read helper.
 *
 * The fopen("rb") → fseek(END) → ftell → fseek(SET) → alloc(sz+1) →
 * fread → NUL-terminate pattern was duplicated across at least seven
 * modules (audio, config_merge, skill_trust, agent_definition,
 * external_judge_fixture, doc_ingest, voice_clone), each with slightly
 * different error handling. This helper is the single implementation;
 * call sites keep their own policy by mapping the returned error.
 *
 * The buffer is always allocated with room for a trailing NUL and
 * NUL-terminated, so text callers can treat it as a C string. Binary
 * callers use *out_len — embedded NULs in the content are preserved
 * and out_len is the true byte count (see the 2026-07-13 memory-loader
 * NUL-overflow incident for why the two must never be conflated).
 */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read an entire file into a freshly allocated, NUL-terminated buffer.
 *
 * @param alloc     allocator used for the result buffer (must be non-NULL)
 * @param path      filesystem path to read (must be non-NULL)
 * @param max_bytes maximum permitted file size in bytes; 0 = no limit.
 *                  Files strictly larger than max_bytes are rejected
 *                  before any allocation.
 * @param out       receives the buffer (size len + 1, NUL-terminated);
 *                  set to NULL on failure. Free with
 *                  alloc->free(alloc->ctx, buf, len + 1).
 * @param out_len   receives the content length in bytes (excluding the
 *                  NUL); may be NULL if the caller doesn't need it.
 * @return HU_OK on success (including empty files, which yield len 0
 *         and a buffer holding just the NUL);
 *         HU_ERR_INVALID_ARGUMENT for NULL alloc/path/out;
 *         HU_ERR_NOT_FOUND if the file does not exist;
 *         HU_ERR_LIMIT_REACHED if the file exceeds max_bytes;
 *         HU_ERR_OUT_OF_MEMORY if allocation fails;
 *         HU_ERR_IO for any other open/seek/read failure.
 */
hu_error_t hu_file_slurp(hu_allocator_t *alloc, const char *path, size_t max_bytes, char **out,
                         size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_CORE_FILE_H */
