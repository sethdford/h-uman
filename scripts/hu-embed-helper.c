/*
 * hu-embed-helper — diagnostic CLI over the local TF-IDF embedder.
 *
 * US-3.4 (Sprint 3): Reads text from argv (one argument) or stdin (when no
 * argument is provided) and writes a one-line JSON document containing the
 * embedding produced by hu_embedder_local_create.
 *
 * This is a DIAGNOSTIC tool. It is NOT consumed by the daemon. The daemon
 * already re-embeds memories at startup (see US-3.3). This helper exists so
 * users and operators can inspect what the local embedder makes of a piece
 * of text, and so embed-existing-memories.sh can populate the additive
 * `embeddings` table for inspection/validation.
 *
 * Output JSON shape (one line):
 *   {"embedder":"tfidf-local-v1","dimensions":384,
 *    "embedding":[0.0,0.0,...],"text_len":<bytes>}
 *
 * Exit codes:
 *   0  success
 *   1  invalid arguments / IO error
 *   2  embedder failure / allocation failure
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/vector.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HU_EMBED_HELPER_VERSION    "tfidf-local-v1"
#define HU_EMBED_HELPER_READ_CHUNK 4096u
#define HU_EMBED_HELPER_MAX_INPUT  (8u * 1024u * 1024u) /* 8 MiB safety cap */

/* Read all of stdin into a heap buffer. On success, *out is a NUL-terminated
 * buffer (caller frees) and *out_len is the byte length excluding NUL.
 * Returns 0 on success, non-zero on error. */
static int read_stdin_all(char **out, size_t *out_len) {
    size_t cap = HU_EMBED_HELPER_READ_CHUNK;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return -1;

    for (;;) {
        if (len + HU_EMBED_HELPER_READ_CHUNK + 1 > cap) {
            size_t new_cap = cap * 2;
            if (new_cap > HU_EMBED_HELPER_MAX_INPUT + 1)
                new_cap = HU_EMBED_HELPER_MAX_INPUT + 1;
            if (new_cap <= cap) {
                free(buf);
                return -2; /* input too large */
            }
            char *nb = (char *)realloc(buf, new_cap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
            cap = new_cap;
        }
        size_t want = cap - len - 1;
        size_t got = fread(buf + len, 1, want, stdin);
        len += got;
        if (got < want) {
            if (feof(stdin))
                break;
            if (ferror(stdin)) {
                free(buf);
                return -1;
            }
        }
        if (len >= HU_EMBED_HELPER_MAX_INPUT) {
            free(buf);
            return -2;
        }
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}

/* Write a single float to stdout with a stable representation. */
static void write_float(float v) {
    /* %g loses too much precision; use a fixed format that round-trips well. */
    printf("%.7g", (double)v);
}

static void write_json_string_escaped(const char *s, size_t len) {
    /* Minimal JSON string escape — used only for embedder version tag. */
    putchar('"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (c < 0x20)
                printf("\\u%04x", c);
            else
                putchar((int)c);
        }
    }
    putchar('"');
}

static void print_usage(FILE *out) {
    fputs("Usage: hu_embed_helper [TEXT]\n"
          "  TEXT  text to embed (optional). If omitted, reads from stdin.\n"
          "\n"
          "Writes one-line JSON to stdout containing the local TF-IDF\n"
          "embedding (384 dimensions). Diagnostic tool; the daemon does\n"
          "not consume this output.\n",
          out);
}

int main(int argc, char **argv) {
    if (argc > 2) {
        print_usage(stderr);
        return 1;
    }
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout);
        return 0;
    }

    char *owned_input = NULL;
    const char *text = NULL;
    size_t text_len = 0;

    if (argc == 2) {
        text = argv[1];
        text_len = strlen(argv[1]);
    } else {
        int rc = read_stdin_all(&owned_input, &text_len);
        if (rc != 0) {
            if (rc == -2)
                fprintf(stderr, "hu_embed_helper: input exceeds %u bytes\n",
                        HU_EMBED_HELPER_MAX_INPUT);
            else
                fprintf(stderr, "hu_embed_helper: failed to read stdin\n");
            free(owned_input);
            return 1;
        }
        text = owned_input;
    }

    hu_allocator_t alloc = hu_system_allocator();
    hu_embedder_t emb = hu_embedder_local_create(&alloc);
    if (!emb.ctx) {
        fprintf(stderr, "hu_embed_helper: failed to create embedder\n");
        free(owned_input);
        return 2;
    }

    hu_embedding_t out = {0};
    hu_error_t err = emb.vtable->embed(emb.ctx, &alloc, text, text_len, &out);
    if (err != HU_OK) {
        fprintf(stderr, "hu_embed_helper: embed failed (err=%d)\n", (int)err);
        emb.vtable->deinit(emb.ctx, &alloc);
        free(owned_input);
        return 2;
    }

    /* Emit one-line JSON. */
    fputs("{\"embedder\":", stdout);
    write_json_string_escaped(HU_EMBED_HELPER_VERSION, strlen(HU_EMBED_HELPER_VERSION));
    printf(",\"dimensions\":%zu,\"text_len\":%zu,\"embedding\":[", out.dim, text_len);
    for (size_t i = 0; i < out.dim; i++) {
        if (i > 0)
            putchar(',');
        write_float(out.values[i]);
    }
    fputs("]}\n", stdout);

    hu_embedding_free(&alloc, &out);
    emb.vtable->deinit(emb.ctx, &alloc);
    free(owned_input);
    return 0;
}
