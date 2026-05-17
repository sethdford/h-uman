/* tests/test_hula_examples.c — US-10.4 HuLa examples gallery.
 *
 * Iterates the five canonical example programs under
 * `examples/hula/<NN>-<slug>/program.json`, parses each via
 * `hu_hula_parse_json`, validates via `hu_hula_validate` against an
 * empty tool registry (structure + opcode rules only), and asserts a
 * pinned root/structural fingerprint for each.
 *
 * Test seam for US-10.4 (see sprints/sprint-10/stories.md AC-10.4.2).
 * References production symbols `hu_hula_parse_json` and
 * `hu_hula_validate` from `src/agent/hula.c`, satisfying
 * `.claude/rules/test-references-production-symbol.md`.
 *
 * Pinned-string assertions (anti-pattern guard per
 * `.claude/rules/tests-that-pin-bugs.md`):
 *   - The program's `name` field must match the case row.
 *   - Where applicable, the root tool_name must match the case row.
 * If any program.json is corrupted (e.g. a closing brace deleted),
 * parsing fails and HU_ASSERT_EQ(err, HU_OK) terminates the test.
 */

#include "human/agent/hula.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HU_EXAMPLES_HULA_DIR
#error "HU_EXAMPLES_HULA_DIR must be defined when building human_tests"
#endif

/* ── helpers ────────────────────────────────────────────────────────────── */

/* Read a file fully into a malloc'd buffer. Returns NULL on failure. */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return NULL;
    }
    size_t sz = (size_t)pos;
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    if (got != sz) {
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    if (out_len)
        *out_len = sz;
    return buf;
}

/* Recursively check whether the subtree rooted at `n` contains any
 * node with `op == target`. Pure structural inspection over the
 * parsed tree; not a re-implementation of validation logic. */
static bool tree_contains_op(const hu_hula_node_t *n, hu_hula_op_t target) {
    if (!n)
        return false;
    if (n->op == target)
        return true;
    for (size_t i = 0; i < n->children_count; i++) {
        if (tree_contains_op(n->children[i], target))
            return true;
    }
    return false;
}

/* ── case table ─────────────────────────────────────────────────────────── */

/* If you add a directory to `examples/hula/`, append a row here. */
typedef struct {
    const char *path_suffix;       /* relative to HU_EXAMPLES_HULA_DIR */
    const char *expected_name;     /* pinned program.name */
    hu_hula_op_t expected_root_op; /* root->op; HU_HULA_OP_COUNT == any */
    const char
        *expected_root_tool;      /* root->tool_name when expected_root_op==CALL; NULL otherwise */
    hu_hula_op_t must_contain_op; /* must appear somewhere in tree; HU_HULA_OP_COUNT == none */
    hu_hula_op_t must_also_contain; /* a second op that must appear; HU_HULA_OP_COUNT == none */
} hula_example_case_t;

static const hula_example_case_t k_cases[] = {
    {
        "01-simple-call/program.json",
        "example_01_simple_call",
        HU_HULA_CALL,
        "echo",
        HU_HULA_OP_COUNT,
        HU_HULA_OP_COUNT,
    },
    {
        "02-branching/program.json",
        "example_02_branching",
        HU_HULA_SEQ,
        NULL,
        HU_HULA_BRANCH,
        HU_HULA_OP_COUNT,
    },
    {
        "03-error-recovery/program.json",
        "example_03_error_recovery",
        HU_HULA_TRY,
        NULL,
        HU_HULA_OP_COUNT,
        HU_HULA_OP_COUNT,
    },
    {
        "04-emergence-detection/program.json",
        "example_04_emergence_detection",
        HU_HULA_SEQ,
        NULL,
        HU_HULA_VERIFY,
        HU_HULA_OP_COUNT,
    },
    {
        "05-multi-step-pipeline/program.json",
        "example_05_multi_step_pipeline",
        HU_HULA_SEQ,
        NULL,
        HU_HULA_PAR,
        HU_HULA_CALL,
    },
};

#define K_CASE_COUNT (sizeof(k_cases) / sizeof(k_cases[0]))

/* ── exercise one case (parse + validate + structural fingerprint) ──────── */

