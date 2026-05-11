/* Data preparation utilities for ML training. */

#include "human/ml/prepare.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/ml.h"
#include "human/ml/tokenizer_ml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#endif

/* ─── hu_ml_prepare_tokenize_file ─────────────────────────────────────────── */

hu_error_t hu_ml_prepare_tokenize_file(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                       const char *input_path, const char *output_path) {
    if (!alloc || !tok || !input_path || !output_path)
        return HU_ERR_INVALID_ARGUMENT;

#ifndef _WIN32
    FILE *in = fopen(input_path, "rb");
    if (!in)
        return HU_ERR_IO;

    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return HU_ERR_IO;
    }
    long sz = ftell(in);
    if (sz < 0) {
        fclose(in);
        return HU_ERR_IO;
    }
    rewind(in);

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(in);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nread = fread(buf, 1, (size_t)sz, in);
    fclose(in);
    if (nread != (size_t)sz) {
        alloc->free(alloc->ctx, buf, (size_t)sz + 1);
        return HU_ERR_IO;
    }
    buf[nread] = '\0';

    int32_t *ids = NULL;
    size_t ids_count = 0;
    hu_error_t err = hu_bpe_tokenizer_encode(tok, buf, nread, &ids, &ids_count);
    alloc->free(alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK)
        return err;

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
        return HU_ERR_IO;
    }
    if (fwrite(ids, sizeof(int32_t), ids_count, out) != ids_count) {
        fclose(out);
        alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
        return HU_ERR_IO;
    }
    fclose(out);
    alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
    return HU_OK;
#else
    (void)alloc;
    (void)tok;
    (void)input_path;
    (void)output_path;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ─── hu_ml_prepare_tokenize_dir ──────────────────────────────────────────── */

hu_error_t hu_ml_prepare_tokenize_dir(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                      const char *input_dir, const char *output_dir) {
    if (!alloc || !tok || !input_dir || !output_dir)
        return HU_ERR_INVALID_ARGUMENT;

#ifndef _WIN32
    DIR *d = opendir(input_dir);
    if (!d)
        return HU_ERR_IO;

    hu_error_t last_err = HU_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        size_t nlen = strlen(e->d_name);
        if (nlen < 4 || strcmp(e->d_name + nlen - 4, ".txt") != 0)
            continue;

        char in_path[1024];
        char out_path[1024];
        char base[512];
        size_t base_len = nlen - 4;
        if (base_len >= sizeof(base))
            continue;
        memcpy(base, e->d_name, base_len);
        base[base_len] = '\0';

        int r1 = snprintf(in_path, sizeof(in_path), "%s/%s", input_dir, e->d_name);
        int r2 = snprintf(out_path, sizeof(out_path), "%s/%s.bin", output_dir, base);
        if (r1 < 0 || (size_t)r1 >= sizeof(in_path) || r2 < 0 || (size_t)r2 >= sizeof(out_path))
            continue;

        hu_error_t err = hu_ml_prepare_tokenize_file(alloc, tok, in_path, out_path);
        if (err != HU_OK)
            last_err = err;
    }
    closedir(d);
    return last_err;
#else
    (void)alloc;
    (void)tok;
    (void)input_dir;
    (void)output_dir;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ─── hu_ml_prepare_token_bytes ───────────────────────────────────────────── */

hu_error_t hu_ml_prepare_token_bytes(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                     int32_t **token_bytes_out, size_t *count) {
    if (!alloc || !tok || !token_bytes_out || !count)
        return HU_ERR_INVALID_ARGUMENT;

    size_t vsz = hu_bpe_tokenizer_vocab_size(tok);
    int32_t *bytes = (int32_t *)alloc->alloc(alloc->ctx, vsz * sizeof(int32_t));
    if (!bytes)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < vsz; i++) {
        size_t len = hu_bpe_tokenizer_token_byte_length(tok, (int32_t)i);
        bytes[i] = (int32_t)(len > (size_t)2147483647 ? 2147483647 : len);
    }

    *token_bytes_out = bytes;
    *count = vsz;
    return HU_OK;
}

/* ─── hu_ml_prepare_load_default_tokenizer ────────────────────────────────── */

