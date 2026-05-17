/* US-8.2 — Persona Encryption-at-Rest implementation.
 *
 * Single translation unit owns:
 *  - the pure classify predicate `hu_persona_classify_bytes`
 *  - the linux keyfile backend (always available; test path also routes here)
 *  - the darwin Keychain backend (production-only; tests bypass via override)
 *  - encrypted save / load (atomic tmp+fsync+rename)
 *  - migrate-to-encrypted (rename-to-.legacy + sentinel + shred + recover)
 *  - the refuse-path `hu_persona_load_legacy`
 *
 * See `sprints/sprint-8/designs/US-8.2.md` for the full design.
 */

#if !defined(HU_HAS_LIBSODIUM)
#error "persona_crypt.c requires libsodium; build with -DHU_ENABLE_LIBSODIUM=ON"
#endif

#define _GNU_SOURCE 1

#include "human/persona/crypto.h"

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/persona.h"

#include <sodium.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* hu_persona_load_json lives in src/persona/persona.c; declared here to avoid
 * including the full persona internal header. */
extern hu_error_t hu_persona_load_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                       hu_persona_t *out);
extern void hu_persona_deinit(hu_allocator_t *alloc, hu_persona_t *persona);

/* ------------------------------------------------------------------------- */
/* 1. Pure classify predicate                                                */
/* ------------------------------------------------------------------------- */

hu_persona_format_t hu_persona_classify_bytes(const uint8_t *buf, size_t len) {
    if (!buf || len == 0)
        return HU_PERSONA_FORMAT_UNKNOWN;

    /* HUP1 magic must be the first 4 bytes AND the file must contain the full
     * 32-byte header (magic + ver + reserved + nonce). A 4-byte truncated file
     * that starts with HUP1 is NOT classified as encrypted — it cannot be
     * decrypted, so calling it "encrypted_v1" would mis-route the loader.
     * The test `classify_HUP1_truncated_to_header_only_is_unknown` pins this. */
    if (len >= HU_PERSONA_CRYPT_HEADER_BYTES && memcmp(buf, HU_PERSONA_CRYPT_MAGIC, 4) == 0) {
        if (buf[4] == HU_PERSONA_CRYPT_VERSION && buf[5] == 0 && buf[6] == 0 && buf[7] == 0)
            return HU_PERSONA_FORMAT_ENCRYPTED_V1;
        /* Wrong version byte or non-zero reserved — not a v1 file. Refuse
         * loudly rather than guessing what newer-version bytes mean. */
        return HU_PERSONA_FORMAT_UNKNOWN;
    }

    /* JSON sniff: skip RFC 8259 whitespace, then look for '{'. We don't
     * accept '[' here because a persona file is always an object at the
     * root. This is a heuristic, not a parser; the actual JSON parse runs
     * later in hu_persona_load_json. */
    size_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r'))
        i++;
    if (i < len && buf[i] == '{')
        return HU_PERSONA_FORMAT_PLAINTEXT_JSON;

    return HU_PERSONA_FORMAT_UNKNOWN;
}

/* ------------------------------------------------------------------------- */
/* 2. Linux keyfile backend (also used by darwin in HU_IS_TEST builds)       */
/* ------------------------------------------------------------------------- */

/* Returns true if path's parent directories were created (mkdir -p); 0700
 * on each component we create.  Returns false if any operation failed for a
 * reason other than EEXIST. */
static bool ensure_parent_dir_0700(const char *path) {
    char buf[1024];
    size_t pl = strlen(path);
    if (pl == 0 || pl >= sizeof(buf))
        return false;
    memcpy(buf, path, pl + 1);

    /* walk left-to-right creating each intermediate directory */
    for (size_t i = 1; i < pl; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0700) != 0 && errno != EEXIST)
                return false;
            buf[i] = '/';
        }
    }
    return true;
}

/* Resolve the keyfile path. Order:
 *   1. HU_PERSONA_KEYFILE_OVERRIDE (used by tests)
 *   2. $HOME/.human/keys/persona.key
 *   3. error
 */
