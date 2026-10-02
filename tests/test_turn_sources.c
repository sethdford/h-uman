/* tests/test_turn_sources.c — source-presence pins for the hu_agent_turn carve.
 *
 * The characterization goldens run under HU_IS_TEST, so code inside
 * `#ifndef HU_IS_TEST` never runs in the suite. Stage moves carry those blocks
 * verbatim; these tests pin, by reading the source, that each landed in its
 * stage file exactly once, still guarded, and left agent_turn.c. They also
 * hold the carve's structural invariants: no stage file includes <sqlite3.h>
 * or the provider factory; the W12 contact-recall merge leaves graph_ctx alone
 * (#561 removed the free that dropped it; spec §5 item 4); and the turn body
 * and agent_turn.c only shrink. Those last two are hand ratchets: the global function-length
 * and file-size gates are held by hu_service_run / daemon.c and cannot lock
 * agent_turn.c's gains (plan gap G3).
 *
 * Reads repo-relative paths; skips when not run from the repo root. */
// @covers-none — source-presence pins over several production files, no single module
#include "test_framework.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hand ratchets. Lower both to the values this suite prints in every stage
 * commit; never raise them — except for Task 11's DAG-worker heap-alloc fix
 * (asan-pthread-stack-aliasing-darwin.md), the carve's one deliberate
 * behaviour change: it adds code to agent_turn.c in place (no stage file to
 * carry the growth), so this is the one commit where the ceiling moves up to
 * match, by exactly the lines the fix adds. */
#define TS_AGENT_TURN_C_MAX_LINES   7018
#define TS_AGENT_TURN_RUN_MAX_LINES 5716

static char *ts_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static size_t ts_count(const char *hay, const char *needle) {
    size_t n = 0, nl = strlen(needle);
    for (const char *p = strstr(hay, needle); p; p = strstr(p + nl, needle))
        n++;
    return n;
}

/* Lines containing `needle` inside an `#ifndef HU_IS_TEST` or
 * `#if … !defined(HU_IS_TEST)` region (the #else arm of one is test-only). */
static size_t ts_count_not_test(const char *src, const char *needle) {
    enum { TS_MAXD = 64 };
    bool guard[TS_MAXD];
    int depth = 0;
    size_t hits = 0, nl = strlen(needle);
    const char *line = src;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *p = line;
        while (p < line + len && (*p == ' ' || *p == '\t'))
            p++;
        if (p < line + len && *p == '#') {
            const char *d = p + 1;
            while (d < line + len && *d == ' ')
                d++;
            if (strncmp(d, "if", 2) == 0) {
                char tmp[256];
                size_t tl = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
                memcpy(tmp, line, tl);
                tmp[tl] = '\0';
                bool not_test = strstr(tmp, "ifndef HU_IS_TEST") != NULL ||
                                strstr(tmp, "!defined(HU_IS_TEST)") != NULL;
                if (depth < TS_MAXD)
                    guard[depth] = not_test;
                depth++;
            } else if (strncmp(d, "else", 4) == 0 || strncmp(d, "elif", 4) == 0) {
                if (depth > 0 && depth <= TS_MAXD)
                    guard[depth - 1] = false;
            } else if (strncmp(d, "endif", 5) == 0) {
                if (depth > 0)
                    depth--;
            }
        } else {
            bool inside = false;
            for (int i = 0; i < depth && i < TS_MAXD; i++)
                if (guard[i])
                    inside = true;
            if (inside && len >= nl) {
                for (const char *h = line; h + nl <= line + len; h++) {
                    if (memcmp(h, needle, nl) == 0) {
                        hits++;
                        break;
                    }
                }
            }
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    return hits;
}

static void turn_stage_files_never_include_sqlite3_or_the_provider_factory(void) {
    DIR *d = opendir("src/agent/turn");
    HU_SKIP_IF(!d, "run from the repo root");
    size_t files = 0, bad = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 3 || strcmp(e->d_name + n - 2, ".c") != 0)
            continue;
        char path[512];
        (void)snprintf(path, sizeof(path), "src/agent/turn/%s", e->d_name);
        char *src = ts_read(path);
        if (!src) {
            bad++;
            continue;
        }
        files++;
        if (ts_count(src, "#include <sqlite3.h>") != 0 ||
            ts_count(src, "human/providers/factory.h") != 0) {
            printf("    %s includes sqlite3.h or the provider factory\n", path);
            bad++;
        }
        free(src);
    }
    closedir(d);
    HU_ASSERT_EQ(bad, 0);
    HU_ASSERT_GT(files, 1); /* turn_ctx.c + at least one stage */
}

/* #561 (913a3f7e3): the W12 merge used to free graph_ctx when it folded
 * contact recall into memory_ctx, dropping LIVE graph grounding before the
 * prompt on every memory-bearing turn. The moved block must keep the merge and
 * must not regain that free. */
