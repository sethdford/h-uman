#include "human/multimodal/document.h"
#include "human/core/string.h"
#include <ctype.h>
#include <string.h>

static int to_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool match_ext(const char *filename, size_t filename_len, const char *ext, size_t ext_len) {
    if (filename_len < ext_len + 1)
        return false;
    if (filename[filename_len - ext_len - 1] != '.')
        return false;
    for (size_t i = 0; i < ext_len; i++) {
        if (to_lower((unsigned char)filename[filename_len - ext_len + i]) != (unsigned char)ext[i])
            return false;
    }
    return true;
}

static const char *doc_type_name(hu_doc_type_t type) {
    switch (type) {
    case HU_DOC_PLAINTEXT:
        return "plaintext";
    case HU_DOC_MARKDOWN:
        return "markdown";
    case HU_DOC_JSON:
        return "json";
    case HU_DOC_CSV:
        return "csv";
    case HU_DOC_CODE:
        return "code";
    default:
        return "document";
    }
}

hu_doc_type_t hu_doc_detect_type(const char *filename, size_t filename_len) {
    if (!filename || filename_len == 0)
        return HU_DOC_UNKNOWN_TYPE;
    if (match_ext(filename, filename_len, "txt", 3))
        return HU_DOC_PLAINTEXT;
    if (match_ext(filename, filename_len, "md", 2) ||
        match_ext(filename, filename_len, "markdown", 8))
        return HU_DOC_MARKDOWN;
    if (match_ext(filename, filename_len, "json", 4))
        return HU_DOC_JSON;
    if (match_ext(filename, filename_len, "csv", 3))
        return HU_DOC_CSV;
    if (match_ext(filename, filename_len, "py", 2) || match_ext(filename, filename_len, "c", 1) ||
        match_ext(filename, filename_len, "h", 1) || match_ext(filename, filename_len, "js", 2) ||
        match_ext(filename, filename_len, "ts", 2) || match_ext(filename, filename_len, "rs", 2) ||
        match_ext(filename, filename_len, "go", 2))
        return HU_DOC_CODE;
    return HU_DOC_UNKNOWN_TYPE;
}

hu_error_t hu_doc_chunk(hu_allocator_t *alloc, const char *content, size_t content_len,
                        size_t chunk_size, size_t overlap, hu_doc_chunk_t **out,
                        size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    if (!content || chunk_size == 0)
        return HU_OK;

    if (overlap >= chunk_size)
        return HU_ERR_INVALID_ARGUMENT;

    size_t cap = 16;
    hu_doc_chunk_t *chunks =
        (hu_doc_chunk_t *)alloc->alloc(alloc->ctx, cap * sizeof(hu_doc_chunk_t));
    if (!chunks)
        return HU_ERR_OUT_OF_MEMORY;

    size_t count = 0;
    size_t pos = 0;

    while (pos < content_len) {
        size_t chunk_start = pos;
        size_t chunk_end = pos + chunk_size;
        if (chunk_end > content_len)
            chunk_end = content_len;

        size_t split_at = chunk_end;
        if (chunk_end < content_len) {
            size_t para = (size_t)-1;
            for (size_t i = chunk_start; i < chunk_end; i++) {
                if (content[i] == '\n' && i + 1 < content_len && content[i + 1] == '\n') {
                    para = i + 2;
                    break;
                }
            }
            if (para != (size_t)-1 && para > chunk_start)
                split_at = para;
            else {
                size_t nl = (size_t)-1;
                for (size_t i = chunk_end; i > chunk_start; i--) {
                    if (content[i - 1] == '\n') {
                        nl = i;
                        break;
                    }
                }
                if (nl != (size_t)-1)
                    split_at = nl;
            }
        }

        size_t len = split_at - chunk_start;
        char *dup = hu_strndup(alloc, content + chunk_start, len);
        if (!dup) {
            hu_doc_chunks_free(alloc, chunks, count);
            return HU_ERR_OUT_OF_MEMORY;
        }

        if (count >= cap) {
            size_t new_cap = cap * 2;
            hu_doc_chunk_t *n = (hu_doc_chunk_t *)alloc->realloc(
                alloc->ctx, chunks, cap * sizeof(hu_doc_chunk_t), new_cap * sizeof(hu_doc_chunk_t));
            if (!n) {
                alloc->free(alloc->ctx, dup, len + 1);
                hu_doc_chunks_free(alloc, chunks, count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            chunks = n;
            cap = new_cap;
        }

        chunks[count].content = dup;
        chunks[count].content_len = len;
        chunks[count].start_offset = chunk_start;
        chunks[count].end_offset = split_at;
        count++;

        if (split_at >= content_len)
            break;

        pos = split_at;
        if (overlap > 0 && pos > overlap)
            pos -= overlap;
    }

    *out = chunks;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_doc_build_extract_prompt(hu_allocator_t *alloc, const char *filename,
                                       size_t filename_len, hu_doc_type_t type, const char *chunk,
                                       size_t chunk_len, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *fname = filename && filename_len > 0 ? filename : "(unnamed)";
    size_t fname_len = filename && filename_len > 0 ? filename_len : 8;
    const char *type_str = doc_type_name(type);
    const char *chunk_str = chunk && chunk_len > 0 ? chunk : "";
    size_t chunk_str_len = chunk && chunk_len > 0 ? chunk_len : 0;

    char *prompt = hu_sprintf(alloc,
                              "Extract key facts, entities, and relationships from this %s "
                              "document (%.*s):\n\n%.*s\n\nReturn structured JSON with: "
                              "entities[], facts[], relationships[]",
                              type_str, (int)fname_len, fname, (int)chunk_str_len, chunk_str);
    if (!prompt)
        return HU_ERR_OUT_OF_MEMORY;
    *out = prompt;
    *out_len = strlen(prompt);
    return HU_OK;
}

void hu_doc_chunks_free(hu_allocator_t *alloc, hu_doc_chunk_t *chunks, size_t count) {
    if (!alloc || !chunks)
        return;
    for (size_t i = 0; i < count; i++) {
        if (chunks[i].content) {
            alloc->free(alloc->ctx, chunks[i].content, chunks[i].content_len + 1);
            chunks[i].content = NULL;
        }
    }
    alloc->free(alloc->ctx, chunks, count * sizeof(hu_doc_chunk_t));
}
