#include "human/memory/ingest.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/memory/inbox.h"
#include "human/multimodal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *ext;
    size_t ext_len;
    hu_ingest_file_type_t type;
} hu_ext_map_t;

static const hu_ext_map_t EXT_MAP[] = {
    {".txt", 4, HU_INGEST_TEXT},   {".md", 3, HU_INGEST_TEXT},    {".json", 5, HU_INGEST_TEXT},
    {".csv", 4, HU_INGEST_TEXT},   {".log", 4, HU_INGEST_TEXT},   {".xml", 4, HU_INGEST_TEXT},
    {".yaml", 5, HU_INGEST_TEXT},  {".yml", 4, HU_INGEST_TEXT},   {".c", 2, HU_INGEST_TEXT},
    {".h", 2, HU_INGEST_TEXT},     {".py", 3, HU_INGEST_TEXT},    {".js", 3, HU_INGEST_TEXT},
    {".ts", 3, HU_INGEST_TEXT},    {".rs", 3, HU_INGEST_TEXT},    {".go", 3, HU_INGEST_TEXT},
    {".html", 5, HU_INGEST_TEXT},  {".css", 4, HU_INGEST_TEXT},   {".png", 4, HU_INGEST_IMAGE},
    {".jpg", 4, HU_INGEST_IMAGE},  {".jpeg", 5, HU_INGEST_IMAGE}, {".gif", 4, HU_INGEST_IMAGE},
    {".webp", 5, HU_INGEST_IMAGE}, {".bmp", 4, HU_INGEST_IMAGE},  {".svg", 4, HU_INGEST_IMAGE},
    {".mp3", 4, HU_INGEST_AUDIO},  {".wav", 4, HU_INGEST_AUDIO},  {".ogg", 4, HU_INGEST_AUDIO},
    {".flac", 5, HU_INGEST_AUDIO}, {".m4a", 4, HU_INGEST_AUDIO},  {".aac", 4, HU_INGEST_AUDIO},
    {".mp4", 4, HU_INGEST_VIDEO},  {".webm", 5, HU_INGEST_VIDEO}, {".mov", 4, HU_INGEST_VIDEO},
    {".avi", 4, HU_INGEST_VIDEO},  {".mkv", 4, HU_INGEST_VIDEO},  {".pdf", 4, HU_INGEST_PDF},
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

hu_ingest_file_type_t hu_ingest_detect_type(const char *path, size_t path_len) {
    if (!path || path_len == 0)
        return HU_INGEST_UNKNOWN;

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
        return HU_INGEST_UNKNOWN;

    size_t ext_len = (size_t)((path + path_len) - dot);
    for (size_t i = 0; i < EXT_MAP_COUNT; i++) {
        if (ext_match_ci(dot, ext_len, EXT_MAP[i].ext, EXT_MAP[i].ext_len))
            return EXT_MAP[i].type;
    }
    return HU_INGEST_UNKNOWN;
}

hu_error_t hu_ingest_read_text(hu_allocator_t *alloc, const char *path, size_t path_len, char **out,
                               size_t *out_len) {
    if (!alloc || !path || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

#ifdef HU_IS_TEST
    (void)path_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    char path_buf[1024];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "rb");
    if (!f)
        return HU_ERR_IO;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > (long)HU_INBOX_MAX_FILE_SIZE) {
        fclose(f);
        return size <= 0 ? HU_ERR_IO : HU_ERR_INVALID_ARGUMENT;
    }

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';
    *out = buf;
    *out_len = nread;
    return HU_OK;
#endif
}

