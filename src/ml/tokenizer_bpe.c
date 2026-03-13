/* BPE tokenizer for ML training — byte-pair encoding with train/save/load. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/ml/tokenizer_ml.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HBPE_MAGIC 0x45504248 /* "HBPE" in little-endian */
#define HBPE_VERSION 1
#define BPE_INITIAL_VOCAB 256
#define BPE_INITIAL_MERGE_CAP 256
#define BPE_INITIAL_VOCAB_CAP 512

struct hu_bpe_tokenizer {
    hu_allocator_t *alloc;
    uint8_t **vocab_bytes;
    size_t *vocab_lens;
    size_t vocab_size;
    size_t vocab_capacity;
    int32_t *merge_a;
    int32_t *merge_b;
    size_t merge_count;
    size_t merge_capacity;
    int32_t bos_token_id;
};

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static hu_error_t ensure_vocab_capacity(struct hu_bpe_tokenizer *tok, size_t need)
{
    if (need <= tok->vocab_capacity)
        return HU_OK;
    size_t cap = tok->vocab_capacity ? tok->vocab_capacity * 2 : BPE_INITIAL_VOCAB_CAP;
    while (cap < need)
        cap *= 2;
    size_t old_cap = tok->vocab_capacity;

    uint8_t **new_bytes = (uint8_t **)tok->alloc->realloc(
        tok->alloc->ctx, tok->vocab_bytes,
        old_cap * sizeof(uint8_t *),
        cap * sizeof(uint8_t *));
    if (!new_bytes)
        return HU_ERR_OUT_OF_MEMORY;
    tok->vocab_bytes = new_bytes;

    size_t *new_lens = (size_t *)tok->alloc->realloc(
        tok->alloc->ctx, tok->vocab_lens,
        old_cap * sizeof(size_t),
        cap * sizeof(size_t));
    if (!new_lens) {
        /* vocab_bytes already grew — update capacity to match so deinit frees correctly */
        tok->vocab_capacity = cap;
        return HU_ERR_OUT_OF_MEMORY;
    }
    tok->vocab_lens = new_lens;
    tok->vocab_capacity = cap;
    return HU_OK;
}

static hu_error_t ensure_merge_capacity(struct hu_bpe_tokenizer *tok, size_t need)
{
    if (need <= tok->merge_capacity)
        return HU_OK;
    size_t cap = tok->merge_capacity ? tok->merge_capacity * 2 : BPE_INITIAL_MERGE_CAP;
    while (cap < need)
        cap *= 2;
    size_t old_cap = tok->merge_capacity;

    int32_t *new_a = (int32_t *)tok->alloc->realloc(
        tok->alloc->ctx, tok->merge_a,
        old_cap * sizeof(int32_t),
        cap * sizeof(int32_t));
    if (!new_a)
        return HU_ERR_OUT_OF_MEMORY;
    tok->merge_a = new_a;

    int32_t *new_b = (int32_t *)tok->alloc->realloc(
        tok->alloc->ctx, tok->merge_b,
        old_cap * sizeof(int32_t),
        cap * sizeof(int32_t));
    if (!new_b) {
        tok->merge_capacity = cap;
        return HU_ERR_OUT_OF_MEMORY;
    }
    tok->merge_b = new_b;
    tok->merge_capacity = cap;
    return HU_OK;
}

/* ─── Create ────────────────────────────────────────────────────────────── */

hu_error_t hu_bpe_tokenizer_create(hu_allocator_t *alloc, hu_bpe_tokenizer_t **out)
{
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;

    struct hu_bpe_tokenizer *tok =
        (struct hu_bpe_tokenizer *)alloc->alloc(alloc->ctx, sizeof(struct hu_bpe_tokenizer));
    if (!tok)
        return HU_ERR_OUT_OF_MEMORY;

    tok->alloc = alloc;
    tok->vocab_bytes = NULL;
    tok->vocab_lens = NULL;
    tok->vocab_size = 0;
    tok->vocab_capacity = 0;
    tok->merge_a = NULL;
    tok->merge_b = NULL;
    tok->merge_count = 0;
    tok->merge_capacity = 0;
    tok->bos_token_id = -1;

    hu_error_t err = ensure_vocab_capacity(tok, BPE_INITIAL_VOCAB);
    if (err != HU_OK)
        goto fail;

    for (int i = 0; i < BPE_INITIAL_VOCAB; i++) {
        tok->vocab_bytes[i] = (uint8_t *)alloc->alloc(alloc->ctx, 1);
        if (!tok->vocab_bytes[i]) {
            err = HU_ERR_OUT_OF_MEMORY;
            goto fail;
        }
        tok->vocab_bytes[i][0] = (uint8_t)i;
        tok->vocab_lens[i] = 1;
    }
    tok->vocab_size = BPE_INITIAL_VOCAB;

    *out = (hu_bpe_tokenizer_t *)tok;
    return HU_OK;

fail:
    hu_bpe_tokenizer_deinit((hu_bpe_tokenizer_t *)tok);
    return err;
}