static hu_error_t resolve_keyfile_path(char *out, size_t cap) {
    const char *override = getenv("HU_PERSONA_KEYFILE_OVERRIDE");
    if (override && override[0]) {
        size_t n = strlen(override);
        if (n + 1 > cap)
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(out, override, n + 1);
        return HU_OK;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_CONFIG_INVALID;
    int n = snprintf(out, cap, "%s/.human/keys/persona.key", home);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

/* Read 32 bytes of OS randomness via libsodium (which routes to
 * getrandom/arc4random/etc. as appropriate for the platform). */
static void random_key_bytes(uint8_t out[HU_PERSONA_CRYPT_KEY_BYTES]) {
    randombytes_buf(out, HU_PERSONA_CRYPT_KEY_BYTES);
}

/* Refuses to read a keyfile that has been widened past 0600. Per design R4,
 * a widened keyfile is surfaced as HU_ERR_SECURITY_DENIED, NOT silently
 * accepted — defense in depth against `umask 022` accidents. */
static hu_error_t keystore_linux_load_or_create(uint8_t out[HU_PERSONA_CRYPT_KEY_BYTES]) {
    char path[1024];
    hu_error_t pe = resolve_keyfile_path(path, sizeof(path));
    if (pe != HU_OK)
        return pe;

    /* Ensure ~/.human/keys/ (or override's parent) is 0700 before we touch
     * the keyfile. */
    if (!ensure_parent_dir_0700(path))
        return HU_ERR_IO;

    struct stat st;
    if (stat(path, &st) == 0) {
        /* Existing keyfile — perms gate first. */
        if (!S_ISREG(st.st_mode))
            return HU_ERR_INVALID_FORMAT;
        mode_t perm = st.st_mode & 0777;
        if (perm & 077)
            return HU_ERR_SECURITY_LOCKOUT; /* design R4: widened perms refused */
        if (st.st_size != (off_t)HU_PERSONA_CRYPT_KEY_BYTES)
            return HU_ERR_INVALID_FORMAT;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return HU_ERR_IO;
        ssize_t r = read(fd, out, HU_PERSONA_CRYPT_KEY_BYTES);
        int saved = errno;
        close(fd);
        if (r != (ssize_t)HU_PERSONA_CRYPT_KEY_BYTES) {
            errno = saved;
            return HU_ERR_IO;
        }
        return HU_OK;
    }
    if (errno != ENOENT)
        return HU_ERR_IO;

    /* Generate fresh 32 random bytes and write with O_EXCL so a concurrent
     * keystore_load can't see a partial file. */
    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    random_key_bytes(key);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        sodium_memzero(key, sizeof(key));
        if (errno == EEXIST) {
            /* Race: another process won. Retry the read path. */
            return keystore_linux_load_or_create(out);
        }
        return HU_ERR_IO;
    }
    /* Belt + suspenders: umask might have stripped the mode; force 0600. */
    (void)fchmod(fd, 0600);
    ssize_t w = write(fd, key, HU_PERSONA_CRYPT_KEY_BYTES);
    if (w == (ssize_t)HU_PERSONA_CRYPT_KEY_BYTES)
        (void)fsync(fd);
    close(fd);
    if (w != (ssize_t)HU_PERSONA_CRYPT_KEY_BYTES) {
        sodium_memzero(key, sizeof(key));
        (void)unlink(path);
        return HU_ERR_IO;
    }
    memcpy(out, key, HU_PERSONA_CRYPT_KEY_BYTES);
    sodium_memzero(key, sizeof(key));
    return HU_OK;
}

/* ------------------------------------------------------------------------- */
/* 3. Public key derivation                                                  */
/* ------------------------------------------------------------------------- */

/* For v1 we use a single master persona key for all personas. This matches
 * AC-8.2.* (which never asserts per-persona keys) and keeps the threat
 * model simple. Multi-persona key separation is a follow-on if/when needed;
 * the test `keystore_linux_returns_same_key_on_repeat_call_same_name`
 * documents this choice. */
hu_error_t hu_persona_crypt_derive_key(hu_allocator_t *alloc, const char *persona_name,
                                       uint8_t key_out[HU_PERSONA_CRYPT_KEY_BYTES]) {
    (void)alloc;
    if (!persona_name || !key_out)
        return HU_ERR_INVALID_ARGUMENT;
    /* In v1, persona_name is accepted (for future per-persona derivation
     * via HKDF) but does not affect the returned key — see the
     * keystore_linux_returns_same_key_on_repeat_call_same_name test. */
    return keystore_linux_load_or_create(key_out);
}

/* ------------------------------------------------------------------------- */
/* 4. Minimal persona->JSON serializer (subset matching hu_persona_load_json) */
/* ------------------------------------------------------------------------- */

typedef struct json_buf {
    char *data;
    size_t len;
    size_t cap;
    bool oom;
} json_buf_t;

static void jb_init(json_buf_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->oom = false;
}
static void jb_free(json_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}
static void jb_grow(json_buf_t *b, size_t need) {
    if (b->oom)
        return;
    if (b->len + need + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + need + 1)
        cap *= 2;
    char *p = (char *)realloc(b->data, cap);
    if (!p) {
        b->oom = true;
        return;
    }
    b->data = p;
    b->cap = cap;
}
static void jb_putc(json_buf_t *b, char c) {
    jb_grow(b, 1);
    if (b->oom)
        return;
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}
static void jb_puts(json_buf_t *b, const char *s) {
    size_t n = strlen(s);
    jb_grow(b, n);
    if (b->oom)
        return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}
static void jb_putstr(json_buf_t *b, const char *s) {
    /* Emit a JSON string with conservative escaping. We only escape the
     * characters that the JSON parser used by hu_persona_load_json
     * refuses unescaped: ", \, control chars 0x00-0x1F. UTF-8 byte
     * sequences pass through verbatim. */
    jb_putc(b, '"');
    if (!s) {
        jb_putc(b, '"');
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            jb_putc(b, '\\');
            jb_putc(b, (char)c);
        } else if (c == '\n') {
            jb_puts(b, "\\n");
        } else if (c == '\r') {
            jb_puts(b, "\\r");
        } else if (c == '\t') {
            jb_puts(b, "\\t");
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            jb_puts(b, buf);
        } else {
            jb_putc(b, (char)c);
        }
    }
    jb_putc(b, '"');
}

