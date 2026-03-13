/* Binary token data loader with BOS-aligned packing. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/dataloader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#endif

struct hu_ml_dataloader {
    hu_allocator_t *alloc;
    char *data_dir;
    char *split;
    size_t batch_size;
    size_t seq_len;
    char **shard_paths;
    size_t shard_count;
    size_t current_shard;
    size_t current_offset;
    int epoch;
    int32_t *shard_data;
    size_t shard_tokens;
    FILE *current_file;
};

static int path_compare(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static hu_error_t load_shard(struct hu_ml_dataloader *dl)
{
    if (dl->current_file) {
        fclose(dl->current_file);
        dl->current_file = NULL;
    }
    if (dl->shard_data) {
        dl->alloc->free(dl->alloc->ctx, dl->shard_data,
                        dl->shard_tokens * sizeof(int32_t));
        dl->shard_data = NULL;
    }
    dl->shard_tokens = 0;
    dl->current_offset = 0;

    if (dl->current_shard >= dl->shard_count)
        return HU_OK;

#ifndef _WIN32
    const char *path = dl->shard_paths[dl->current_shard];
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    long file_size = ftell(f);
    if (file_size < 0 || (size_t)file_size % sizeof(int32_t) != 0) {
        fclose(f);
        return HU_ERR_IO;
    }
    rewind(f);

    size_t n = (size_t)file_size / sizeof(int32_t);
    int32_t *data = (int32_t *)dl->alloc->alloc(dl->alloc->ctx, n * sizeof(int32_t));
    if (!data) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (fread(data, sizeof(int32_t), n, f) != n) {
        dl->alloc->free(dl->alloc->ctx, data, n * sizeof(int32_t));
        fclose(f);
        return HU_ERR_IO;
    }
    fclose(f);

    dl->shard_data = data;
    dl->shard_tokens = n;
#else
    (void)dl;
#endif
    return HU_OK;
}

static hu_error_t advance_to_next_shard(struct hu_ml_dataloader *dl)
{
    hu_error_t err = load_shard(dl);
    if (err != HU_OK)
        return err;
    if (dl->current_shard < dl->shard_count && dl->shard_tokens == 0)
        return HU_ERR_IO;
    return HU_OK;
}

hu_error_t hu_ml_dataloader_create(hu_allocator_t *alloc, const char *data_dir,
                                   size_t batch_size, size_t seq_len,
                                   const char *split, hu_ml_dataloader_t **out)
{
    if (!alloc || !data_dir || !split || !out || batch_size == 0 || seq_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    int is_train = (strcmp(split, "train") == 0);
    int is_val = (strcmp(split, "val") == 0);
    if (!is_train && !is_val)
        return HU_ERR_INVALID_ARGUMENT;

#ifndef _WIN32
    DIR *d = opendir(data_dir);
    if (!d)
        return HU_ERR_IO;

    size_t cap = 64;
    char **paths = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    if (!paths) {
        closedir(d);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t count = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        size_t nlen = strlen(e->d_name);
        if (nlen < 4 || strcmp(e->d_name + nlen - 4, ".bin") != 0)
            continue;

        if (count >= cap) {
            size_t new_cap = cap * 2;
            char **new_paths = (char **)alloc->realloc(
                alloc->ctx, paths, cap * sizeof(char *), new_cap * sizeof(char *));
            if (!new_paths) {
                for (size_t i = 0; i < count; i++)
                    alloc->free(alloc->ctx, paths[i], strlen(paths[i]) + 1);
                alloc->free(alloc->ctx, paths, cap * sizeof(char *));
                closedir(d);
                return HU_ERR_OUT_OF_MEMORY;
            }
            paths = new_paths;
            cap = new_cap;
        }

        size_t dlen = strlen(data_dir);
        size_t need = dlen + 1 + nlen + 1;
        char *full = (char *)alloc->alloc(alloc->ctx, need);
        if (!full) {
            for (size_t i = 0; i < count; i++)
                alloc->free(alloc->ctx, paths[i], strlen(paths[i]) + 1);
            alloc->free(alloc->ctx, paths, cap * sizeof(char *));
            closedir(d);
            return HU_ERR_OUT_OF_MEMORY;
        }
        snprintf(full, need, "%s/%s", data_dir, e->d_name);
        paths[count++] = full;
    }
    closedir(d);

    if (count == 0) {
        alloc->free(alloc->ctx, paths, cap * sizeof(char *));
        return HU_ERR_NOT_FOUND;
    }

    qsort(paths, count, sizeof(char *), path_compare);

    size_t shard_start, shard_end;
    if (is_train) {
        shard_start = 0;
        shard_end = count > 1 ? count - 1 : 0;
    } else {
        shard_start = count - 1;
        shard_end = count;
    }

    size_t sel_count = shard_end - shard_start;
    if (sel_count == 0) {
        for (size_t i = 0; i < count; i++)
            alloc->free(alloc->ctx, paths[i], strlen(paths[i]) + 1);
        alloc->free(alloc->ctx, paths, cap * sizeof(char *));
        return HU_ERR_NOT_FOUND;
    }
    char **sel_paths = (char **)alloc->alloc(alloc->ctx, sel_count * sizeof(char *));
    if (!sel_paths) {
        for (size_t i = 0; i < count; i++)
            alloc->free(alloc->ctx, paths[i], strlen(paths[i]) + 1);
        alloc->free(alloc->ctx, paths, cap * sizeof(char *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < sel_count; i++)
        sel_paths[i] = paths[shard_start + i];
    for (size_t i = 0; i < count; i++) {
        if (i < shard_start || i >= shard_end)
            alloc->free(alloc->ctx, paths[i], strlen(paths[i]) + 1);
    }
    alloc->free(alloc->ctx, paths, cap * sizeof(char *));
#else
    (void)is_train;
    (void)is_val;
    return HU_ERR_NOT_SUPPORTED;
#endif

    size_t dlen = strlen(data_dir) + 1;
    size_t slen = strlen(split) + 1;
    char *data_dir_copy = (char *)alloc->alloc(alloc->ctx, dlen);
    if (!data_dir_copy) {
        for (size_t i = 0; i < sel_count; i++)
            alloc->free(alloc->ctx, sel_paths[i], strlen(sel_paths[i]) + 1);
        alloc->free(alloc->ctx, sel_paths, sel_count * sizeof(char *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(data_dir_copy, data_dir, dlen);

    char *split_copy = (char *)alloc->alloc(alloc->ctx, slen);
    if (!split_copy) {
        alloc->free(alloc->ctx, data_dir_copy, dlen);
        for (size_t i = 0; i < sel_count; i++)
            alloc->free(alloc->ctx, sel_paths[i], strlen(sel_paths[i]) + 1);
        alloc->free(alloc->ctx, sel_paths, sel_count * sizeof(char *));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(split_copy, split, slen);

    struct hu_ml_dataloader *dl =
        (struct hu_ml_dataloader *)alloc->alloc(alloc->ctx, sizeof(*dl));
    if (!dl) {
        alloc->free(alloc->ctx, split_copy, slen);
        alloc->free(alloc->ctx, data_dir_copy, dlen);
        for (size_t i = 0; i < sel_count; i++)
            alloc->free(alloc->ctx, sel_paths[i], strlen(sel_paths[i]) + 1);
        alloc->free(alloc->ctx, sel_paths, sel_count * sizeof(char *));
        return HU_ERR_OUT_OF_MEMORY;
    }

    dl->alloc = alloc;
    dl->data_dir = data_dir_copy;
    dl->split = split_copy;
    dl->batch_size = batch_size;
    dl->seq_len = seq_len;
    dl->shard_paths = sel_paths;
    dl->shard_count = sel_count;
    dl->current_shard = 0;
    dl->current_offset = 0;
    dl->epoch = 0;
    dl->shard_data = NULL;
    dl->shard_tokens = 0;
    dl->current_file = NULL;

    hu_error_t err = advance_to_next_shard(dl);
    if (err != HU_OK) {
        hu_ml_dataloader_deinit((hu_ml_dataloader_t *)dl);
        return err;
    }

    *out = (hu_ml_dataloader_t *)dl;
    return HU_OK;
}

hu_error_t hu_ml_dataloader_next(hu_ml_dataloader_t *dl_ptr, hu_ml_batch_t *batch)
{
    if (!dl_ptr || !batch)
        return HU_ERR_INVALID_ARGUMENT;

    struct hu_ml_dataloader *dl = (struct hu_ml_dataloader *)dl_ptr;
    hu_allocator_t *alloc = dl->alloc;

    size_t row_tokens = dl->seq_len + 1;
    size_t input_per_row = dl->seq_len;
    size_t target_per_row = dl->seq_len;

    int32_t *input_ids =
        (int32_t *)alloc->alloc(alloc->ctx, dl->batch_size * input_per_row * sizeof(int32_t));
    if (!input_ids)
        return HU_ERR_OUT_OF_MEMORY;

    int32_t *target_ids =
        (int32_t *)alloc->alloc(alloc->ctx, dl->batch_size * target_per_row * sizeof(int32_t));
    if (!target_ids) {
        alloc->free(alloc->ctx, input_ids, dl->batch_size * input_per_row * sizeof(int32_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t rows_filled = 0;
    int batch_epoch = dl->epoch;

#ifndef _WIN32
    while (rows_filled < dl->batch_size && dl->shard_count > 0) {
        if (!dl->shard_data || dl->current_offset + row_tokens > dl->shard_tokens) {
            dl->current_shard++;
            if (dl->current_shard >= dl->shard_count) {
                dl->epoch++;
                dl->current_shard = 0;
                batch_epoch = dl->epoch;
            }
            hu_error_t err = advance_to_next_shard(dl);
            if (err != HU_OK) {
                alloc->free(alloc->ctx, target_ids,
                            dl->batch_size * target_per_row * sizeof(int32_t));
                alloc->free(alloc->ctx, input_ids,
                            dl->batch_size * input_per_row * sizeof(int32_t));
                return err;
            }
            if (dl->shard_count == 0 || !dl->shard_data)
                break;
        }

        if (dl->current_offset + row_tokens > dl->shard_tokens)
            continue;

        int32_t *row = &dl->shard_data[dl->current_offset];
        for (size_t i = 0; i < input_per_row; i++)
            input_ids[rows_filled * input_per_row + i] = row[i];
        for (size_t i = 0; i < target_per_row; i++)
            target_ids[rows_filled * target_per_row + i] = row[i + 1];

        rows_filled++;
        dl->current_offset += row_tokens;
    }
#else
    (void)row_tokens;
    (void)input_per_row;
    (void)target_per_row;
    (void)batch_epoch;
#endif

    batch->input_ids = input_ids;
    batch->target_ids = target_ids;
    batch->batch_size = rows_filled;
    batch->alloc_rows = dl->batch_size;
    batch->seq_len = dl->seq_len;
    batch->epoch = batch_epoch;

    return HU_OK;
}

void hu_ml_dataloader_reset(hu_ml_dataloader_t *dl_ptr)
{
    if (!dl_ptr)
        return;

    struct hu_ml_dataloader *dl = (struct hu_ml_dataloader *)dl_ptr;
    dl->current_shard = 0;
    dl->current_offset = 0;
    dl->epoch = 0;
    advance_to_next_shard(dl);
}

void hu_ml_dataloader_deinit(hu_ml_dataloader_t *dl_ptr)
{
    if (!dl_ptr)
        return;

    struct hu_ml_dataloader *dl = (struct hu_ml_dataloader *)dl_ptr;
    hu_allocator_t *alloc = dl->alloc;

    if (dl->current_file) {
        fclose(dl->current_file);
        dl->current_file = NULL;
    }
    if (dl->shard_data) {
        alloc->free(alloc->ctx, dl->shard_data,
                    dl->shard_tokens * sizeof(int32_t));
        dl->shard_data = NULL;
    }
    if (dl->shard_paths) {
        for (size_t i = 0; i < dl->shard_count; i++)
            alloc->free(alloc->ctx, dl->shard_paths[i],
                        strlen(dl->shard_paths[i]) + 1);
        alloc->free(alloc->ctx, dl->shard_paths,
                    dl->shard_count * sizeof(char *));
    }
    if (dl->data_dir)
        alloc->free(alloc->ctx, dl->data_dir, strlen(dl->data_dir) + 1);
    if (dl->split)
        alloc->free(alloc->ctx, dl->split, strlen(dl->split) + 1);

    alloc->free(alloc->ctx, dl, sizeof(*dl));
}

void hu_ml_batch_free(hu_allocator_t *alloc, hu_ml_batch_t *batch)
{
    if (!alloc || !batch)
        return;

    size_t rows = batch->alloc_rows ? batch->alloc_rows : batch->batch_size;
    if (batch->input_ids) {
        alloc->free(alloc->ctx, batch->input_ids,
                    rows * batch->seq_len * sizeof(int32_t));
        batch->input_ids = NULL;
    }
    if (batch->target_ids) {
        alloc->free(alloc->ctx, batch->target_ids,
                    rows * batch->seq_len * sizeof(int32_t));
        batch->target_ids = NULL;
    }
    batch->batch_size = 0;
    batch->alloc_rows = 0;
    batch->seq_len = 0;
}