static void exercise_case(const hula_example_case_t *c) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", HU_EXAMPLES_HULA_DIR, c->path_suffix);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));

    size_t json_len = 0;
    char *json = read_file(path, &json_len);
    HU_ASSERT_NOT_NULL(json);
    HU_ASSERT_TRUE(json_len > 0);

    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    hu_error_t err = hu_hula_parse_json(&alloc, json, json_len, &prog);
    if (err != HU_OK) {
        fprintf(stderr, "FAIL parse %s: err=%d\n", path, (int)err);
    }
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prog.root);

    /* Pinned program.name — guards against silent edits that swap the
     * intent of an example. */
    HU_ASSERT_NOT_NULL(prog.name);
    HU_ASSERT_STR_EQ(prog.name, c->expected_name);

    /* AC-10.4.2: validate with tools=NULL,count=0 (empty registry —
     * skips tool-name resolution; structure + opcode rules apply). */
    hu_hula_validation_t v;
    err = hu_hula_validate(&prog, &alloc, NULL, 0, &v);
    if (err != HU_OK || !v.valid) {
        fprintf(stderr, "FAIL validate %s: err=%d valid=%d diag_count=%zu\n", path, (int)err,
                (int)v.valid, v.diag_count);
        if (v.diag_count > 0 && v.diags[0].message)
            fprintf(stderr, "  first diag: %s\n", v.diags[0].message);
    }
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(v.valid);

    /* Root opcode fingerprint. */
    if (c->expected_root_op != HU_HULA_OP_COUNT) {
        HU_ASSERT_EQ((int)prog.root->op, (int)c->expected_root_op);
    }

    /* Root tool_name fingerprint for CALL roots. AC-10.4.3 requires
     * "non-empty tool" for the simple-call example. */
    if (c->expected_root_op == HU_HULA_CALL) {
        HU_ASSERT_NOT_NULL(prog.root->tool_name);
        HU_ASSERT_TRUE(strlen(prog.root->tool_name) > 0);
        if (c->expected_root_tool) {
            HU_ASSERT_STR_EQ(prog.root->tool_name, c->expected_root_tool);
        }
    }

    /* Subtree contains required ops. */
    if (c->must_contain_op != HU_HULA_OP_COUNT) {
        HU_ASSERT_TRUE(tree_contains_op(prog.root, c->must_contain_op));
    }
    if (c->must_also_contain != HU_HULA_OP_COUNT) {
        HU_ASSERT_TRUE(tree_contains_op(prog.root, c->must_also_contain));
    }

    hu_hula_validation_deinit(&alloc, &v);
    hu_hula_program_deinit(&prog);
    free(json);
}

/* ── test cases ─────────────────────────────────────────────────────────── */

static void example_01_simple_call_is_valid_call_with_echo_tool(void) {
    exercise_case(&k_cases[0]);
}

static void example_02_branching_seq_contains_branch_node(void) {
    exercise_case(&k_cases[1]);
}

static void example_03_error_recovery_root_is_try_with_catch(void) {
    /* Beyond the shared structural fingerprint, AC-10.4.5 wants a
     * non-NULL catch child specifically. */
    exercise_case(&k_cases[2]);

    char path[1024];
    (void)snprintf(path, sizeof(path), "%s/%s", HU_EXAMPLES_HULA_DIR, k_cases[2].path_suffix);
    size_t json_len = 0;
    char *json = read_file(path, &json_len);
    HU_ASSERT_NOT_NULL(json);

    hu_allocator_t alloc = hu_system_allocator();
    hu_hula_program_t prog;
    HU_ASSERT_EQ(hu_hula_parse_json(&alloc, json, json_len, &prog), HU_OK);
    HU_ASSERT_EQ((int)prog.root->op, (int)HU_HULA_TRY);
    /* TRY parser appends body then catch; we expect >=2 children with
     * the second being the catch child. */
    HU_ASSERT_GE(prog.root->children_count, (size_t)2);
    HU_ASSERT_NOT_NULL(prog.root->children[0]);
    HU_ASSERT_NOT_NULL(prog.root->children[1]);
    hu_hula_program_deinit(&prog);
    free(json);
}

static void example_04_emergence_detection_contains_verify(void) {
    exercise_case(&k_cases[3]);
}

static void example_05_multi_step_pipeline_contains_seq_and_par(void) {
    exercise_case(&k_cases[4]);
}

static void all_five_examples_are_present(void) {
    /* Defensive guard against AC-10.4.1 — if any case row's file
     * disappears, this is the canonical place where it fails loudly. */
    HU_ASSERT_EQ(K_CASE_COUNT, (size_t)5);
    for (size_t i = 0; i < K_CASE_COUNT; i++) {
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", HU_EXAMPLES_HULA_DIR, k_cases[i].path_suffix);
        HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "missing example: %s\n", path);
        }
        HU_ASSERT_NOT_NULL(f);
        fclose(f);
    }
}

/* ── registration ───────────────────────────────────────────────────────── */

void run_hula_examples_tests(void) {
    HU_TEST_SUITE("hula_examples");
    HU_RUN_TEST(all_five_examples_are_present);
    HU_RUN_TEST(example_01_simple_call_is_valid_call_with_echo_tool);
    HU_RUN_TEST(example_02_branching_seq_contains_branch_node);
    HU_RUN_TEST(example_03_error_recovery_root_is_try_with_catch);
    HU_RUN_TEST(example_04_emergence_detection_contains_verify);
    HU_RUN_TEST(example_05_multi_step_pipeline_contains_seq_and_par);
}