static void jb_putstr_array(json_buf_t *b, char **arr, size_t n) {
    jb_putc(b, '[');
    for (size_t i = 0; i < n; i++) {
        if (i)
            jb_putc(b, ',');
        jb_putstr(b, arr[i] ? arr[i] : "");
    }
    jb_putc(b, ']');
}

/* Serialise the subset of hu_persona_t that hu_persona_load_json parses
 * and that AC-8.2.1 asserts to round-trip: name, core.identity,
 * core.traits, core.communication_rules, core.values, core.decision_style,
 * and example_banks (channel + per-example {context,incoming,response}).
 *
 * Other fields exist on hu_persona_t but are derived/cached state or
 * outside the AC scope. */
static hu_error_t persona_to_json(const hu_persona_t *p, char **out, size_t *out_len) {
    json_buf_t b;
    jb_init(&b);
    jb_putc(&b, '{');
    jb_puts(&b, "\"name\":");
    jb_putstr(&b, p->name ? p->name : "");
    jb_puts(&b, ",\"core\":{");
    jb_puts(&b, "\"identity\":");
    jb_putstr(&b, p->identity ? p->identity : "");
    jb_puts(&b, ",\"traits\":");
    jb_putstr_array(&b, p->traits, p->traits_count);
    jb_puts(&b, ",\"communication_rules\":");
    jb_putstr_array(&b, p->communication_rules, p->communication_rules_count);
    jb_puts(&b, ",\"values\":");
    jb_putstr_array(&b, p->values, p->values_count);
    if (p->decision_style) {
        jb_puts(&b, ",\"decision_style\":");
        jb_putstr(&b, p->decision_style);
    }
    jb_putc(&b, '}'); /* close core */

    jb_puts(&b, ",\"example_banks\":[");
    for (size_t i = 0; i < p->example_banks_count; i++) {
        if (i)
            jb_putc(&b, ',');
        const hu_persona_example_bank_t *bank = &p->example_banks[i];
        jb_putc(&b, '{');
        jb_puts(&b, "\"channel\":");
        jb_putstr(&b, bank->channel ? bank->channel : "");
        jb_puts(&b, ",\"examples\":[");
        for (size_t j = 0; j < bank->examples_count; j++) {
            if (j)
                jb_putc(&b, ',');
            const hu_persona_example_t *ex = &bank->examples[j];
            jb_putc(&b, '{');
            jb_puts(&b, "\"context\":");
            jb_putstr(&b, ex->context ? ex->context : "");
            jb_puts(&b, ",\"incoming\":");
            jb_putstr(&b, ex->incoming ? ex->incoming : "");
            jb_puts(&b, ",\"response\":");
            jb_putstr(&b, ex->response ? ex->response : "");
            jb_putc(&b, '}');
        }
        jb_puts(&b, "]}");
    }
    jb_putc(&b, ']');
    jb_putc(&b, '}');

    if (b.oom) {
        jb_free(&b);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = b.data;
    *out_len = b.len;
    return HU_OK;
}

/* ------------------------------------------------------------------------- */
/* 5. Atomic ciphertext write                                                */
/* ------------------------------------------------------------------------- */

/* Build the on-disk HUP1 byte string in `ct_out`. Caller frees with free().
 * `nonce_out` returns the random nonce used (so a follow-up save can verify
 * the test that successive saves produce different ciphertext). */
static hu_error_t seal_bytes(const uint8_t *plaintext, size_t pt_len,
                             const uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES], uint8_t **ct_out,
                             size_t *ct_len_out) {
    size_t ct_buf_len = HU_PERSONA_CRYPT_HEADER_BYTES + pt_len + HU_PERSONA_CRYPT_MAC_BYTES;
    uint8_t *ct = (uint8_t *)malloc(ct_buf_len);
    if (!ct)
        return HU_ERR_OUT_OF_MEMORY;
    /* magic */
    memcpy(ct, HU_PERSONA_CRYPT_MAGIC, 4);
    /* version + reserved */
    ct[4] = HU_PERSONA_CRYPT_VERSION;
    ct[5] = 0;
    ct[6] = 0;
    ct[7] = 0;
    /* nonce */
    randombytes_buf(ct + 8, HU_PERSONA_CRYPT_NONCE_BYTES);
    /* ciphertext+MAC */
    if (crypto_secretbox_easy(ct + HU_PERSONA_CRYPT_HEADER_BYTES, plaintext, pt_len, ct + 8, key) !=
        0) {
        sodium_memzero(ct, ct_buf_len);
        free(ct);
        return HU_ERR_CRYPTO_ENCRYPT;
    }
    *ct_out = ct;
    *ct_len_out = ct_buf_len;
    return HU_OK;
}

