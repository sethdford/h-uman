/* src/eval/commitment_sample.c — contract in include/human/eval/commitment_sample.h */
#include "human/eval/commitment_sample.h"
#include "human/core/io_secure.h"
#include "human/core/paths.h"
#include "human/memory.h"
#include "human/persona.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define SHEET_HEADER "label\tid\tdirection\tcontact\tdeadline\tsaid"

static void put_clean(FILE *f, const char *s) {
    for (; s && *s; s++)
        fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
}

static const char *contact_label(const hu_persona_t *p, const char *contact_id) {
    for (size_t i = 0; p && i < p->contacts_count; i++) {
        const hu_contact_profile_t *c = &p->contacts[i];
        if (c->contact_id && c->name && c->name[0] && strcmp(c->contact_id, contact_id) == 0)
            return c->name;
    }
    return contact_id;
}

int hu_commitment_sample_write(FILE *f, const hu_superhuman_commitment_t *rows, size_t n,
                               const hu_persona_t *persona) {
    if (!f || (!rows && n > 0))
        return -1;
    fputs("# Commitment detection sample. In the first column write y if the row is a\n"
          "# real commitment (someone said they will do something), n if it is not.\n"
          "# Leave it blank to skip. Then: human commitments score <this file>\n",
          f);
    fputs(SHEET_HEADER "\n", f);
    int written = 0;
    for (size_t i = 0; i < n; i++) {
        const hu_superhuman_commitment_t *r = &rows[i];
        const char *dir = strcmp(r->who, "me") == 0 ? "mine" : "theirs";
        char deadline[16] = "-";
        if (r->deadline > 0) {
            time_t t = (time_t)r->deadline;
            struct tm tm;
            if (localtime_r(&t, &tm))
                strftime(deadline, sizeof(deadline), "%Y-%m-%d", &tm);
        }
        fprintf(f, "\t%lld\t%s\t", (long long)r->id, dir);
        put_clean(f, contact_label(persona, r->contact_id));
        fprintf(f, "\t%s\t", deadline);
        put_clean(f, r->description);
        fputc('\n', f);
        written++;
    }
    return ferror(f) ? -1 : written;
}

/* Field `idx` (0-based, tab-separated) of line[0..len) into buf. */
static bool field(const char *line, size_t len, int idx, char *buf, size_t cap) {
    size_t start = 0;
    for (int k = 0; k < idx; k++) {
        const char *tab = memchr(line + start, '\t', len - start);
        if (!tab)
            return false;
        start = (size_t)(tab - line) + 1;
    }
    const char *tab = memchr(line + start, '\t', len - start);
    size_t end = tab ? (size_t)(tab - line) : len;
    size_t n = end - start;
    while (n > 0 && (line[start] == ' ')) /* tolerate a space typed before the label */
        start++, n--;
    while (n > 0 && (line[start + n - 1] == ' ' || line[start + n - 1] == '\r'))
        n--;
    if (n >= cap)
        return false;
    memcpy(buf, line + start, n);
    buf[n] = '\0';
    return true;
}