/* ─── Pair counting for training ─────────────────────────────────────────── */

typedef struct {
    int32_t a;
    int32_t b;
    size_t count;
} pair_count_t;

static int pair_find(pair_count_t *pairs, size_t n, int32_t a, int32_t b)
{
    for (size_t i = 0; i < n; i++)
        if (pairs[i].a == a && pairs[i].b == b)
            return (int)i;
    return -1;
}

static void pair_add_or_inc(pair_count_t **pairs, size_t *count, size_t *cap,
                            hu_allocator_t *alloc, int32_t a, int32_t b)
{
    int idx = pair_find(*pairs, *count, a, b);
    if (idx >= 0) {
        (*pairs)[idx].count++;
        return;
    }
    if (*count >= *cap) {
        size_t new_cap = *cap ? *cap * 2 : 256;
        pair_count_t *new_pairs = (pair_count_t *)alloc->realloc(
            alloc->ctx, *pairs,
            *cap * sizeof(pair_count_t),
            new_cap * sizeof(pair_count_t));
        if (!new_pairs)
            return;
        *pairs = new_pairs;
        *cap = new_cap;
    }
    (*pairs)[*count].a = a;
    (*pairs)[*count].b = b;
    (*pairs)[*count].count = 1;
    (*count)++;
}

static void count_pairs(const int32_t *ids, size_t n, pair_count_t **pairs,
                        size_t *pair_count, size_t *pair_cap,
                        hu_allocator_t *alloc)
{
    for (size_t i = 0; i + 1 < n; i++)
        pair_add_or_inc(pairs, pair_count, pair_cap, alloc, ids[i], ids[i + 1]);
}

static size_t find_max_pair(pair_count_t *pairs, size_t n)
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++)
        if (pairs[i].count > pairs[best].count)
            best = i;
    return best;
}

/* Replace all occurrences of (a,b) with new_id in ids, in-place. */
static void replace_pairs(int32_t *ids, size_t *n, int32_t a, int32_t b, int32_t new_id)
{
    size_t j = 0;
    for (size_t i = 0; i < *n; i++) {
        if (i + 1 < *n && ids[i] == a && ids[i + 1] == b) {
            ids[j++] = new_id;
            i++;
        } else {
            ids[j++] = ids[i];
        }
    }
    *n = j;
}

/* ─── Train ─────────────────────────────────────────────────────────────── */

