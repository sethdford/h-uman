#include "human/behavior/dialog_act.h"
#include "human/core/json.h"
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HU_EVAL_SUITES_DIR
#error "HU_EVAL_SUITES_DIR must be defined when building human_tests"
#endif

static char *read_entire_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(4 * 1024 * 1024)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) {
        *out_len = rd;
    }
    return buf;
}

static void behavior_tom_synthetic_pack_loads(void) {
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/tom/tom_synthetic.json", HU_EVAL_SUITES_DIR);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));
    size_t len = 0;
    char *buf = read_entire_file(path, &len);
    HU_ASSERT_NOT_NULL(buf);

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, buf, len, &root), HU_OK);
    HU_ASSERT_NOT_NULL(root);
    HU_ASSERT_EQ(root->type, HU_JSON_OBJECT);

    hu_json_value_t *items = hu_json_object_get(root, "items");
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_EQ(items->type, HU_JSON_ARRAY);
    HU_ASSERT_EQ(items->data.array.len, 10u);

    hu_json_free(&alloc, root);
    free(buf);
}

static void behavior_repair_pack_loads_and_dialog_acts_align(void) {
    char path[768];
    int n = snprintf(path, sizeof(path), "%s/repair/repair_scenarios.json", HU_EVAL_SUITES_DIR);
    HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(path));
    size_t len = 0;
    char *buf = read_entire_file(path, &len);
    HU_ASSERT_NOT_NULL(buf);

    hu_allocator_t alloc = hu_system_allocator();
    hu_json_value_t *root = NULL;
    HU_ASSERT_EQ(hu_json_parse(&alloc, buf, len, &root), HU_OK);
    hu_json_value_t *items = hu_json_object_get(root, "items");
    HU_ASSERT_NOT_NULL(items);
    HU_ASSERT_EQ(items->type, HU_JSON_ARRAY);
    HU_ASSERT_EQ(items->data.array.len, 15u);

    size_t dialog_hits = 0;
    for (size_t i = 0; i < items->data.array.len; i++) {
        const hu_json_value_t *it = items->data.array.items[i];
        HU_ASSERT_NOT_NULL(it);
        HU_ASSERT_EQ(it->type, HU_JSON_OBJECT);
        const char *utt = hu_json_get_string(it, "user_utterance");
        const char *eda = hu_json_get_string(it, "expected_dialog_act");
        const char *era = hu_json_get_string(it, "expected_relational_act");
        HU_ASSERT_NOT_NULL(utt);
        HU_ASSERT_NOT_NULL(eda);
        HU_ASSERT_NOT_NULL(era);
        (void)era;
        size_t ul = strlen(utt);
        hu_dialog_act_t got = hu_dialog_act_classify(utt, ul);
        if (strcmp(hu_dialog_act_name(got), eda) == 0) {
            dialog_hits++;
        }
    }
    /* Allow a small drift while the dialog-act lexicon evolves. */
    HU_ASSERT_TRUE(dialog_hits >= 12u);

    hu_json_free(&alloc, root);
    free(buf);
}

void run_behavior_corpora_tests(void);

void run_behavior_corpora_tests(void) {
    HU_TEST_SUITE("behavior_corpora");
    HU_RUN_TEST(behavior_tom_synthetic_pack_loads);
    HU_RUN_TEST(behavior_repair_pack_loads_and_dialog_acts_align);
}