hu_error_t hu_commitment_sample_score(const char *tsv, size_t len, hu_commitment_score_t *out) {
    if (!tsv || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    for (size_t pos = 0; pos < len;) {
        const char *line = tsv + pos;
        const char *nl = memchr(line, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - line) : len - pos;
        pos += ll + (nl ? 1 : 0);
        if (ll == 0 || line[0] == '#' || (ll >= 6 && strncmp(line, "label\t", 6) == 0))
            continue;
        char label[16], dir[16], deadline[16];
        if (!field(line, ll, 0, label, sizeof(label)) || !field(line, ll, 2, dir, sizeof(dir)) ||
            !field(line, ll, 4, deadline, sizeof(deadline))) {
            out->bad_rows++;
            continue;
        }
        bool mine = strcmp(dir, "mine") == 0;
        if (!mine && strcmp(dir, "theirs") != 0) {
            out->bad_rows++;
            continue;
        }
        int verdict;
        if (label[0] == '\0') {
            out->unlabeled++;
            continue;
        } else if (!strcasecmp(label, "y") || !strcasecmp(label, "yes") || !strcmp(label, "1")) {
            verdict = 1;
        } else if (!strcasecmp(label, "n") || !strcasecmp(label, "no") || !strcmp(label, "0")) {
            verdict = 0;
        } else {
            out->bad_rows++;
            continue;
        }
        if (deadline[0] && strcmp(deadline, "-") != 0) {
            out->labeled_dated++;
            out->correct_dated += (size_t)verdict;
        }
        if (mine) {
            out->labeled_mine++;
            out->correct_mine += (size_t)verdict;
        } else {
            out->labeled_theirs++;
            out->correct_theirs += (size_t)verdict;
        }
    }
    return HU_OK;
}

/* ── CLI ────────────────────────────────────────────────────────────────── */

static void print_precision(const char *what, size_t correct, size_t labeled) {
    if (labeled == 0)
        printf("%-8s no labelled rows yet — no precision to report\n", what);
    else
        printf("%-8s precision %.2f  (%zu of %zu labelled rows are real commitments)\n", what,
               (double)correct / (double)labeled, correct, labeled);
}

static hu_error_t score_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "commitments score: cannot open %s: %s\n", path, strerror(errno));
        return HU_ERR_NOT_FOUND;
    }
    char *buf = NULL;
    size_t cap = 0, len = 0;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (len + got + 1 > cap) {
            size_t ncap = (len + got + 1) * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                return HU_ERR_OUT_OF_MEMORY;
            }
            buf = nb, cap = ncap;
        }
        memcpy(buf + len, chunk, got);
        len += got;
    }
    fclose(f);
    hu_commitment_score_t s;
    hu_error_t err = hu_commitment_sample_score(buf ? buf : "", len, &s);
    free(buf);
    if (err != HU_OK)
        return err;
    print_precision("theirs", s.correct_theirs, s.labeled_theirs);
    print_precision("mine", s.correct_mine, s.labeled_mine);
    print_precision("dated", s.correct_dated, s.labeled_dated);
    printf("         (dated rows are the ones the morning briefing shows)\n");
    printf("skipped  %zu blank, %zu unreadable\n", s.unlabeled, s.bad_rows);
    return s.bad_rows > 0 ? HU_ERR_PARSE : HU_OK;
}

