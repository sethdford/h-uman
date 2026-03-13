/* Generic JSONL file ingest feed. F98.
 * Reads .jsonl files from ~/.human/feeds/ingest/, parses each line as a
 * JSON object with: source, content_type, content, url (optional).
 * Files are renamed to .done after processing. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/file_ingest.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/feeds/ingest.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INGEST_DIR_SUFFIX "/.human/feeds/ingest"
#define MAX_LINE_LEN 4096

#if HU_IS_TEST

hu_error_t hu_file_ingest_fetch(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    (void)alloc;
    if (!items || !out_count || items_cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(items, 0, sizeof(hu_feed_ingest_item_t) * 2);
    (void)strncpy(items[0].source, "facebook", sizeof(items[0].source) - 1);
    (void)strncpy(items[0].content_type, "post", sizeof(items[0].content_type) - 1);
    (void)strncpy(items[0].content,
        "AI agents are transforming software development workflows",
        sizeof(items[0].content) - 1);
    items[0].content_len = strlen(items[0].content);
    items[0].ingested_at = (int64_t)time(NULL);
    (void)strncpy(items[1].source, "tiktok", sizeof(items[1].source) - 1);
    (void)strncpy(items[1].content_type, "video_caption", sizeof(items[1].content_type) - 1);
    (void)strncpy(items[1].content,
        "Coding with AI: building a full app in 10 minutes using autonomous agents",
        sizeof(items[1].content) - 1);
    items[1].content_len = strlen(items[1].content);
    items[1].ingested_at = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else

static int ends_with_jsonl(const char *name) {
    size_t len = strlen(name);
    return len > 6 && strcmp(name + len - 6, ".jsonl") == 0;
}

static void parse_jsonl_line(hu_allocator_t *alloc, const char *line, size_t line_len,
    hu_feed_ingest_item_t *item) {
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, line, line_len, &root) != HU_OK || !root)
        return;

    const char *src = hu_json_get_string(root, "source");
    const char *ctype = hu_json_get_string(root, "content_type");
    const char *content = hu_json_get_string(root, "content");
    const char *url = hu_json_get_string(root, "url");

    if (src)
        snprintf(item->source, sizeof(item->source), "%s", src);
    if (ctype)
        snprintf(item->content_type, sizeof(item->content_type), "%s", ctype);
    if (content) {
        snprintf(item->content, sizeof(item->content), "%s", content);
        item->content_len = strlen(item->content);
    }
    if (url)
        snprintf(item->url, sizeof(item->url), "%s", url);
    item->ingested_at = (int64_t)time(NULL);

    hu_json_free(alloc, root);
}

hu_error_t hu_file_ingest_fetch(hu_allocator_t *alloc,
    hu_feed_ingest_item_t *items, size_t items_cap, size_t *out_count) {
    if (!alloc || !items || !out_count || items_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    const char *home = getenv("HOME");
    if (!home)
        return HU_ERR_NOT_FOUND;

    char dir_path[512];
    int n = snprintf(dir_path, sizeof(dir_path), "%s%s", home, INGEST_DIR_SUFFIX);
    if (n <= 0 || (size_t)n >= sizeof(dir_path))
        return HU_ERR_INVALID_ARGUMENT;

    DIR *dir = opendir(dir_path);
    if (!dir)
        return HU_OK;

    size_t cnt = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && cnt < items_cap) {
        if (!ends_with_jsonl(ent->d_name))
            continue;

        char file_path[768];
        snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, ent->d_name);

        FILE *fp = fopen(file_path, "r");
        if (!fp)
            continue;

        char line[MAX_LINE_LEN];
        while (fgets(line, sizeof(line), fp) && cnt < items_cap) {
            size_t line_len = strlen(line);
            while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
                line[--line_len] = '\0';
            if (line_len == 0)
                continue;

            memset(&items[cnt], 0, sizeof(items[cnt]));
            parse_jsonl_line(alloc, line, line_len, &items[cnt]);
            if (items[cnt].content_len > 0)
                cnt++;
        }
        fclose(fp);

        /* Rename to .done to avoid re-processing */
        char done_path[768];
        snprintf(done_path, sizeof(done_path), "%s.done", file_path);
        (void)rename(file_path, done_path);
    }

    closedir(dir);
    *out_count = cnt;
    return HU_OK;
}

#endif /* HU_IS_TEST */

#else
typedef int hu_file_ingest_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */
