/*
 * src/app/cli_prospective.c — `human prospective init|probe|backfill`.
 *
 *   human prospective init --db PATH
 *   human prospective probe [--full] [--shadow] --db PATH --contact ID [--now EPOCH]
 *         (--inbound TEXT | --tick | --deliver TEXT) [--history FILE]
 *         [--judge fire|already_resolved|cancel|not_now|model] [--group] [--self]
 *   human prospective backfill --db PATH [--write] [--now EPOCH]
 *
 * The probe runs the same functions the daemon runs (hu_prospective_v2_run /
 * _after_delivery) against the database named by --db, which is required:
 * it never opens ~/.human/memory.db by default. scripts/pm_bench_local.py
 * drives it with a fixture DB and a scripted clock.
 */
#include "human/cli_prospective.h"

#include "human/cli_commands.h"
#include "human/memory/prospective_policy.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PM_HISTORY_CAP 16384

bool hu_cli_prospective_parse(int argc, char **argv, hu_cli_prospective_args_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    out->judge = "not_now";
    if (!argv || argc < 3 || !argv[2])
        return false;
    bool probe = strcmp(argv[2], "probe") == 0;
    if (strcmp(argv[2], "init") == 0)
        out->op = HU_CLI_PM_INIT;
    else if (strcmp(argv[2], "backfill") == 0)
        out->op = HU_CLI_PM_BACKFILL;
    else if (!probe)
        return false;
    for (int i = 3; i < argc; i++) {
        const char *k = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(k, "--full") == 0) {
            out->full = true;
        } else if (strcmp(k, "--shadow") == 0) {
            out->shadow = true;
        } else if (strcmp(k, "--group") == 0) {
            out->group = true;
        } else if (strcmp(k, "--self") == 0) {
            out->self = true;
        } else if (strcmp(k, "--write") == 0) {
            out->write = true;
        } else if (strcmp(k, "--tick") == 0) {
            if (out->op != HU_CLI_PM_NONE)
                return false;
            out->op = HU_CLI_PM_TICK;
        } else if (!v) {
            return false;
        } else if (strcmp(k, "--db") == 0) {
            out->db = argv[++i];
        } else if (strcmp(k, "--contact") == 0) {
            out->contact = argv[++i];
        } else if (strcmp(k, "--history") == 0) {
            out->history_path = argv[++i];
        } else if (strcmp(k, "--judge") == 0) {
            out->judge = argv[++i];
        } else if (strcmp(k, "--now") == 0) {
            char *end = NULL;
            out->now = strtoll(v, &end, 10);
            if (!end || *end || out->now <= 0)
                return false;
            i++;
        } else if (strcmp(k, "--inbound") == 0 || strcmp(k, "--deliver") == 0) {
            if (out->op != HU_CLI_PM_NONE)
                return false;
            out->op = k[2] == 'i' ? HU_CLI_PM_INBOUND : HU_CLI_PM_DELIVER;
            out->text = argv[++i];
        } else {
            return false;
        }
    }
    if (!out->db || !out->db[0])
        return false;
    if (!probe)
        return out->op == HU_CLI_PM_INIT || out->op == HU_CLI_PM_BACKFILL;
    if (out->op == HU_CLI_PM_NONE || !out->contact || !out->contact[0])
        return false;
    return strcmp(out->judge, "model") == 0 ||
           hu_prospective_parse_verdict(out->judge, strlen(out->judge)) != HU_PM_VERDICT_PARSE_FAIL;
}

#ifdef HU_ENABLE_SQLITE

static void pm_emit_counts(FILE *out, const hu_prospective_counts_t *c, size_t bytes) {
    fprintf(out,
            "candidates=%zu fire=%zu resolved=%zu cancel=%zu not_now=%zu parse_fail=%zu "
            "judge_err=%zu expired=%zu capped=%zu bytes=%zu\n",
            c->candidates, c->fire, c->resolved, c->cancel, c->not_now, c->parse_fail, c->judge_err,
            c->expired, c->capped, bytes);
}

static hu_error_t pm_run_deliver(hu_allocator_t *alloc, sqlite3 *db,
                                 const hu_cli_prospective_args_t *a, int64_t now, FILE *out) {
    size_t cl = strlen(a->contact);
    size_t tl = a->text ? strlen(a->text) : 0;
    hu_prospective_delivery_counts_t k;
    hu_prospective_delivery_counts_t t;
    hu_error_t e = hu_prospective_v2_after_delivery(alloc, db, HU_PM_CUE_KEYWORD, a->contact, cl,
                                                    a->text, tl, now, &k);
    if (e == HU_OK)
        e = hu_prospective_v2_after_delivery(alloc, db, HU_PM_CUE_TIME, a->contact, cl, a->text, tl,
                                             now, &t);
    if (e != HU_OK)
        return e;
    fprintf(out, "surfaced=%zu used=%zu ignored=%zu expired=%zu\n", k.surfaced + t.surfaced,
            k.used + t.used, k.ignored + t.ignored, k.expired + t.expired);
    return HU_OK;
}