#ifdef HU_ENABLE_SQLITE
static const char *flag_value(int argc, char **argv, const char *name) {
    for (int i = 0; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return NULL;
}
#endif

static void usage(void) {
    printf("usage: human commitments sample [--days N] [--n N] [--db PATH] [--out PATH]\n"
           "                                [--persona NAME]\n"
           "       human commitments score <sheet.tsv>\n"
           "\n"
           "sample  writes recent detected commitments (default: last 30 days, up to 50)\n"
           "        to a sheet for labelling; default --out is\n"
           "        <state dir>/commitment_samples/<today>.tsv\n"
           "score   reports precision per direction from a labelled sheet\n");
}

#ifdef HU_ENABLE_SQLITE
static hu_error_t sample(hu_allocator_t *alloc, int argc, char **argv) {
    const char *v;
    int days = (v = flag_value(argc, argv, "--days")) ? atoi(v) : 30;
    int n = (v = flag_value(argc, argv, "--n")) ? atoi(v) : 50;
    if (days <= 0 || n <= 0 || n > 1000) {
        fprintf(stderr, "commitments sample: --days and --n must be positive (--n at most 1000)\n");
        return HU_ERR_INVALID_ARGUMENT;
    }
    char db_buf[512], out_buf[600];
    const char *db_path = flag_value(argc, argv, "--db");
    if (!db_path) {
        if (hu_paths_state(db_buf, sizeof(db_buf), "memory.db") < 0)
            return HU_ERR_NOT_FOUND;
        db_path = db_buf;
    }
    struct stat st;
    if (stat(db_path, &st) != 0) {
        fprintf(stderr, "commitments sample: no memory database at %s\n", db_path);
        return HU_ERR_NOT_FOUND;
    }
    const char *out_path = flag_value(argc, argv, "--out");
    if (!out_path) {
        char dir[512];
        if (hu_paths_state(dir, sizeof(dir), "commitment_samples") < 0)
            return HU_ERR_NOT_FOUND;
        if (mkdir(dir, 0700) != 0 && errno != EEXIST)
            return HU_ERR_IO;
        time_t now = time(NULL);
        struct tm tm;
        char day[16] = "today";
        if (localtime_r(&now, &tm))
            strftime(day, sizeof(day), "%Y-%m-%d", &tm);
        snprintf(out_buf, sizeof(out_buf), "%s/%s.tsv", dir, day);
        out_path = out_buf;
    }

    hu_memory_t mem = hu_sqlite_memory_create(alloc, db_path);
    if (!mem.ctx) {
        fprintf(stderr, "commitments sample: cannot open %s\n", db_path);
        return HU_ERR_MEMORY_BACKEND;
    }
    hu_superhuman_commitment_t *rows = NULL;
    size_t count = 0;
    int64_t since = (int64_t)time(NULL) - (int64_t)days * 86400;
    hu_error_t err =
        hu_superhuman_commitment_list_recent(&mem, alloc, since, (size_t)n, &rows, &count);

    hu_persona_t persona;
    bool have_persona = false;
    const char *pname = flag_value(argc, argv, "--persona");
    if (err == HU_OK && pname)
        have_persona = hu_persona_load(alloc, pname, strlen(pname), &persona) == HU_OK;

    if (err == HU_OK) {
        /* The sheet quotes private messages: owner-only. The path comes from
         * argv or HU_STATE_DIR, so refuse traversal here, next to the open,
         * as minja_guard.c does (CodeQL cpp/path-injection). */
        FILE *f = NULL;
        if (strstr(out_path, "..") != NULL || strstr(out_path, "%2e") != NULL ||
            strstr(out_path, "%2E") != NULL) {
            fprintf(stderr, "commitments sample: refusing an output path containing '..'\n");
            f = NULL;
        } else if (hu_io_secure_open(out_path, HU_IO_PERM_SECRET, "w", &f) != HU_OK) {
            f = NULL;
        }
        int w = f ? hu_commitment_sample_write(f, rows, count, have_persona ? &persona : NULL) : -1;
        if (!f || fclose(f) != 0 || w < 0) {
            fprintf(stderr, "commitments sample: cannot write %s\n", out_path);
            err = HU_ERR_IO;
        } else {
            printf("wrote %d commitment(s) from the last %d day(s) to %s\n", w, days, out_path);
            if (w == 0)
                printf("nothing detected in that window, so there is nothing to label yet\n");
        }
    }
    if (have_persona)
        hu_persona_deinit(alloc, &persona);
    hu_superhuman_commitment_free(alloc, rows, count);
    mem.vtable->deinit(mem.ctx);
    return err;
}
#endif

hu_error_t cmd_commitments(hu_allocator_t *alloc, int argc, char **argv) {
    /* argv[0] is the program, argv[1] "commitments". */
    const char *sub = argc > 2 ? argv[2] : NULL;
    if (!sub || !strcmp(sub, "--help") || !strcmp(sub, "-h") || !strcmp(sub, "help")) {
        usage();
        return sub ? HU_OK : HU_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(sub, "score") == 0) {
        if (argc < 4) {
            usage();
            return HU_ERR_INVALID_ARGUMENT;
        }
        return score_file(argv[3]);
    }
    if (strcmp(sub, "sample") == 0) {
#ifdef HU_ENABLE_SQLITE
        return sample(alloc, argc - 3, argv + 3);
#else
        (void)alloc;
        fprintf(stderr, "commitments sample needs a build with SQLite\n");
        return HU_ERR_NOT_SUPPORTED;
#endif
    }
    fprintf(stderr, "commitments: unknown subcommand '%s'\n", sub);
    usage();
    return HU_ERR_INVALID_ARGUMENT;
}