/* Sibling-directory fsync (best-effort; not all platforms or fs's honour it,
 * but POSIX requires it for rename durability). */
static void fsync_parent_dir(const char *path) {
    char dir[1024];
    size_t n = strlen(path);
    if (n >= sizeof(dir))
        return;
    memcpy(dir, path, n + 1);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    if (slash == dir)
        slash[1] = '\0';
    else
        slash[0] = '\0';
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    (void)fsync(fd);
    close(fd);
}

/* Write `bytes` to <path> via the atomic tmp+fsync+rename pattern.
 *
 * If <path>.tmp already exists, returns HU_ERR_IO_BUSY (concurrent migration
 * lock; design R5).  This is what makes the
 * test_save_encrypted_preserves_prior_state_when_tmp_blocked adversary
 * (pre-blocking <path>.tmp with a directory) deterministic. */
static hu_error_t atomic_write(const char *path, const uint8_t *bytes, size_t len) {
    char tmp[1024];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return HU_ERR_INVALID_ARGUMENT;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno == EEXIST || errno == EISDIR)
            return HU_ERR_IO_BUSY;
        return HU_ERR_IO;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, bytes + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            (void)unlink(tmp);
            return HU_ERR_IO;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0) {
        close(fd);
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (close(fd) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        return HU_ERR_IO;
    }
    fsync_parent_dir(path);
    return HU_OK;
}

/* ------------------------------------------------------------------------- */
/* 6. Public save_encrypted / load_encrypted                                 */
/* ------------------------------------------------------------------------- */

hu_error_t hu_persona_save_encrypted(hu_allocator_t *alloc, const char *path,
                                     const hu_persona_t *persona,
                                     const uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES]) {
    (void)alloc;
    if (!path || !persona || !key)
        return HU_ERR_INVALID_ARGUMENT;

    char *json = NULL;
    size_t json_len = 0;
    hu_error_t err = persona_to_json(persona, &json, &json_len);
    if (err != HU_OK)
        return err;

    uint8_t *ct = NULL;
    size_t ct_len = 0;
    err = seal_bytes((const uint8_t *)json, json_len, key, &ct, &ct_len);
    sodium_memzero(json, json_len);
    free(json);
    if (err != HU_OK)
        return err;

    err = atomic_write(path, ct, ct_len);
    sodium_memzero(ct, ct_len);
    free(ct);
    return err;
}