hu_error_t hu_cli_prospective_run(hu_allocator_t *alloc, hu_memory_t *mem,
                                  const hu_cli_prospective_args_t *a, const char *history,
                                  size_t history_len, const hu_prospective_judge_t *judge,
                                  FILE *out) {
    if (!alloc || !mem || !a || !out)
        return HU_ERR_INVALID_ARGUMENT;
    sqlite3 *db = hu_sqlite_memory_get_db(mem);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;
    if (a->op == HU_CLI_PM_INIT) { /* opening the store created and migrated it */
        fprintf(out, "ok\n");
        return HU_OK;
    }
    int64_t now = a->now > 0 ? (int64_t)a->now : (int64_t)time(NULL);
    if (a->op == HU_CLI_PM_DELIVER)
        return pm_run_deliver(alloc, db, a, now, out);
    if (a->op != HU_CLI_PM_INBOUND && a->op != HU_CLI_PM_TICK)
        return HU_ERR_NOT_SUPPORTED;
    hu_prospective_turn_t turn;
    memset(&turn, 0, sizeof(turn));
    turn.contact = a->contact;
    turn.contact_len = strlen(a->contact);
    if (a->op == HU_CLI_PM_INBOUND && a->text) {
        turn.inbound = a->text;
        turn.inbound_len = strlen(a->text);
    }
    turn.history = history;
    turn.history_len = history ? history_len : 0;
    turn.is_group = a->group;
    turn.is_self = a->self;
    turn.now = now;
    turn.day_start = hu_prospective_local_day_start(now);
    hu_prospective_counts_t c;
    char *d = NULL;
    size_t dl = 0;
    hu_error_t e = hu_prospective_v2_run(
        alloc, db, a->op == HU_CLI_PM_TICK ? HU_PM_CUE_TIME : HU_PM_CUE_KEYWORD, &turn, judge,
        !a->shadow, &c, &d, &dl);
    if (e != HU_OK)
        return e;
    pm_emit_counts(out, &c, dl);
    if (a->full) {
        for (size_t i = 0; i < c.item_count; i++)
            fprintf(out, "item id=%lld verdict=%s\n", (long long)c.items[i].id,
                    c.items[i].judge_ok ? hu_prospective_verdict_str(c.items[i].verdict)
                                        : "judge_err");
        if (d && dl > 0)
            fprintf(out, "%.*s\n", (int)dl, d);
    }
    if (d)
        alloc->free(alloc->ctx, d, dl + 1);
    return HU_OK;
}

/* A fixed answer — the scripted judge the probe offers besides the model. */
static hu_error_t pm_const_judge(void *ctx, hu_allocator_t *alloc, const char *system,
                                 size_t system_len, const char *user, size_t user_len, char **out,
                                 size_t *out_len) {
    (void)system;
    (void)system_len;
    (void)user;
    (void)user_len;
    const char *word = (const char *)ctx;
    size_t n = strlen(word);
    char *b = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!b)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(b, word, n + 1);
    *out = b;
    *out_len = n;
    return HU_OK;
}

static char *pm_read_file(hu_allocator_t *alloc, const char *path, size_t *len) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = (char *)alloc->alloc(alloc->ctx, PM_HISTORY_CAP);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, PM_HISTORY_CAP - 1, f);
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static void pm_usage(void) {
    fprintf(stderr, "Usage: human prospective init --db PATH\n"
                    "       human prospective probe [--full] [--shadow] --db PATH --contact ID "
                    "[--now EPOCH]\n"
                    "             (--inbound TEXT | --tick | --deliver TEXT) [--history FILE]\n"
                    "             [--judge fire|already_resolved|cancel|not_now|model] [--group] "
                    "[--self]\n"
                    "       human prospective backfill --db PATH [--write] [--now EPOCH]\n");
}

hu_error_t cmd_prospective(hu_allocator_t *alloc, int argc, char **argv) {
    hu_cli_prospective_args_t a;
    if (!hu_cli_prospective_parse(argc, argv, &a)) {
        pm_usage();
        return HU_ERR_INVALID_ARGUMENT;
    }
    size_t hist_len = 0;
    char *hist = NULL;
    if (a.history_path) {
        hist = pm_read_file(alloc, a.history_path, &hist_len);
        if (!hist) {
            fprintf(stderr, "prospective: cannot read %s\n", a.history_path);
            return HU_ERR_IO;
        }
    }
    hu_memory_t mem = hu_sqlite_memory_create(alloc, a.db);
    if (!mem.vtable) {
        fprintf(stderr, "prospective: cannot open %s\n", a.db);
        if (hist)
            alloc->free(alloc->ctx, hist, PM_HISTORY_CAP);
        return HU_ERR_IO;
    }
    char word[24];
    snprintf(word, sizeof(word), "%s", a.judge);
    hu_prospective_judge_t judge = {.fn = pm_const_judge, .ctx = word};
    hu_error_t err;
    if (strcmp(a.judge, "model") == 0) {
        fprintf(stderr, "prospective: --judge model needs the provider adapter (Task 7)\n");
        err = HU_ERR_NOT_SUPPORTED;
    } else {
        err = hu_cli_prospective_run(alloc, &mem, &a, hist, hist_len, &judge, stdout);
    }
    if (err != HU_OK)
        fprintf(stderr, "prospective: %s\n", hu_error_string(err));
    mem.vtable->deinit(mem.ctx);
    if (hist)
        alloc->free(alloc->ctx, hist, PM_HISTORY_CAP);
    return err;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t cmd_prospective(hu_allocator_t *alloc, int argc, char **argv) {
    (void)alloc;
    (void)argc;
    (void)argv;
    fprintf(stderr, "prospective: this build has no SQLite memory\n");
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
