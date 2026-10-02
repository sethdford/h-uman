/*
 * src/app/cli_replay.c — `human replay`: one arm of the real-turn replay
 * harness. Contract: include/human/cli_replay.h. Runbook:
 * docs/guides/replay-harness.md.
 *
 * Reads a JSONL of real turns, replays each through hu_replay_turn_run
 * (src/daemon/replay_turn.c) with the process's own env as the arm's gate
 * configuration, and writes one JSONL row per turn. Nothing is sent: the turn
 * runs on a null channel. Nothing leaves the machine: the only provider is a
 * loopback one, and every libcurl proxy variable points at a dead port.
 */
#include "human/cli_replay.h"

#include "human/bootstrap.h"
#include "human/cli_commands.h"
#include "human/config.h"
#include "human/core/file.h"
#include "human/core/gate_mode.h"
#include "human/core/json.h"
#include "human/core/paths.h"
#include "human/core/string.h"
#include "human/core/time.h"
#include "human/daemon/common.h"
#include "human/memory/semantic_recall.h"
#include "human/memory/vector.h"
#include "human/platform.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REPLAY_MAX_INPUT  (64u * 1024u * 1024u)
#define REPLAY_DEAD_PROXY "http://127.0.0.1:9"

/* ── argument parsing ───────────────────────────────────────────────── */

static bool replay_why(char *why, size_t cap, const char *msg) {
    if (why && cap > 0)
        snprintf(why, cap, "%s", msg);
    return false;
}

bool hu_cli_replay_parse(int argc, char **argv, hu_cli_replay_args_t *out, char *why,
                         size_t why_cap) {
    if (!out)
        return replay_why(why, why_cap, "internal: no output struct");
    memset(out, 0, sizeof(*out));
    out->arm = "default";
    out->endpoint = "http://127.0.0.1:8741/v1";
    out->provider = "mlx_local";
    out->delay_ms = 1500;
    out->director = true;
    out->seed = 1;
    const char *env_dir = getenv("HU_REPLAY_DIRECTOR");
    if (env_dir && strcmp(env_dir, "off") == 0)
        out->director = false;
    for (int i = 2; argv && i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!a)
            continue;
        if (strcmp(a, "--no-director") == 0) {
            out->director = false;
            continue;
        }
        if (!v)
            return replay_why(why, why_cap, "flag needs a value");
        if (strcmp(a, "--in") == 0)
            out->in_path = v;
        else if (strcmp(a, "--out") == 0)
            out->out_path = v;
        else if (strcmp(a, "--arm") == 0)
            out->arm = v;
        else if (strcmp(a, "--endpoint") == 0)
            out->endpoint = v;
        else if (strcmp(a, "--provider") == 0)
            out->provider = v;
        else if (strcmp(a, "--model") == 0)
            out->model = v;
        else if (strcmp(a, "--temperature") == 0) {
            out->force_temperature = true;
            out->temperature = strtod(v, NULL);
        } else if (strcmp(a, "--delay-ms") == 0)
            out->delay_ms = (uint32_t)strtoul(v, NULL, 10);
        else if (strcmp(a, "--limit") == 0)
            out->limit = (size_t)strtoul(v, NULL, 10);
        else if (strcmp(a, "--dump-requests") == 0)
            out->dump_dir = v;
        else if (strcmp(a, "--seed") == 0)
            out->seed = (uint32_t)strtoul(v, NULL, 10);
        else
            return replay_why(why, why_cap, "unknown flag");
        i++;
    }
    if (!out->in_path || !out->out_path)
        return replay_why(why, why_cap, "--in and --out are required");
    return true;
}

/* ── isolation from the live state ──────────────────────────────────── */