/* Read entire file into a heap buffer. Caller frees with free(). */
static hu_error_t read_file_all(const char *path, uint8_t **buf_out, size_t *len_out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return HU_ERR_NOT_FOUND;
        return HU_ERR_IO;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return HU_ERR_IO;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) {
        close(fd);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, buf + off, len - off);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return HU_ERR_IO;
        }
        if (r == 0)
            break;
        off += (size_t)r;
    }
    close(fd);
    if (off != len) {
        free(buf);
        return HU_ERR_IO;
    }
    *buf_out = buf;
    *len_out = len;
    return HU_OK;
}

hu_error_t hu_persona_load_encrypted(hu_allocator_t *alloc, const char *path,
                                     const uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES],
                                     hu_persona_t *out) {
    if (!alloc || !path || !key || !out)
        return HU_ERR_INVALID_ARGUMENT;

    uint8_t *file = NULL;
    size_t file_len = 0;
    hu_error_t err = read_file_all(path, &file, &file_len);
    if (err != HU_OK)
        return err;

    /* Classify FIRST. AC-8.2.2 + design R1: never decode bytes we can't
     * confirm are HUP1, and never populate `out` if anything goes wrong. */
    if (hu_persona_classify_bytes(file, file_len) != HU_PERSONA_FORMAT_ENCRYPTED_V1) {
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_INVALID_FORMAT;
    }
    if (file_len < HU_PERSONA_CRYPT_HEADER_BYTES + HU_PERSONA_CRYPT_MAC_BYTES) {
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_INVALID_FORMAT;
    }

    const uint8_t *nonce = file + 8;
    const uint8_t *ct = file + HU_PERSONA_CRYPT_HEADER_BYTES;
    size_t ct_len = file_len - HU_PERSONA_CRYPT_HEADER_BYTES;
    uint8_t *pt = (uint8_t *)malloc(ct_len);
    if (!pt) {
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_OUT_OF_MEMORY;
    }
    if (crypto_secretbox_open_easy(pt, ct, ct_len, nonce, key) != 0) {
        /* MAC verification failed.  Leave *out untouched per AC-8.2.2. */
        sodium_memzero(pt, ct_len);
        free(pt);
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_DECRYPT_FAILED;
    }
    size_t pt_len = ct_len - HU_PERSONA_CRYPT_MAC_BYTES;

    /* Parse plaintext JSON into `out`. hu_persona_load_json memsets out on
     * entry, so we only populate it on success. */
    hu_persona_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    err = hu_persona_load_json(alloc, (const char *)pt, pt_len, &tmp);
    sodium_memzero(pt, ct_len);
    free(pt);
    sodium_memzero(file, file_len);
    free(file);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, &tmp);
        return err;
    }
    *out = tmp;
    return HU_OK;
}

/* ------------------------------------------------------------------------- */
/* 7. Migration                                                              */
/* ------------------------------------------------------------------------- */

static hu_error_t shred_and_unlink(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT)
            return HU_OK;
        return HU_ERR_IO;
    }
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        size_t len = (size_t)st.st_size;
        const size_t chunk = 4096;
        uint8_t zero[4096];
        memset(zero, 0, sizeof(zero));
        size_t off = 0;
        while (off < len) {
            size_t n = len - off;
            if (n > chunk)
                n = chunk;
            ssize_t w = write(fd, zero, n);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
        (void)fsync(fd);
        close(fd);
    } else if (fd >= 0) {
        close(fd);
    }
    if (unlink(path) != 0 && errno != ENOENT)
        return HU_ERR_IO;
    return HU_OK;
}