hu_error_t hu_bpe_tokenizer_train(hu_bpe_tokenizer_t *tok, const char **texts,
                                  size_t texts_count, size_t vocab_size,
                                  const char *pattern)
{
    (void)pattern;
    if (!tok || !texts || vocab_size <= BPE_INITIAL_VOCAB)
        return HU_ERR_INVALID_ARGUMENT;

    struct hu_bpe_tokenizer *t = (struct hu_bpe_tokenizer *)tok;
    hu_allocator_t *alloc = t->alloc;

    /* Convert all texts to byte-level token sequences. */
    typedef struct {
        int32_t *ids;
        size_t len;
        size_t cap;
    } seq_t;
    seq_t *seqs = (seq_t *)alloc->alloc(alloc->ctx, texts_count * sizeof(seq_t));
    if (!seqs)
        return HU_ERR_OUT_OF_MEMORY;

    for (size_t i = 0; i < texts_count; i++) {
        const char *text = texts[i];
        size_t len = text ? strlen(text) : 0;
        seqs[i].ids = NULL;
        seqs[i].len = 0;
        seqs[i].cap = 0;

        if (len == 0)
            continue;

        size_t cap = len > 64 ? len * 2 : 64;
        int32_t *ids = (int32_t *)alloc->alloc(alloc->ctx, cap * sizeof(int32_t));
        if (!ids) {
            for (size_t k = 0; k < i; k++)
                alloc->free(alloc->ctx, seqs[k].ids, seqs[k].cap * sizeof(int32_t));
            alloc->free(alloc->ctx, seqs, texts_count * sizeof(seq_t));
            return HU_ERR_OUT_OF_MEMORY;
        }
        for (size_t k = 0; k < len; k++)
            ids[k] = (int32_t)(unsigned char)text[k];
        seqs[i].ids = ids;
        seqs[i].len = len;
        seqs[i].cap = cap;
    }

    /* Clear any existing merges and ensure we start from byte-level. */
    t->merge_count = 0;
    t->vocab_size = BPE_INITIAL_VOCAB;

    pair_count_t *pairs = NULL;
    size_t pair_count = 0;
    size_t pair_cap = 0;

    hu_error_t err = HU_OK;
    while (t->vocab_size < vocab_size) {
        pair_count = 0;
        for (size_t i = 0; i < texts_count; i++)
            count_pairs(seqs[i].ids, seqs[i].len, &pairs, &pair_count, &pair_cap, alloc);

        if (pair_count == 0)
            break;

        size_t best_idx = find_max_pair(pairs, pair_count);
        int32_t a = pairs[best_idx].a;
        int32_t b = pairs[best_idx].b;

        err = ensure_vocab_capacity(t, t->vocab_size + 1);
        if (err != HU_OK)
            break;

        err = ensure_merge_capacity(t, t->merge_count + 1);
        if (err != HU_OK)
            break;

        size_t len_a = t->vocab_lens[a];
        size_t len_b = t->vocab_lens[b];
        size_t new_len = len_a + len_b;
        uint8_t *new_bytes = (uint8_t *)alloc->alloc(alloc->ctx, new_len);
        if (!new_bytes) {
            err = HU_ERR_OUT_OF_MEMORY;
            break;
        }
        memcpy(new_bytes, t->vocab_bytes[a], len_a);
        memcpy(new_bytes + len_a, t->vocab_bytes[b], len_b);

        int32_t new_id = (int32_t)t->vocab_size;
        t->vocab_bytes[new_id] = new_bytes;
        t->vocab_lens[new_id] = new_len;
        t->vocab_size++;

        t->merge_a[t->merge_count] = a;
        t->merge_b[t->merge_count] = b;
        t->merge_count++;

        for (size_t i = 0; i < texts_count; i++)
            replace_pairs(seqs[i].ids, &seqs[i].len, a, b, new_id);
    }

    if (pairs)
        alloc->free(alloc->ctx, pairs, pair_cap * sizeof(pair_count_t));
    for (size_t i = 0; i < texts_count; i++)
        alloc->free(alloc->ctx, seqs[i].ids, seqs[i].cap * sizeof(int32_t));
    alloc->free(alloc->ctx, seqs, texts_count * sizeof(seq_t));

    return err;
}

/* ─── Encode ────────────────────────────────────────────────────────────── */