hu_error_t hu_ingest_file(hu_allocator_t *alloc, hu_memory_t *memory, const char *path,
                          size_t path_len) {
    if (!alloc || !memory || !memory->vtable || !path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_ingest_file_type_t type = hu_ingest_detect_type(path, path_len);
    if (type != HU_INGEST_TEXT)
        return HU_ERR_NOT_SUPPORTED;

    char *content = NULL;
    size_t content_len = 0;
    hu_error_t err = hu_ingest_read_text(alloc, path, path_len, &content, &content_len);
    if (err != HU_OK)
        return err;

    const char *filename = path;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') {
            filename = path + i;
            break;
        }
    }
    size_t fname_len = (size_t)((path + path_len) - filename);

    char *key = hu_sprintf(alloc, "ingest:%.*s", (int)fname_len, filename);
    if (!key) {
        alloc->free(alloc->ctx, content, content_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    char *source = hu_sprintf(alloc, "file://%.*s", (int)path_len, path);
    if (!source) {
        hu_str_free(alloc, key);
        alloc->free(alloc->ctx, content, content_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_DAILY};
    err = hu_memory_store_with_source(memory, key, strlen(key), content, content_len, &cat, NULL, 0,
                                      source, strlen(source));

    hu_str_free(alloc, source);
    hu_str_free(alloc, key);
    alloc->free(alloc->ctx, content, content_len + 1);
    return err;
}

hu_error_t hu_ingest_build_extract_prompt(hu_allocator_t *alloc, const char *filename,
                                          size_t filename_len, hu_ingest_file_type_t type,
                                          char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    const char *type_name = "file";
    switch (type) {
    case HU_INGEST_IMAGE:
        type_name = "image";
        break;
    case HU_INGEST_AUDIO:
        type_name = "audio";
        break;
    case HU_INGEST_VIDEO:
        type_name = "video";
        break;
    case HU_INGEST_PDF:
        type_name = "PDF";
        break;
    default:
        break;
    }

    char *buf = hu_sprintf(alloc,
                           "Extract the text content from this %s file.\n"
                           "Filename: %.*s\n"
                           "Return a JSON object: "
                           "{\"content\":\"extracted text\",\"summary\":\"1-2 sentence summary\"}",
                           type_name, (int)filename_len, filename ? filename : "");
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    *out = buf;
    *out_len = strlen(buf);
    return HU_OK;
}

static hu_error_t read_binary_file(hu_allocator_t *alloc, const char *path, size_t path_len,
                                   void **out, size_t *out_len) {
#ifdef HU_IS_TEST
    (void)alloc;
    (void)path;
    (void)path_len;
    (void)out;
    (void)out_len;
    return HU_ERR_NOT_SUPPORTED;
#else
    char path_buf[1024];
    if (path_len >= sizeof(path_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(path_buf, path, path_len);
    path_buf[path_len] = '\0';

    FILE *f = fopen(path_buf, "rb");
    if (!f)
        return HU_ERR_IO;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > (long)HU_INBOX_MAX_FILE_SIZE) {
        fclose(f);
        return size <= 0 ? HU_ERR_IO : HU_ERR_INVALID_ARGUMENT;
    }

    void *buf = alloc->alloc(alloc->ctx, (size_t)size);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    *out = buf;
    *out_len = nread;
    return HU_OK;
#endif
}

static const char *mime_for_type(hu_ingest_file_type_t type, const char *path, size_t path_len) {
    (void)path;
    (void)path_len;
    switch (type) {
    case HU_INGEST_IMAGE:
        return "image/png";
    case HU_INGEST_AUDIO:
        return "audio/wav";
    case HU_INGEST_VIDEO:
        return "video/mp4";
    case HU_INGEST_PDF:
        return "application/pdf";
    default:
        return "application/octet-stream";
    }
}

static void extract_filename(const char *path, size_t path_len, const char **fname,
                             size_t *fname_len) {
    *fname = path;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '/' || path[i - 1] == '\\') {
            *fname = path + i;
            break;
        }
    }
    *fname_len = (size_t)((path + path_len) - *fname);
}

/* Strip markdown code fences (```json ... ```) from LLM responses */
static void strip_md_json(const char *in, size_t in_len, const char **out, size_t *out_len) {
    *out = in;
    *out_len = in_len;
    if (!in || in_len < 7)
        return;
    const char *p = in;
    const char *end = in + in_len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    if (end - p < 3 || p[0] != '`' || p[1] != '`' || p[2] != '`')
        return;
    p += 3;
    while (p < end && *p != '\n' && *p != '\r')
        p++;
    while (p < end && (*p == '\n' || *p == '\r'))
        p++;
    const char *close = end;
    for (const char *s = end - 1; s >= p; s--) {
        if (*s == '`' && s >= p + 2 && s[-1] == '`' && s[-2] == '`') {
            close = s - 2;
            break;
        }
    }
    while (close > p &&
           (close[-1] == ' ' || close[-1] == '\t' || close[-1] == '\n' || close[-1] == '\r'))
        close--;
    *out = p;
    *out_len = (size_t)(close - p);
}

static hu_error_t store_extracted(hu_allocator_t *alloc, hu_memory_t *memory, const char *response,
                                  size_t response_len, const char *path, size_t path_len,
                                  const char *fname, size_t fname_len) {
    const char *json_str = NULL;
    size_t json_len = 0;
    strip_md_json(response, response_len, &json_str, &json_len);

    hu_json_value_t *json = NULL;
    hu_error_t err = hu_json_parse(alloc, json_str, json_len, &json);
    if (err != HU_OK || !json)
        return HU_ERR_PARSE;

    const char *content = hu_json_get_string(json, "content");
    if (!content || content[0] == '\0') {
        hu_json_free(alloc, json);
        return HU_ERR_PARSE;
    }

    char *key = hu_sprintf(alloc, "ingest:%.*s", (int)fname_len, fname);
    char *source = hu_sprintf(alloc, "file://%.*s", (int)path_len, path);

    if (!key || !source) {
        if (key)
            hu_str_free(alloc, key);
        if (source)
            hu_str_free(alloc, source);
        hu_json_free(alloc, json);
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_DAILY};
    err = hu_memory_store_with_source(memory, key, strlen(key), content, strlen(content), &cat,
                                      NULL, 0, source, strlen(source));

    hu_str_free(alloc, source);
    hu_str_free(alloc, key);
    hu_json_free(alloc, json);
    return err;
}

hu_error_t hu_ingest_file_with_provider(hu_allocator_t *alloc, hu_memory_t *memory,
                                        hu_provider_t *provider, const char *path, size_t path_len,
                                        const char *model, size_t model_len) {
    if (!alloc || !memory || !memory->vtable || !path || path_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_ingest_file_type_t type = hu_ingest_detect_type(path, path_len);

    if (type == HU_INGEST_TEXT)
        return hu_ingest_file(alloc, memory, path, path_len);

    if (!provider || !provider->vtable)
        return HU_ERR_NOT_SUPPORTED;

    const char *fname = NULL;
    size_t fname_len = 0;
    extract_filename(path, path_len, &fname, &fname_len);

    if (type == HU_INGEST_IMAGE) {
        if (!provider->vtable->chat)
            return HU_ERR_NOT_SUPPORTED;
        void *raw = NULL;
        size_t raw_len = 0;
        hu_error_t err = read_binary_file(alloc, path, path_len, &raw, &raw_len);
        if (err != HU_OK)
            return err;

        char *b64 = NULL;
        size_t b64_len = 0;
        err = hu_multimodal_encode_base64(alloc, raw, raw_len, &b64, &b64_len);
        alloc->free(alloc->ctx, raw, raw_len);
        if (err != HU_OK)
            return err;

        char *prompt = NULL;
        size_t prompt_len = 0;
        err = hu_ingest_build_extract_prompt(alloc, fname, fname_len, type, &prompt, &prompt_len);
        if (err != HU_OK) {
            alloc->free(alloc->ctx, b64, b64_len + 1);
            return err;
        }

        const char *mime = mime_for_type(type, path, path_len);

        hu_content_part_t parts[2] = {
            {.tag = HU_CONTENT_PART_TEXT, .data.text = {.ptr = prompt, .len = prompt_len}},
            {.tag = HU_CONTENT_PART_IMAGE_BASE64,
             .data.image_base64 = {.data = b64,
                                   .data_len = b64_len,
                                   .media_type = mime,
                                   .media_type_len = strlen(mime)}},
        };

        hu_chat_message_t msg = {.role = HU_ROLE_USER,
                                 .content = prompt,
                                 .content_len = prompt_len,
                                 .content_parts = parts,
                                 .content_parts_count = 2};
        hu_chat_request_t req = {
            .messages = &msg, .messages_count = 1, .temperature = 0.2, .max_tokens = 2048};

        hu_chat_response_t resp = {0};
        err = provider->vtable->chat(provider->ctx, alloc, &req, NULL, 0, 0.2, &resp);

        alloc->free(alloc->ctx, b64, b64_len + 1);
        hu_str_free(alloc, prompt);

        if (err != HU_OK)
            return err;

        if (resp.content && resp.content_len > 0) {
            err = store_extracted(alloc, memory, resp.content, resp.content_len, path, path_len,
                                  fname, fname_len);
        } else {
            err = HU_ERR_PARSE;
        }
        hu_chat_response_free(alloc, &resp);
        return err;
    }

    if (provider->vtable->chat_with_system) {
        char *prompt = NULL;
        size_t prompt_len = 0;
        hu_error_t err =
            hu_ingest_build_extract_prompt(alloc, fname, fname_len, type, &prompt, &prompt_len);
        if (err != HU_OK)
            return err;

        char *response = NULL;
        size_t response_len = 0;
        const char *sys = "Extract content and return JSON only.";
        err = provider->vtable->chat_with_system(provider->ctx, alloc, sys, 37, prompt, prompt_len,
                                                 model, model_len, 0.2, &response, &response_len);
        hu_str_free(alloc, prompt);

        if (err != HU_OK)
            return err;

        if (response && response_len > 0) {
            err = store_extracted(alloc, memory, response, response_len, path, path_len, fname,
                                  fname_len);
            alloc->free(alloc->ctx, response, response_len + 1);
        } else {
            err = HU_ERR_PARSE;
        }
        return err;
    }

    return HU_ERR_NOT_SUPPORTED;
}

void hu_ingest_result_deinit(hu_ingest_result_t *result, hu_allocator_t *alloc) {
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