hu_error_t hu_ml_prepare_load_default_tokenizer(hu_allocator_t *alloc, const char *data_dir,
                                                hu_bpe_tokenizer_t **out_tok,
                                                int32_t **out_token_bytes, size_t *out_count) {
    if (!alloc || !out_tok || !out_token_bytes || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_tok = NULL;
    *out_token_bytes = NULL;
    *out_count = 0;

    hu_bpe_tokenizer_t *tok = NULL;
    hu_error_t err = hu_bpe_tokenizer_create(alloc, &tok);
    if (err != HU_OK)
        return err;

    /* Try data_dir first so per-experiment vocab can shadow the global one. */
    char path[1024];
    int loaded = 0;
    if (data_dir && data_dir[0]) {
        int n = snprintf(path, sizeof(path), "%s/tokenizer.vocab", data_dir);
        if (n > 0 && (size_t)n < sizeof(path) &&
            hu_bpe_tokenizer_load(tok, path) == HU_OK) {
            loaded = 1;
        }
    }
    if (!loaded) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            int n = snprintf(path, sizeof(path), "%s/.human/models/tokenizer.vocab", home);
            if (n > 0 && (size_t)n < sizeof(path))
                (void)hu_bpe_tokenizer_load(tok, path);
        }
        /* On both failures, tok keeps its default 256-byte byte-level vocab —
         * every token is one byte, BPB is well-defined. */
    }

    int32_t *bytes = NULL;
    size_t n = 0;
    err = hu_ml_prepare_token_bytes(alloc, tok, &bytes, &n);
    if (err != HU_OK) {
        hu_bpe_tokenizer_deinit(tok);
        return err;
    }

    *out_tok = tok;
    *out_token_bytes = bytes;
    *out_count = n;
    return HU_OK;
}

/* ─── hu_ml_prepare_conversations ─────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

static hu_error_t append_text(hu_allocator_t *alloc, char **buf, size_t *buf_len, size_t *buf_cap,
                              const char *text, size_t text_len) {
    if (*buf_len + text_len + 1 > *buf_cap) {
        size_t new_cap = (*buf_cap == 0) ? 65536 : *buf_cap;
        while (new_cap < *buf_len + text_len + 1)
            new_cap *= 2;
        char *nb = (char *)alloc->alloc(alloc->ctx, new_cap);
        if (!nb)
            return HU_ERR_OUT_OF_MEMORY;
        if (*buf && *buf_len > 0)
            memcpy(nb, *buf, *buf_len);
        if (*buf)
            alloc->free(alloc->ctx, *buf, *buf_cap);
        *buf = nb;
        *buf_cap = new_cap;
    }
    memcpy(*buf + *buf_len, text, text_len);
    *buf_len += text_len;
    (*buf)[*buf_len] = '\0';
    return HU_OK;
}

static hu_error_t collect_imessage(hu_allocator_t *alloc, const char *chat_db_path, char **buf,
                                   size_t *buf_len, size_t *buf_cap, size_t *count) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(chat_db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT m.is_from_me, m.text FROM message m "
                      "WHERE m.text IS NOT NULL AND m.text != '' "
                      "ORDER BY m.date ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_OK;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int is_from_me = sqlite3_column_int(stmt, 0);
        const char *text = (const char *)sqlite3_column_text(stmt, 1);
        if (!text || text[0] == '\0')
            continue;
        size_t tlen = strlen(text);

        const char *prefix = is_from_me ? "<|user|>" : "<|other|>";
        size_t plen = strlen(prefix);
        static const char suffix[] = "<|end|>\n";

        append_text(alloc, buf, buf_len, buf_cap, prefix, plen);
        append_text(alloc, buf, buf_len, buf_cap, text, tlen);
        append_text(alloc, buf, buf_len, buf_cap, suffix, sizeof(suffix) - 1);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return HU_OK;
}

static hu_error_t collect_memories(hu_allocator_t *alloc, const char *memory_db_path, char **buf,
                                   size_t *buf_len, size_t *buf_cap, size_t *count) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(memory_db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT content FROM memories WHERE content IS NOT NULL "
                      "ORDER BY timestamp ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_OK;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *content = (const char *)sqlite3_column_text(stmt, 0);
        if (!content || content[0] == '\0')
            continue;
        size_t clen = strlen(content);

        static const char prefix[] = "<|memory|>";
        static const char suffix[] = "<|end|>\n";
        append_text(alloc, buf, buf_len, buf_cap, prefix, sizeof(prefix) - 1);
        append_text(alloc, buf, buf_len, buf_cap, content, clen);
        append_text(alloc, buf, buf_len, buf_cap, suffix, sizeof(suffix) - 1);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return HU_OK;
}

static hu_error_t collect_conversations(hu_allocator_t *alloc, const char *memory_db_path,
                                        char **buf, size_t *buf_len, size_t *buf_cap,
                                        size_t *count) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(memory_db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return HU_OK;

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT role, content FROM messages "
                      "WHERE content IS NOT NULL AND content != '' "
                      "ORDER BY id ASC";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_OK;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *role = (const char *)sqlite3_column_text(stmt, 0);
        const char *content = (const char *)sqlite3_column_text(stmt, 1);
        if (!role || !content || content[0] == '\0')
            continue;
        size_t clen = strlen(content);

        const char *prefix = (strcmp(role, "user") == 0) ? "<|user|>" : "<|assistant|>";
        size_t plen = strlen(prefix);
        static const char suffix[] = "<|end|>\n";

        append_text(alloc, buf, buf_len, buf_cap, prefix, plen);
        append_text(alloc, buf, buf_len, buf_cap, content, clen);
        append_text(alloc, buf, buf_len, buf_cap, suffix, sizeof(suffix) - 1);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return HU_OK;
}
#endif

hu_error_t hu_ml_prepare_conversations(hu_allocator_t *alloc, hu_bpe_tokenizer_t *tok,
                                       const char *chat_db_path, const char *memory_db_path,
                                       const char *output_dir, size_t *messages_processed) {
    if (!alloc || !tok || !output_dir || !messages_processed)
        return HU_ERR_INVALID_ARGUMENT;
    *messages_processed = 0;

#ifdef HU_ENABLE_SQLITE
    char *buf = NULL;
    size_t buf_len = 0;
    size_t buf_cap = 0;

    if (chat_db_path)
        collect_imessage(alloc, chat_db_path, &buf, &buf_len, &buf_cap, messages_processed);
    if (memory_db_path) {
        collect_memories(alloc, memory_db_path, &buf, &buf_len, &buf_cap, messages_processed);
        collect_conversations(alloc, memory_db_path, &buf, &buf_len, &buf_cap, messages_processed);
    }

    if (buf_len == 0) {
        if (buf)
            alloc->free(alloc->ctx, buf, buf_cap);
        return HU_OK;
    }

    int32_t *ids = NULL;
    size_t ids_count = 0;
    hu_error_t err = hu_bpe_tokenizer_encode(tok, buf, buf_len, &ids, &ids_count);
    alloc->free(alloc->ctx, buf, buf_cap);
    if (err != HU_OK)
        return err;

    if (ids_count == 0) {
        if (ids)
            alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
        return HU_OK;
    }

    /* 90/10 train/val split */
    size_t val_start = ids_count * 9 / 10;
    if (val_start == 0)
        val_start = 1;
    if (val_start >= ids_count)
        val_start = ids_count - 1;

    char train_path[1024];
    char val_path[1024];
    snprintf(train_path, sizeof(train_path), "%s/train.bin", output_dir);
    snprintf(val_path, sizeof(val_path), "%s/val.bin", output_dir);

    FILE *tf = fopen(train_path, "wb");
    if (tf) {
        fwrite(ids, sizeof(int32_t), val_start, tf);
        fclose(tf);
    }
    FILE *vf = fopen(val_path, "wb");
    if (vf) {
        fwrite(ids + val_start, sizeof(int32_t), ids_count - val_start, vf);
        fclose(vf);
    }

    printf("[prepare] Tokenized %zu messages -> %zu tokens (train=%zu, val=%zu)\n",
           *messages_processed, ids_count, val_start, ids_count - val_start);

    alloc->free(alloc->ctx, ids, ids_count * sizeof(int32_t));
    return (tf && vf) ? HU_OK : HU_ERR_IO;
