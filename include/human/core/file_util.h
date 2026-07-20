#ifndef HU_CORE_FILE_UTIL_H
#define HU_CORE_FILE_UTIL_H

/*
 * hu_file_util — whole-file read helper.
 *
 * The fopen / fseek(SEEK_END) / ftell / fseek(SEEK_SET) / fread
 * "slurp a file into an allocator buffer" preamble was hand-rolled in
 * half a dozen tools (doc_ingest, file_edit, pdf, image, computer_use,
 * meeting_transcribe), forming clone groups counted by
 * scripts/check-clone-ratchet.sh. This helper is the single canonical
 * implementation; new code must call it instead of re-rolling the
 * preamble.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the entire regular file at `path` into a buffer obtained from
 * `alloc`. The buffer is NUL-terminated; *out_len is the byte count
 * EXCLUDING the terminator. The caller frees with
 *
 *     alloc->free(alloc->ctx, *out_data, *out_len + 1);
 *
 * `max_bytes` caps the accepted file size (strictly greater rejects);
 * pass 0 for no cap.
 *
 * Errors:
 *   HU_ERR_INVALID_ARGUMENT  NULL alloc/path/out_data/out_len
 *   HU_ERR_NOT_FOUND         fopen failed (missing or unreadable file)
 *   HU_ERR_INVALID_FORMAT    file is empty, or larger than max_bytes
 *   HU_ERR_IO                seek/tell failed, or short read
 *   HU_ERR_OUT_OF_MEMORY     allocation failed
 *
 * On any error *out_data is NULL and *out_len is 0. */
hu_error_t hu_file_read_all(hu_allocator_t *alloc, const char *path, size_t max_bytes,
                            char **out_data, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_CORE_FILE_UTIL_H */