static bool replay_path_within(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool hu_cli_replay_isolation_ok(hu_allocator_t *alloc, char *why, size_t why_cap) {
    const char *state = getenv("HU_STATE_DIR");
    const char *mem = getenv("HU_MEMORY_SQLITE_PATH");
    const char *home = getenv("HOME");
    if (!alloc)
        return replay_why(why, why_cap, "internal: no allocator");
    if (!state || !state[0])
        return replay_why(why, why_cap, "HU_STATE_DIR must name the run's private state copy");
    if (!mem || !mem[0])
        return replay_why(why, why_cap, "HU_MEMORY_SQLITE_PATH must name a memory.db copy");
    /* The live state dir is the one hu_paths_state_dir resolves WITHOUT the
     * override: drop HU_STATE_DIR for the one call, then put it back. */
    char live[1024];
    char state_copy[1024];
    snprintf(state_copy, sizeof(state_copy), "%s", state);
    unsetenv("HU_STATE_DIR");
    int ln = hu_paths_state_dir(live, sizeof(live));
    setenv("HU_STATE_DIR", state_copy, 1);
    state = getenv("HU_STATE_DIR");
    if (!home || !home[0] || ln < 0 || (size_t)ln >= sizeof(live))
        return replay_why(why, why_cap, "HOME is unset");
    char *live_real = hu_platform_realpath(alloc, live);
    const char *live_cmp = live_real ? live_real : live;
    char *state_real = hu_platform_realpath(alloc, state);
    char *mem_real = hu_platform_realpath(alloc, mem);
    bool ok = false;
    if (!state_real)
        replay_why(why, why_cap, "HU_STATE_DIR does not exist");
    else if (replay_path_within(state_real, live_cmp))
        replay_why(why, why_cap, "HU_STATE_DIR is the live ~/.human");
    else if (!mem_real)
        replay_why(why, why_cap, "HU_MEMORY_SQLITE_PATH does not exist");
    else if (replay_path_within(mem_real, live_cmp))
        replay_why(why, why_cap, "HU_MEMORY_SQLITE_PATH is inside the live ~/.human");
    else
        ok = true;
    if (live_real)
        alloc->free(alloc->ctx, live_real, strlen(live_real) + 1);
    if (state_real)
        alloc->free(alloc->ctx, state_real, strlen(state_real) + 1);
    if (mem_real)
        alloc->free(alloc->ctx, mem_real, strlen(mem_real) + 1);
    return ok;
}

/* ── input ──────────────────────────────────────────────────────────── */

static char *replay_dup_json_str(hu_allocator_t *alloc, const hu_json_value_t *v) {
    if (!v || v->type != HU_JSON_STRING)
        return NULL;
    return hu_strndup(alloc, v->data.string.ptr, v->data.string.len);
}

static bool replay_copy_bounded(char *dst, size_t cap, const hu_json_value_t *v) {
    if (!v || v->type != HU_JSON_STRING)
        return false;
    size_t n = v->data.string.len < cap - 1 ? v->data.string.len : cap - 1;
    memcpy(dst, v->data.string.ptr, n);
    dst[n] = '\0';
    return true;
}

static hu_error_t replay_join_bubbles(hu_allocator_t *alloc, const hu_json_value_t *arr,
                                      hu_cli_replay_turn_t *out) {
    if (!arr || arr->type != HU_JSON_ARRAY || arr->data.array.len == 0)
        return HU_ERR_PARSE;
    size_t total = 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        const hu_json_value_t *b = arr->data.array.items[i];
        if (!b || b->type != HU_JSON_STRING)
            return HU_ERR_PARSE;
        total += b->data.string.len + 1;
    }
    out->inbound = (char *)alloc->alloc(alloc->ctx, total + 1);
    if (!out->inbound)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        const hu_json_value_t *b = arr->data.array.items[i];
        if (pos > 0)
            out->inbound[pos++] = '\n';
        memcpy(out->inbound + pos, b->data.string.ptr, b->data.string.len);
        pos += b->data.string.len;
    }
    out->inbound[pos] = '\0';
    out->inbound_len = pos;
    return pos > 0 ? HU_OK : HU_ERR_PARSE;
}

