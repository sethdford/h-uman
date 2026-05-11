#include "human/agent/tom_scenario.h"

#include "human/core/json.h"
#include "human/memory/belief.h"
#include <stdio.h>
#include <string.h>

static void tom_copy_trunc(char *dest, size_t cap, const char *src, size_t slen) {
    if (!dest || cap == 0) {
        return;
    }
    if (!src || slen == 0) {
        dest[0] = '\0';
        return;
    }
    size_t n = slen < cap - 1 ? slen : cap - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
}

static const char *tom_tag_for_category(const char *category) {
    if (!category || !category[0]) {
        return "[ToM:unknown]";
    }
    if (strcmp(category, "false_belief") == 0) {
        return "[ToM:fb] Privileged facts may be hidden from the modeled agent; answer from "
               "their belief state, not the narrator's.";
    }
    if (strcmp(category, "second_order") == 0) {
        return "[ToM:so] Separate nested beliefs (what A thinks B believes) from ground truth.";
    }
    if (strcmp(category, "pragmatic_implicature") == 0) {
        return "[ToM:pr] Prefer reasonable implicature over literal reading for indirect speech.";
    }
    if (strcmp(category, "multilingual_stub") == 0) {
        return "[ToM:ml] Cross-language turns may need explicit repair or clarification.";
    }
    if (strcmp(category, "common_knowledge") == 0) {
        return "[ToM:ck] Shared nicknames or in-jokes may not resolve for all participants.";
    }
    return "[ToM:unknown]";
}

void hu_tom_scenario_synthesize(const char *premise, size_t premise_len, const char *question,
                                size_t question_len, const char *category, size_t category_len,
                                int64_t now_ms, hu_theory_of_mind_t *out) {
    if (!out) {
        return;
    }
    (void)category_len;
    memset(out, 0, sizeof(*out));
    tom_copy_trunc(out->user_thinks_we_are, sizeof(out->user_thinks_we_are), premise, premise_len);
    tom_copy_trunc(out->user_expects_we_can, sizeof(out->user_expects_we_can), question,
                   question_len);
    const char *tag = tom_tag_for_category(category);
    (void)snprintf(out->user_expects_we_cannot, sizeof(out->user_expects_we_cannot), "%s", tag);
    out->confidence = hu_belief_init(0.55f, "tom-scenario", now_ms);
}

static int tom_item_has_expected_tag(const char *category, const hu_theory_of_mind_t *t) {
    if (!t) {
        return 0;
    }
    const char *c = t->user_expects_we_cannot;
    if (!c || !c[0]) {
        return 0;
    }
    if (strcmp(category, "false_belief") == 0) {
        return strstr(c, "[ToM:fb]") != NULL;
    }
    if (strcmp(category, "second_order") == 0) {
        return strstr(c, "[ToM:so]") != NULL;
    }
    if (strcmp(category, "pragmatic_implicature") == 0) {
        return strstr(c, "[ToM:pr]") != NULL;
    }
    if (strcmp(category, "multilingual_stub") == 0) {
        return strstr(c, "[ToM:ml]") != NULL;
    }
    if (strcmp(category, "common_knowledge") == 0) {
        return strstr(c, "[ToM:ck]") != NULL;
    }
    return 0;
}

static char *read_file_all(hu_allocator_t *alloc, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(512 * 1024)) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
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

hu_error_t hu_tom_b8_synthetic_pack_run_smoke(hu_allocator_t *alloc, const char *json_path,
                                              unsigned *pass_out, unsigned *total_out) {
    if (!alloc || !json_path || !pass_out || !total_out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    *pass_out = 0;
    *total_out = 0;
    size_t json_len = 0;
    char *json = read_file_all(alloc, json_path, &json_len);
    if (!json) {
        return HU_ERR_NOT_FOUND;
    }
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    alloc->free(alloc->ctx, json, json_len + 1);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root) {
            hu_json_free(alloc, root);
        }
        return err != HU_OK ? err : HU_ERR_JSON_PARSE;
    }
    hu_json_value_t *items = hu_json_object_get(root, "items");
    if (!items || items->type != HU_JSON_ARRAY || !items->data.array.items) {
        hu_json_free(alloc, root);
        return HU_ERR_JSON_PARSE;
    }
    unsigned pass = 0;
    unsigned total = 0;
    for (size_t i = 0; i < items->data.array.len; i++) {
        hu_json_value_t *it = items->data.array.items[i];
        if (!it || it->type != HU_JSON_OBJECT) {
            continue;
        }
        const char *premise = hu_json_get_string(it, "premise");
        const char *question = hu_json_get_string(it, "question");
        const char *category = hu_json_get_string(it, "category");
        if (!premise || !question || !category) {
            continue;
        }
        total++;
        hu_theory_of_mind_t tom;
        hu_tom_scenario_synthesize(premise, strlen(premise), question, strlen(question), category,
                                   strlen(category), 1735689600000LL, &tom);
        if (tom.user_thinks_we_are[0] != '\0' && tom.user_expects_we_can[0] != '\0' &&
            tom_item_has_expected_tag(category, &tom)) {
            pass++;
        }
    }
    hu_json_free(alloc, root);
    *pass_out = pass;
    *total_out = total;
    return HU_OK;
}