hu_error_t hu_bpe_tokenizer_encode(const hu_bpe_tokenizer_t *tok, const char *text,
                                   size_t text_len, int32_t **ids_out,
                                   size_t *ids_count)
{
    if (!tok || !text || !ids_out || !ids_count)
        return HU_ERR_INVALID_ARGUMENT;

    const struct hu_bpe_tokenizer *t = (const struct hu_bpe_tokenizer *)tok;
    hu_allocator_t *alloc = t->alloc;

    if (text_len == 0) {
        *ids_out = NULL;
        *ids_count = 0;
        return HU_OK;
    }

    size_t cap = text_len > 64 ? text_len * 2 : 64;
    int32_t *ids = (int32_t *)alloc->alloc(alloc->ctx, cap * sizeof(int32_t));
    if (!ids)
        return HU_ERR_OUT_OF_MEMORY;

    size_t n = 0;
    for (size_t i = 0; i < text_len; i++) {
        if (n >= cap) {
            size_t new_cap = cap * 2;
            int32_t *new_ids = (int32_t *)alloc->realloc(
                alloc->ctx, ids, cap * sizeof(int32_t), new_cap * sizeof(int32_t));
            if (!new_ids) {
                alloc->free(alloc->ctx, ids, cap * sizeof(int32_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            ids = new_ids;
            cap = new_cap;
        }
        ids[n++] = (int32_t)(unsigned char)text[i];
    }

    /* Apply merge rules in order (earlier = higher priority). */
    for (size_t m = 0; m < t->merge_count; m++) {
        int32_t a = t->merge_a[m];
        int32_t b = t->merge_b[m];
        int32_t new_id = (int32_t)(BPE_INITIAL_VOCAB + m);
        replace_pairs(ids, &n, a, b, new_id);
    }

    *ids_out = ids;
    *ids_count = n;
    return HU_OK;
}

/* ─── Decode ────────────────────────────────────────────────────────────── */

hu_error_t hu_bpe_tokenizer_decode(const hu_bpe_tokenizer_t *tok, const int32_t *ids,
                                   size_t ids_count, char **text_out,
                                   size_t *text_len_out)
{
    if (!tok || !ids || !text_out || !text_len_out)
        return HU_ERR_INVALID_ARGUMENT;

    const struct hu_bpe_tokenizer *t = (const struct hu_bpe_tokenizer *)tok;
    hu_allocator_t *alloc = t->alloc;

    if (ids_count == 0) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\0';
        *text_out = empty;
        *text_len_out = 0;
        return HU_OK;
    }

    size_t total = 0;
    for (size_t i = 0; i < ids_count; i++) {
        int32_t id = ids[i];
        if (id < 0 || (size_t)id >= t->vocab_size) {
            *text_out = NULL;
            *text_len_out = 0;
            return HU_ERR_INVALID_ARGUMENT;
        }
        total += t->vocab_lens[id];
    }

    char *out = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!out)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    for (size_t i = 0; i < ids_count; i++) {
        int32_t id = ids[i];
        size_t len = t->vocab_lens[id];
        memcpy(out + pos, t->vocab_bytes[id], len);
        pos += len;
    }
    out[pos] = '\0';

    *text_out = out;
    *text_len_out = pos;
    return HU_OK;
}

/* ─── Vocab size ─────────────────────────────────────────────────────────── */

size_t hu_bpe_tokenizer_vocab_size(const hu_bpe_tokenizer_t *tok)
{
    if (!tok)
        return 0;
    return ((const struct hu_bpe_tokenizer *)tok)->vocab_size;
}

/* ─── Token byte length ──────────────────────────────────────────────────── */

size_t hu_bpe_tokenizer_token_byte_length(const hu_bpe_tokenizer_t *tok,
                                          int32_t token_id)
{
    if (!tok)
        return 0;
    const struct hu_bpe_tokenizer *t = (const struct hu_bpe_tokenizer *)tok;
    if (token_id < 0 || (size_t)token_id >= t->vocab_size)
        return 0;
    return t->vocab_lens[(size_t)token_id];
}

/* ─── Save ──────────────────────────────────────────────────────────────── */

hu_error_t hu_bpe_tokenizer_save(const hu_bpe_tokenizer_t *tok, const char *path)
{
    if (!tok || !path)
        return HU_ERR_INVALID_ARGUMENT;

    const struct hu_bpe_tokenizer *t = (const struct hu_bpe_tokenizer *)tok;

    FILE *f = fopen(path, "wb");
    if (!f)
        return HU_ERR_IO;

    uint32_t magic = HBPE_MAGIC;
    uint32_t version = HBPE_VERSION;
    uint32_t vsz = (uint32_t)t->vocab_size;
    uint32_t mcount = (uint32_t)t->merge_count;

    if (fwrite(&magic, sizeof(magic), 1, f) != 1 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&vsz, sizeof(vsz), 1, f) != 1 ||
        fwrite(&mcount, sizeof(mcount), 1, f) != 1) {
        fclose(f);
        return HU_ERR_IO;
    }

    for (size_t i = 0; i < t->vocab_size; i++) {
        size_t len = t->vocab_lens[i];
        if (len > 65535)
            len = 65535;
        uint16_t len16 = (uint16_t)len;
        if (fwrite(&len16, sizeof(len16), 1, f) != 1 ||
            fwrite(t->vocab_bytes[i], 1, len, f) != len) {
            fclose(f);
            return HU_ERR_IO;
        }
    }

    for (size_t i = 0; i < t->merge_count; i++) {
        int32_t a = t->merge_a[i];
        int32_t b = t->merge_b[i];
        if (fwrite(&a, sizeof(a), 1, f) != 1 || fwrite(&b, sizeof(b), 1, f) != 1) {
            fclose(f);
            return HU_ERR_IO;
        }
    }

    fclose(f);
    return HU_OK;
}