static hu_error_t replay_parse_history(hu_allocator_t *alloc, const hu_json_value_t *arr,
                                       hu_cli_replay_turn_t *out) {
    if (!arr)
        return HU_OK;
    if (arr->type != HU_JSON_ARRAY)
        return HU_ERR_PARSE;
    size_t n = arr->data.array.len;
    if (n == 0)
        return HU_OK;
    out->history =
        (hu_channel_history_entry_t *)alloc->alloc(alloc->ctx, n * sizeof(*out->history));
    if (!out->history)
        return HU_ERR_OUT_OF_MEMORY;
    memset(out->history, 0, n * sizeof(*out->history));
    out->history_count = n;
    for (size_t i = 0; i < n; i++) {
        const hu_json_value_t *e = arr->data.array.items[i];
        if (!e || e->type != HU_JSON_OBJECT)
            return HU_ERR_PARSE;
        out->history[i].from_me = hu_json_get_bool(e, "from_me", false);
        if (!replay_copy_bounded(out->history[i].text, sizeof(out->history[i].text),
                                 hu_json_object_get(e, "text")))
            return HU_ERR_PARSE;
        (void)replay_copy_bounded(out->history[i].timestamp, sizeof(out->history[i].timestamp),
                                  hu_json_object_get(e, "ts"));
    }
    return HU_OK;
}