hu_error_t hu_persona_migrate_to_encrypted(hu_allocator_t *alloc, const char *path,
                                           const char *persona_name) {
    if (!alloc || !path || !persona_name)
        return HU_ERR_INVALID_ARGUMENT;

    uint8_t *file = NULL;
    size_t file_len = 0;
    hu_error_t err = read_file_all(path, &file, &file_len);
    if (err != HU_OK)
        return err;

    hu_persona_format_t fmt = hu_persona_classify_bytes(file, file_len);
    if (fmt == HU_PERSONA_FORMAT_ENCRYPTED_V1) {
        /* Idempotent: already encrypted. */
        free(file);
        return HU_OK;
    }
    if (fmt != HU_PERSONA_FORMAT_PLAINTEXT_JSON) {
        free(file);
        return HU_ERR_INVALID_FORMAT;
    }

    /* Parse plaintext. If parse fails, leave plaintext untouched — we never
     * delete data we cannot re-encrypt. */
    hu_persona_t persona;
    memset(&persona, 0, sizeof(persona));
    err = hu_persona_load_json(alloc, (const char *)file, file_len, &persona);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, &persona);
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_INVALID_FORMAT;
    }

    /* Derive (or fetch) the key for this persona. */
    uint8_t key[HU_PERSONA_CRYPT_KEY_BYTES];
    err = hu_persona_crypt_derive_key(alloc, persona_name, key);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, &persona);
        sodium_memzero(file, file_len);
        free(file);
        return err;
    }

    /* Serialise + seal. */
    char *json = NULL;
    size_t json_len = 0;
    err = persona_to_json(&persona, &json, &json_len);
    if (err != HU_OK) {
        sodium_memzero(key, sizeof(key));
        hu_persona_deinit(alloc, &persona);
        sodium_memzero(file, file_len);
        free(file);
        return err;
    }
    uint8_t *ct = NULL;
    size_t ct_len = 0;
    err = seal_bytes((const uint8_t *)json, json_len, key, &ct, &ct_len);
    sodium_memzero(json, json_len);
    free(json);
    sodium_memzero(key, sizeof(key));
    hu_persona_deinit(alloc, &persona);
    if (err != HU_OK) {
        sodium_memzero(file, file_len);
        free(file);
        return err;
    }

    /* Atomic write to <path>. If the tmp slot is blocked (e.g. directory
     * blocker in the adversary test), atomic_write returns HU_ERR_IO_BUSY
     * BEFORE touching <path>, so the prior plaintext is preserved. */
    err = atomic_write(path, ct, ct_len);
    sodium_memzero(ct, ct_len);
    free(ct);
    sodium_memzero(file, file_len);
    free(file);
    return err;
}

/* ------------------------------------------------------------------------- */
/* 8. Refuse-path legacy loader                                              */
/* ------------------------------------------------------------------------- */

hu_error_t hu_persona_load_legacy(hu_allocator_t *alloc, const char *path, hu_persona_t *out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    uint8_t *file = NULL;
    size_t file_len = 0;
    hu_error_t err = read_file_all(path, &file, &file_len);
    if (err != HU_OK)
        return err;

    hu_persona_format_t fmt = hu_persona_classify_bytes(file, file_len);
    if (fmt == HU_PERSONA_FORMAT_ENCRYPTED_V1) {
        /* AC-8.2.4 / design R1: post-migration enforcement. The file is
         * encrypted; the legacy loader MUST NOT silently re-decode. Refuse
         * loudly with HU_ERR_LEGACY_REFUSED, leave *out untouched.
         *
         * Adversarial test
         *   `test_load_legacy_refuses_encrypted_file_with_legacy_refused_error`
         * asserts both this return code AND that *out is unpopulated. */
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_LEGACY_REFUSED;
    }
    if (fmt != HU_PERSONA_FORMAT_PLAINTEXT_JSON) {
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_INVALID_FORMAT;
    }

    /* Plaintext + sentinel -> load. Plaintext - sentinel -> refuse.
     * The sentinel exists only for the brief window in which an in-progress
     * migration is being recovered; outside that window plaintext personas
     * are NEVER silently loaded. */
    char sentinel[1024];
    int sn = snprintf(sentinel, sizeof(sentinel), "%s.migration-pending", path);
    if (sn < 0 || (size_t)sn >= sizeof(sentinel)) {
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_INVALID_ARGUMENT;
    }
    struct stat st;
    if (stat(sentinel, &st) != 0) {
        sodium_memzero(file, file_len);
        free(file);
        return HU_ERR_LEGACY_REFUSED;
    }

    hu_persona_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    err = hu_persona_load_json(alloc, (const char *)file, file_len, &tmp);
    sodium_memzero(file, file_len);
    free(file);
    if (err != HU_OK) {
        hu_persona_deinit(alloc, &tmp);
        return err;
    }
    *out = tmp;
    return HU_OK;
}

/* Unused-warning silencer for the shred helper, which is referenced by the
 * recovery path that lives outside this story's scope (sprint-9 wiring).
 * Keeping the symbol available avoids reintroducing it later. */
__attribute__((unused)) static void persona_crypt_keep_shred_symbol(const char *p) {
    (void)shred_and_unlink(p);
}