/* ─── Load ──────────────────────────────────────────────────────────────── */

hu_error_t hu_bpe_tokenizer_load(hu_bpe_tokenizer_t *tok, const char *path)
{
    if (!tok || !path)
        return HU_ERR_INVALID_ARGUMENT;

    struct hu_bpe_tokenizer *t = (struct hu_bpe_tokenizer *)tok;
    hu_allocator_t *alloc = t->alloc;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;

    uint32_t magic, version, vsz, mcount;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1 ||
        fread(&vsz, sizeof(vsz), 1, f) != 1 ||
        fread(&mcount, sizeof(mcount), 1, f) != 1) {
        fclose(f);
        return HU_ERR_PARSE;
    }

    if (magic != HBPE_MAGIC || version != HBPE_VERSION) {
        fclose(f);
        return HU_ERR_PARSE;
    }

    /* Free existing vocab (except base 256 if we're replacing). We'll rebuild. */
    for (size_t i = 0; i < t->vocab_size; i++)
        alloc->free(alloc->ctx, t->vocab_bytes[i], t->vocab_lens[i]);
    t->vocab_size = 0;
    t->merge_count = 0;

    hu_error_t err = ensure_vocab_capacity(t, vsz);
    if (err != HU_OK) {
        fclose(f);
        return err;
    }
    err = ensure_merge_capacity(t, mcount);
    if (err != HU_OK) {
        fclose(f);
        return err;
    }

    for (size_t i = 0; i < vsz; i++) {
        uint16_t len16;
        if (fread(&len16, sizeof(len16), 1, f) != 1) {
            fclose(f);
            return HU_ERR_PARSE;
        }
        size_t len = len16;
        uint8_t *bytes = (uint8_t *)alloc->alloc(alloc->ctx, len);
        if (!bytes) {
            fclose(f);
            return HU_ERR_OUT_OF_MEMORY;
        }
        if (fread(bytes, 1, len, f) != len) {
            alloc->free(alloc->ctx, bytes, len);
            fclose(f);
            return HU_ERR_PARSE;
        }
        t->vocab_bytes[i] = bytes;
        t->vocab_lens[i] = len;
    }
    t->vocab_size = vsz;

    for (size_t i = 0; i < mcount; i++) {
        int32_t a, b;
        if (fread(&a, sizeof(a), 1, f) != 1 || fread(&b, sizeof(b), 1, f) != 1) {
            fclose(f);
            return HU_ERR_PARSE;
        }
        t->merge_a[i] = a;
        t->merge_b[i] = b;
    }
    t->merge_count = mcount;

    fclose(f);
    return HU_OK;
}

/* ─── Deinit ───────────────────────────────────────────────────────────── */

void hu_bpe_tokenizer_deinit(hu_bpe_tokenizer_t *tok)
{
    if (!tok)
        return;

    struct hu_bpe_tokenizer *t = (struct hu_bpe_tokenizer *)tok;
    hu_allocator_t *alloc = t->alloc;

    for (size_t i = 0; i < t->vocab_size; i++)
        alloc->free(alloc->ctx, t->vocab_bytes[i], t->vocab_lens[i]);

    if (t->vocab_bytes)
        alloc->free(alloc->ctx, t->vocab_bytes, t->vocab_capacity * sizeof(uint8_t *));
    if (t->vocab_lens)
        alloc->free(alloc->ctx, t->vocab_lens, t->vocab_capacity * sizeof(size_t));
    if (t->merge_a)
        alloc->free(alloc->ctx, t->merge_a, t->merge_capacity * sizeof(int32_t));
    if (t->merge_b)
        alloc->free(alloc->ctx, t->merge_b, t->merge_capacity * sizeof(int32_t));

    alloc->free(alloc->ctx, tok, sizeof(struct hu_bpe_tokenizer));
}