hu_error_t hu_cli_replay_parse_turn(hu_allocator_t *alloc, const char *line, size_t len,
                                    hu_cli_replay_turn_t *out) {
    if (!alloc || !line || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    hu_json_value_t *root = NULL;
    if (hu_json_parse(alloc, line, len, &root) != HU_OK || !root)
        return HU_ERR_PARSE;
    hu_error_t err = HU_ERR_PARSE;
    if (root->type == HU_JSON_OBJECT) {
        out->id = replay_dup_json_str(alloc, hu_json_object_get(root, "id"));
        out->contact_id = replay_dup_json_str(alloc, hu_json_object_get(root, "contact_id"));
        if (out->id && out->contact_id && out->contact_id[0]) {
            err = replay_join_bubbles(alloc, hu_json_object_get(root, "inbound_bubbles"), out);
            if (err == HU_OK)
                err = replay_parse_history(alloc, hu_json_object_get(root, "history"), out);
        }
    }
    hu_json_free(alloc, root);
    if (err != HU_OK)
        hu_cli_replay_turn_free(alloc, out);
    return err;
}

void hu_cli_replay_turn_free(hu_allocator_t *alloc, hu_cli_replay_turn_t *t) {
    if (!alloc || !t)
        return;
    if (t->id)
        alloc->free(alloc->ctx, t->id, strlen(t->id) + 1);
    if (t->contact_id)
        alloc->free(alloc->ctx, t->contact_id, strlen(t->contact_id) + 1);
    if (t->inbound)
        alloc->free(alloc->ctx, t->inbound, t->inbound_len + 1);
    if (t->history)
        alloc->free(alloc->ctx, t->history, t->history_count * sizeof(*t->history));
    memset(t, 0, sizeof(*t));
}

/* ── output ─────────────────────────────────────────────────────────── */

typedef struct replay_jw {
    hu_json_buf_t buf;
    bool first;
    hu_error_t err;
} replay_jw_t;

static void jw_key(replay_jw_t *w, const char *key) {
    if (w->err == HU_OK && !w->first)
        w->err = hu_json_buf_append_raw(&w->buf, ",", 1);
    w->first = false;
    if (w->err == HU_OK)
        w->err = hu_json_append_key(&w->buf, key, strlen(key));
}
static void jw_str(replay_jw_t *w, const char *key, const char *s, size_t n) {
    jw_key(w, key);
    if (w->err == HU_OK)
        w->err =
            s ? hu_json_append_string(&w->buf, s, n) : hu_json_buf_append_raw(&w->buf, "null", 4);
}
static void jw_int(replay_jw_t *w, const char *key, long long v) {
    char num[32];
    int n = snprintf(num, sizeof(num), "%lld", v);
    jw_key(w, key);
    if (w->err == HU_OK)
        w->err = hu_json_buf_append_raw(&w->buf, num, (size_t)n);
}
static void jw_bool(replay_jw_t *w, const char *key, bool v) {
    jw_key(w, key);
    if (w->err == HU_OK)
        w->err = hu_json_buf_append_raw(&w->buf, v ? "true" : "false", v ? 4 : 5);
}
static void jw_raw(replay_jw_t *w, const char *s) {
    if (w->err == HU_OK)
        w->err = hu_json_buf_append_raw(&w->buf, s, strlen(s));
}

static const char *replay_dir_action(hu_director_action_t a) {
    return a == DIR_TAPBACK ? "tapback" : a == DIR_SILENCE ? "silence" : "text";
}

hu_error_t hu_cli_replay_format_result(hu_allocator_t *alloc, const char *id, const char *arm,
                                       uint64_t elapsed_ms, const hu_replay_turn_result_t *r,
                                       char **out, size_t *out_len) {
    if (!alloc || !id || !arm || !r || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    replay_jw_t w = {.first = true};
    w.err = hu_json_buf_init(&w.buf, alloc);
    jw_raw(&w, "{");
    jw_str(&w, "id", id, strlen(id));
    jw_str(&w, "arm", arm, strlen(arm));
    const char *action = hu_replay_action_name(r->action);
    jw_str(&w, "action", action, strlen(action));
    jw_int(&w, "err", (long long)r->err);
    jw_bool(&w, "director_valid", r->director_valid);
    const char *da = replay_dir_action(r->director.action);
    const char *form = hu_director_form_name(r->director.form);
    jw_str(&w, "director_action", r->director_valid ? da : NULL, strlen(da));
    jw_str(&w, "director_form", r->director_valid ? form : NULL, strlen(form));
    jw_int(&w, "director_reaction", (long long)r->director.reaction);
    jw_str(&w, "director_direction", r->director_valid ? r->director.direction : NULL,
           strlen(r->director.direction));
    jw_bool(&w, "voice_memo", r->voice_memo);
    jw_str(&w, "voice_reason", r->voice_reason, r->voice_reason ? strlen(r->voice_reason) : 0);
    jw_bool(&w, "retried", r->retried);
    jw_str(&w, "ai_tell", r->ai_tell, r->ai_tell ? strlen(r->ai_tell) : 0);
    jw_int(&w, "max_chars", (long long)r->max_chars);
    jw_str(&w, "text", r->text, r->text_len);
    jw_key(&w, "bubbles");
    jw_raw(&w, "[");
    for (size_t i = 0; i < r->bubble_count; i++) {
        if (i > 0)
            jw_raw(&w, ",");
        if (w.err == HU_OK)
            w.err = hu_json_append_string(&w.buf, r->bubbles[i], r->bubble_lens[i]);
    }
    jw_raw(&w, "]");
    jw_int(&w, "bubble_count", (long long)r->bubble_count);
    jw_int(&w, "provider_calls", (long long)r->provider_calls);
    jw_int(&w, "reply_calls", (long long)r->reply_calls);
    char fp[24];
    snprintf(fp, sizeof(fp), "%016llx", (unsigned long long)r->reply_fp);
    jw_str(&w, "reply_fp", r->reply_calls ? fp : NULL, strlen(fp));
    jw_int(&w, "reply_bytes", (long long)r->reply_bytes);
    jw_int(&w, "reply_system_bytes", (long long)r->reply_system_bytes);
    jw_int(&w, "channel_outbound_calls", (long long)r->channel_outbound_calls);
    jw_int(&w, "elapsed_ms", (long long)elapsed_ms);
    jw_raw(&w, "}");
    if (w.err != HU_OK) {
        hu_json_buf_free(&w.buf);
        return w.err;
    }
    *out = w.buf.ptr;
    *out_len = w.buf.len;
    return HU_OK;
}

/* ── the command ────────────────────────────────────────────────────── */

/* libcurl honors these; with NO_PROXY covering loopback, the local endpoint
 * stays reachable and every other host fails at a closed port. */
static void replay_fence_network(void) {
    static const char *const k_vars[] = {"ALL_PROXY",   "all_proxy",  "HTTPS_PROXY",
                                         "https_proxy", "HTTP_PROXY", "http_proxy"};
    for (size_t i = 0; i < sizeof(k_vars) / sizeof(k_vars[0]); i++)
        setenv(k_vars[i], REPLAY_DEAD_PROXY, 1);
    setenv("NO_PROXY", "127.0.0.1,localhost,::1", 1);
    setenv("no_proxy", "127.0.0.1,localhost,::1", 1);
}

static FILE *replay_open_private(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return NULL;
    (void)fchmod(fd, 0600);
    FILE *f = fdopen(fd, "w");
    if (!f)
        close(fd);
    return f;
}

static void replay_dump_request(const hu_cli_replay_args_t *a, const char *id,
                                const hu_replay_provider_t *rp) {
    if (!a->dump_dir || !rp->captured || rp->captured_len == 0)
        return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.%s.txt", a->dump_dir, id, a->arm);
    if (n < 0 || (size_t)n >= sizeof(path))
        return;
    FILE *f = replay_open_private(path);
    if (!f)
        return;
    (void)fwrite(rp->captured, 1, rp->captured_len, f);
    fclose(f);
}

/* Swap the agent's provider, the director's classifier and (unless semantic
 * recall is on, which must then embed on loopback) the embedder for local
 * ones. Returns false with a reason when the process cannot be kept local. */
static bool replay_localize(hu_app_ctx_t *app, const hu_cli_replay_args_t *a,
                            hu_replay_provider_t *rp, char *why, size_t cap) {
    hu_provider_t inner;
    const char *key = hu_config_get_provider_key(app->cfg, a->provider);
    hu_error_t err =
        hu_replay_provider_create_local(app->alloc, a->provider, a->endpoint, key, &inner);
    if (err == HU_ERR_PERMISSION_DENIED)
        return replay_why(why, cap, "endpoint is not loopback; replay is local-only");
    if (err != HU_OK)
        return replay_why(why, cap, "could not create the local provider");
    const char *model =
        a->model ? a->model : (app->cfg->default_model ? app->cfg->default_model : "");
    hu_replay_provider_init(rp, inner, true, model, a->force_temperature, a->temperature);
    rp->capture = a->dump_dir != NULL;
    app->agent->provider = hu_replay_provider_as_provider(rp);
    g_classify_provider = hu_replay_provider_as_provider(rp);
    g_classify_provider_ok = true;
    g_classify_model = rp->model;
    g_classify_model_len = strlen(rp->model);
    if (hu_semantic_recall_mode() != HU_GATE_OFF) {
        if (!hu_replay_url_is_loopback(hu_semantic_recall_embed_url()))
            return replay_why(why, cap, "semantic recall embeds off-machine; refusing");
    } else if (app->embedder) {
        hu_embedder_t *emb = (hu_embedder_t *)app->embedder;
        if (emb->vtable && emb->vtable->deinit)
            emb->vtable->deinit(emb->ctx, app->alloc);
        *emb = hu_embedder_local_create(app->alloc);
    }
    return true;
}

static size_t replay_run_lines(hu_allocator_t *alloc, hu_app_ctx_t *app,
                               const hu_cli_replay_args_t *a, hu_replay_provider_t *rp, char *data,
                               size_t data_len, FILE *out, size_t *errors) {
    size_t done = 0;
    char *p = data;
    char *end = data + data_len;
    while (p < end && (a->limit == 0 || done < a->limit)) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *line = p;
        p = nl ? nl + 1 : end;
        if (len == 0)
            continue;
        hu_cli_replay_turn_t t;
        if (hu_cli_replay_parse_turn(alloc, line, len, &t) != HU_OK) {
            fprintf(stderr, "replay: skipping an unparseable line\n");
            (*errors)++;
            continue;
        }
        if (done > 0 && a->delay_ms > 0)
            hu_platform_sleep_ms(a->delay_ms); /* strict sequential pacing */
        hu_replay_turn_input_t in = {.contact_id = t.contact_id,
                                     .inbound = t.inbound,
                                     .inbound_len = t.inbound_len,
                                     .history = t.history,
                                     .history_count = t.history_count,
                                     .director = a->director,
                                     .seed = a->seed};
        int64_t t0 = hu_time_wall_ms();
        hu_replay_turn_result_t r;
        (void)hu_replay_turn_run(alloc, app->agent, app->cfg, rp, &in, &r);
        int64_t t1 = hu_time_wall_ms();
        if (r.channel_outbound_calls != 0) {
            /* Unreachable by construction; abort loudly rather than continue. */
            fprintf(stderr, "replay: FATAL a channel send was attempted\n");
            abort();
        }
        if (r.action == HU_REPLAY_ACTION_ERROR)
            (*errors)++;
        char *row = NULL;
        size_t row_len = 0;
        if (hu_cli_replay_format_result(alloc, t.id, a->arm, (uint64_t)(t1 - t0), &r, &row,
                                        &row_len) == HU_OK) {
            (void)fwrite(row, 1, row_len, out);
            (void)fputc('\n', out);
            fflush(out);
            alloc->free(alloc->ctx, row, row_len + 1);
        }
        replay_dump_request(a, t.id, rp);
        fprintf(stderr, "replay: %zu %s action=%s bubbles=%zu\n", done + 1, a->arm,
                hu_replay_action_name(r.action), r.bubble_count);
        hu_replay_turn_result_deinit(alloc, &r);
        hu_cli_replay_turn_free(alloc, &t);
        done++;
    }
    return done;
}

static const char k_replay_usage[] =
    "Usage: human replay --in TURNS.jsonl --out RESULTS.jsonl [--arm NAME] [--endpoint URL]\n"
    "         [--provider mlx_local] [--model NAME] [--temperature T] [--delay-ms N]\n"
    "         [--limit N] [--no-director] [--dump-requests DIR] [--seed N]\n"
    "Replays real inbound turns through the daemon's reply path offline: nothing is sent,\n"
    "models are reached on loopback only, and HU_STATE_DIR / HU_MEMORY_SQLITE_PATH must\n"
    "name a private snapshot. The process env is the arm's gate configuration.\n"
    "Runbook: docs/guides/replay-harness.md\n";

hu_error_t cmd_replay(hu_allocator_t *alloc, int argc, char **argv) {
    for (int i = 2; argv && i < argc; i++) {
        if (argv[i] && (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)) {
            fputs(k_replay_usage, stdout);
            return HU_OK;
        }
    }
    hu_cli_replay_args_t a;
    char why[256] = "";
    if (!hu_cli_replay_parse(argc, argv, &a, why, sizeof(why))) {
        fprintf(stderr, "replay: %s\n%s", why, k_replay_usage);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!hu_replay_url_is_loopback(a.endpoint)) {
        fprintf(stderr, "replay: refusing a non-loopback endpoint; the harness is local-only\n");
        return HU_ERR_PERMISSION_DENIED;
    }
    if (!hu_cli_replay_isolation_ok(alloc, why, sizeof(why))) {
        fprintf(stderr, "replay: refusing to start: %s\n", why);
        return HU_ERR_PERMISSION_DENIED;
    }
    replay_fence_network();
    char *data = NULL;
    size_t data_len = 0;
    if (hu_file_slurp(alloc, a.in_path, REPLAY_MAX_INPUT, &data, &data_len) != HU_OK) {
        fprintf(stderr, "replay: cannot read --in\n");
        return HU_ERR_IO;
    }
    FILE *out = replay_open_private(a.out_path);
    if (!out) {
        alloc->free(alloc->ctx, data, data_len + 1);
        fprintf(stderr, "replay: cannot open --out\n");
        return HU_ERR_IO;
    }
    hu_app_ctx_t app;
    memset(&app, 0, sizeof(app));
    hu_error_t err = hu_app_bootstrap(&app, alloc, NULL, true, false);
    hu_replay_provider_t rp;
    memset(&rp, 0, sizeof(rp));
    hu_provider_t original = {0};
    size_t done = 0;
    size_t errors = 0;
    if (err != HU_OK || !app.agent_ok || !app.agent) {
        fprintf(stderr, "replay: bootstrap failed: %s\n", hu_error_string(err));
        err = err != HU_OK ? err : HU_ERR_INTERNAL;
    } else {
        original = app.agent->provider;
        if (!replay_localize(&app, &a, &rp, why, sizeof(why))) {
            fprintf(stderr, "replay: %s\n", why);
            err = HU_ERR_PERMISSION_DENIED;
        } else {
            done = replay_run_lines(alloc, &app, &a, &rp, data, data_len, out, &errors);
            fprintf(stderr, "replay: arm=%s turns=%zu errors=%zu\n", a.arm, done, errors);
            err = (done == 0 || errors > 0) ? HU_ERR_INTERNAL : HU_OK;
        }
    }
    fclose(out);
    alloc->free(alloc->ctx, data, data_len + 1);
    g_classify_provider_ok = false;
    if (app.agent && original.vtable)
        app.agent->provider = original; /* teardown frees the bootstrap provider */
    hu_replay_provider_deinit(&rp, alloc);
    hu_app_teardown(&app);
    return err;
}