static void turn_retrieve_w12_merge_leaves_graph_ctx_alive(void) {
    char *src = ts_read("src/agent/turn/turn_retrieve.c");
    HU_SKIP_IF(!src, "run from the repo root");
    size_t frees = ts_count(src, "graph_ctx, graph_ctx_len + 1);");
    size_t merges = ts_count(src, "memcpy(merged + pos, contact_text, contact_text_len);");
    free(src);
    HU_ASSERT_EQ(merges, 1);
    HU_ASSERT_EQ(frees, 0);
}

static void agent_turn_body_and_file_only_shrink(void) {
    char *src = ts_read("src/agent/agent_turn.c");
    HU_SKIP_IF(!src, "run from the repo root");
    const char *run = strstr(src, "static hu_error_t agent_turn_run(");
    const char *wrap = strstr(src, "\nhu_error_t hu_agent_turn(hu_agent_t *agent,");
    size_t body = 0, file_lines = 0;
    if (run && wrap && run < wrap)
        for (const char *p = run; p < wrap; p++)
            body += *p == '\n';
    for (const char *p = src; *p; p++)
        file_lines += *p == '\n';
    free(src);
    printf("    agent_turn_run spans %zu lines (ceiling %d); agent_turn.c %zu lines (ceiling %d)\n",
           body, TS_AGENT_TURN_RUN_MAX_LINES, file_lines, TS_AGENT_TURN_C_MAX_LINES);
    HU_ASSERT_GT(body, 0);
    HU_ASSERT_LE(body, TS_AGENT_TURN_RUN_MAX_LINES);
    HU_ASSERT_LE(file_lines, TS_AGENT_TURN_C_MAX_LINES);
}

/* S4's two local-hour reads run only in the daemon (tests pin hour = 10). */
static void turn_context_keeps_both_not_test_hour_blocks(void) {
    char *stage = ts_read("src/agent/turn/turn_context.c");
    char *turn = ts_read("src/agent/agent_turn.c");
    HU_SKIP_IF(!stage || !turn, "run from the repo root");
    const char *needle = "hour = (uint8_t)(lt->tm_hour & 0xFF);";
    size_t in_stage = ts_count_not_test(stage, needle);
    size_t in_turn = ts_count(turn, needle);
    free(stage);
    free(turn);
    HU_ASSERT_EQ(in_stage, 2);
    HU_ASSERT_EQ(in_turn, 0);
}

/* spec §3 item 6 / .claude/rules/asan-pthread-stack-aliasing-darwin.md: the
 * cross-thread DAG worker contexts must not live in the loop-scoped frame. */
static void dag_batch_workers_live_on_the_heap(void) {
    char *src = ts_read("src/agent/turn/turn_tools.c");
    HU_SKIP_IF(!src, "run from the repo root");
    size_t stack_arrays = ts_count(src, "dag_parallel_work_t works[");
    size_t heap_blocks = ts_count_not_test(src, "dag_parallel_work_t *works =");
    size_t sizes = ts_count(src, "sizeof(dag_parallel_work_t)");
    free(src);
    HU_ASSERT_EQ(stack_arrays, 0);
    HU_ASSERT_EQ(heap_blocks, 1);
    HU_ASSERT_EQ(sizes, 2); /* the alloc and the free */
}

/* S16's daemon-only regions landed in turn_tools.c exactly once, still
 * guarded, and left agent_turn.c. */
static void turn_tools_keeps_its_not_test_regions(void) {
    static const char *const needles[] = {
        "/* HuLa compiler: LLM emits full HuLa JSON (preferred over DAG when enabled). */",
        "bool batch_thread_safe = (batch.count > 1);",
        "if (!used_llm_compiler && !used_hula_ir && agent->hula_enabled && tc_count >= 1) {",
        "static void *dag_parallel_worker(void *arg) {",
        "static void hula_compiler_agent_done(void *ctx, const hu_hula_program_t *prog,",
    };
    char *stage = ts_read("src/agent/turn/turn_tools.c");
    char *turn = ts_read("src/agent/agent_turn.c");
    HU_SKIP_IF(!stage || !turn, "run from the repo root");
    size_t bad = 0;
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        size_t in_stage = ts_count_not_test(stage, needles[i]);
        size_t in_turn = ts_count(turn, needles[i]);
        if (in_stage != 1 || in_turn != 0) {
            printf(
                "    \"%s\": %zu in turn_tools.c (want 1, guarded), %zu in agent_turn.c (want 0)\n",
                needles[i], in_stage, in_turn);
            bad++;
        }
    }
    free(stage);
    free(turn);
    HU_ASSERT_EQ(bad, 0);
}

void run_turn_sources_tests(void) {
    HU_TEST_SUITE("TurnSources");
    HU_RUN_TEST(turn_stage_files_never_include_sqlite3_or_the_provider_factory);
    HU_RUN_TEST(turn_retrieve_w12_merge_leaves_graph_ctx_alive);
    HU_RUN_TEST(turn_context_keeps_both_not_test_hour_blocks);
    HU_RUN_TEST(agent_turn_body_and_file_only_shrink);
    HU_RUN_TEST(dag_batch_workers_live_on_the_heap);
    HU_RUN_TEST(turn_tools_keeps_its_not_test_regions);
}
