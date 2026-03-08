#include "seaclaw/memory/ingest.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory/inbox.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *ext;
    size_t ext_len;
    sc_ingest_file_type_t type;
} sc_ext_map_t;

static const sc_ext_map_t EXT_MAP[] = {
    {".txt", 4, SC_INGEST_TEXT},   {".md", 3, SC_INGEST_TEXT},    {".json", 5, SC_INGEST_TEXT},
    {".csv", 4, SC_INGEST_TEXT},   {".log", 4, SC_INGEST_TEXT},   {".xml", 4, SC_INGEST_TEXT},
    {".yaml", 5, SC_INGEST_TEXT},  {".yml", 4, SC_INGEST_TEXT},   {".c", 2, SC_INGEST_TEXT},
    {".h", 2, SC_INGEST_TEXT},     {".py", 3, SC_INGEST_TEXT},    {".js", 3, SC_INGEST_TEXT},
    {".ts", 3, SC_INGEST_TEXT},    {".rs", 3, SC_INGEST_TEXT},    {".go", 3, SC_INGEST_TEXT},
    {".html", 5, SC_INGEST_TEXT},  {".css", 4, SC_INGEST_TEXT},   {".png", 4, SC_INGEST_IMAGE},
    {".jpg", 4, SC_INGEST_IMAGE},  {".jpeg", 5, SC_INGEST_IMAGE}, {".gif", 4, SC_INGEST_IMAGE},
    {".webp", 5, SC_INGEST_IMAGE}, {".bmp", 4, SC_INGEST_IMAGE},  {".svg", 4, SC_INGEST_IMAGE},
    {".mp3", 4, SC_INGEST_AUDIO},  {".wav", 4, SC_INGEST_AUDIO},  {".ogg", 4, SC_INGEST_AUDIO},
    {".flac", 5, SC_INGEST_AUDIO}, {".m4a", 4, SC_INGEST_AUDIO},  {".aac", 4, SC_INGEST_AUDIO},
    {".mp4", 4, SC_INGEST_VIDEO},  {".webm", 5, SC_INGEST_VIDEO}, {".mov", 4, SC_INGEST_VIDEO},
    {".avi", 4, SC_INGEST_VIDEO},  {".mkv", 4, SC_INGEST_VIDEO},  {".pdf", 4, SC_INGEST_PDF},
};

#define EXT_MAP_COUNT (sizeof(EXT_MAP) / sizeof(EXT_MAP[0]))

static bool ext_match_ci(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return false;
    }
    return true;
}

sc_ingest_file_type_t sc_ingest_detect_type(const char *path, size_t path_len) {
    if (!path || path_len == 0)
        return SC_INGEST_UNKNOWN;

    const char *dot = NULL;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '.') {
            dot = path + i - 1;
            break;
        }
        if (path[i - 1] == '/' || path[i - 1] == '\\')
            break;
    }
    if (!dot)
        return SC_INGEST_UNKNOWN;

    size_t ext_len = (size_t)((path + path_len) - dot);
    for (size_t i = 0; i < EXT_MAP_COUNT; i++) {
        if (ext_match_ci(dot, ext_len, EXT_MAP[i].ext, EXT_MAP[i].ext_len))
            return EXT_MAP[i].type;
    }
    return SC_INGEST_UNKNOWN;
}

sc_error_t sc_ingest_read_text(sc_allocator_t *alloc, const char *path, size_t path_len, char **out,
                               size_t *out_len) {
    if (!alloc || !path || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

#ifdef SC_IS_TEST
    (void)path_len;
    return SC_ERR_NOT_SUPPORTED;
#else
    char path_buf[1024];
    if (path_len >= sizeof(path_buf))
        return SC_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "rb");
    if (!f)
        return SC_ERR_IO;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > (long)SC_INBOX_MAX_FILE_SIZE) {
        fclose(f);
        return size <= 0 ? SC_ERR_IO : SC_ERR_INVALID_ARGUMENT;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return SC_ERR_OUT_OF_MEMORY;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';
    *out = buf;
    *out_len = nread;
    return SC_OK;
#endif
}

sc_error_t sc_ingest_file(sc_allocator_t *alloc, sc_memory_t *memory, const char *path,
                          size_t path_len) {
    if (!alloc || !memory || !memory->vtable || !path || path_len == 0)
        return SC_ERR_INVALID_ARGUMENT;

    sc_ingest_file_type_t type = sc_ingest_detect_type(path, path_len);
    if (type != SC_INGEST_TEXT)
        return SC_ERR_NOT_SUPPORTED;

    char *content = NULL;
    size_t content_len = 0;
    sc_error_t err = sc_ingest_read_text(alloc, path, path_len, &content, &content_len);
    if (err != SC_OK)
        return err;

    const char *filename = path;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') {
            filename = path + i;
            break;
        }
    }
    size_t fname_len = (size_t)((path + path_len) - filename);

    char *key = sc_sprintf(alloc, "ingest:%.*s", (int)fname_len, filename);
    if (!key) {
        alloc->free(alloc->ctx, content, content_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }

    char *source = sc_sprintf(alloc, "file://%.*s", (int)path_len, path);
    if (!source) {
        sc_str_free(alloc, key);
        alloc->free(alloc->ctx, content, content_len + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }

    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_DAILY};
    err = sc_memory_store_with_source(memory, key, strlen(key), content, content_len, &cat, NULL, 0,
                                      source, strlen(source));

    sc_str_free(alloc, source);
    sc_str_free(alloc, key);
    alloc->free(alloc->ctx, content, content_len + 1);
    return err;
}

sc_error_t sc_ingest_build_extract_prompt(sc_allocator_t *alloc, const char *filename,
                                          size_t filename_len, sc_ingest_file_type_t type,
                                          char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *type_name = "file";
    switch (type) {
    case SC_INGEST_IMAGE:
        type_name = "image";
        break;
    case SC_INGEST_AUDIO:
        type_name = "audio";
        break;
    case SC_INGEST_VIDEO:
        type_name = "video";
        break;
    case SC_INGEST_PDF:
        type_name = "PDF";
        break;
    default:
        break;
    }

    char *buf = sc_sprintf(alloc,
                           "Extract the text content from this %s file.\n"
                           "Filename: %.*s\n"
                           "Return a JSON object: "
                           "{\"content\":\"extracted text\",\"summary\":\"1-2 sentence summary\"}",
                           type_name, (int)filename_len, filename ? filename : "");
    if (!buf)
        return SC_ERR_OUT_OF_MEMORY;

    *out = buf;
    *out_len = strlen(buf);
    return SC_OK;
}

void sc_ingest_result_deinit(sc_ingest_result_t *result, sc_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    if (result->content)
        alloc->free(alloc->ctx, result->content, result->content_len + 1);
    if (result->summary)
        alloc->free(alloc->ctx, result->summary, result->summary_len + 1);
    if (result->source_path)
        alloc->free(alloc->ctx, result->source_path, result->source_path_len + 1);
    memset(result, 0, sizeof(*result));
}