#else
    (void)chat_db_path;
    (void)memory_db_path;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

/* ─── hu_experiment_config_default ─────────────────────────────────────────── */

hu_experiment_config_t hu_experiment_config_default(void) {
    hu_experiment_config_t c = {0};

    c.gpt.sequence_len = 2048;
    c.gpt.vocab_size = 8192;
    c.gpt.n_layer = 8;
    c.gpt.n_head = 4;
    c.gpt.n_kv_head = 4;
    c.gpt.n_embd = 512;
    c.gpt.head_dim = 128;
    memcpy(c.gpt.window_pattern, "SSSL", 5);
    c.gpt.activation = HU_ML_ACT_RELU_SQ;
    c.gpt.logit_soft_cap = 30.0f;

    c.optimizer.embedding_lr = 0.6f;
    c.optimizer.unembedding_lr = 0.004f;
    c.optimizer.matrix_lr = 0.04f;
    c.optimizer.scalar_lr = 0.5f;
    c.optimizer.weight_decay = 0.2f;
    c.optimizer.adam_beta1 = 0.8f;
    c.optimizer.adam_beta2 = 0.95f;
    c.optimizer.warmup_ratio = 0.0f;
    c.optimizer.warmdown_ratio = 0.5f;
    c.optimizer.final_lr_frac = 0.0f;

    c.training.device_batch_size = 128;
    c.training.time_budget_secs = 300;
    c.training.eval_tokens = 20971520;

    c.backend = HU_ML_BACKEND_CPU;

    return c;
}
