#include "human/agent/tom_scenario.h"

#include "human/core/json.h"
#include "human/memory/belief.h"
#include <stdbool.h>
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

static unsigned char tom_ascii_tolower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32u) : c;
}

static bool tom_substr_ci_bounded(const char *s, size_t slen, const char *needle) {
    if (!s || !needle || !needle[0]) {
        return false;
    }
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > slen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= slen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (tom_ascii_tolower((unsigned char)s[i + j]) !=
                tom_ascii_tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static void wm_tom_merge_field(char *field, size_t cap, const char *incoming) {
    if (!field || cap == 0 || !incoming || !incoming[0]) {
        return;
    }
    if (field[0] == '\0' || strcmp(field, "unknown") == 0) {
        tom_copy_trunc(field, cap, incoming, strlen(incoming));
        return;
    }
    size_t lo = strlen(field);
    const char *sep = " | ";
    size_t sep_len = strlen(sep);
    if (lo + sep_len >= cap) {
        return;
    }
    memcpy(field + lo, sep, sep_len);
    lo += sep_len;
    field[lo] = '\0';
    size_t room = cap - lo - 1;
    size_t inlen = strlen(incoming);
    if (inlen > room) {
        inlen = room;
    }
    memcpy(field + lo, incoming, inlen);
    field[lo + inlen] = '\0';
}

void hu_world_model_merge_tom_scenario(hu_world_model_t *wm, const char *premise, size_t premise_len,
                                       const char *question, size_t question_len,
                                       const char *category, size_t category_len, int64_t now_ms) {
    if (!wm || !premise || premise_len == 0 || !question || question_len == 0 || !category ||
        category_len == 0) {
        return;
    }
    hu_theory_of_mind_t sc;
    hu_tom_scenario_synthesize(premise, premise_len, question, question_len, category, category_len,
                               now_ms, &sc);
    wm_tom_merge_field(wm->tom.user_thinks_we_are, sizeof(wm->tom.user_thinks_we_are),
                       sc.user_thinks_we_are);
    wm_tom_merge_field(wm->tom.user_expects_we_can, sizeof(wm->tom.user_expects_we_can),
                       sc.user_expects_we_can);
    wm_tom_merge_field(wm->tom.user_expects_we_cannot, sizeof(wm->tom.user_expects_we_cannot),
                       sc.user_expects_we_cannot);
    wm->tom.confidence = sc.confidence;
}

bool hu_tom_scenario_gold_matches_response(const char *gold_answer, const char *response,
                                           size_t response_len, size_t min_token_len) {
    if (!gold_answer || gold_answer[0] == '\0' || !response) {
        return false;
    }
    if (min_token_len < 1) {
        min_token_len = 1;
    }
    if (min_token_len > 64) {
        min_token_len = 64;
    }

    size_t long_segments = 0;
    size_t matched_long = 0;
    const char *p = gold_answer;
    while (*p) {
        const char *seg_start = p;
        while (*p && *p != '_') {
            p++;
        }
        size_t seglen = (size_t)(p - seg_start);
        if (seglen >= min_token_len) {
            long_segments++;
            char seg[96];
            if (seglen >= sizeof(seg)) {
                seglen = sizeof(seg) - 1;
            }
            memcpy(seg, seg_start, seglen);
            seg[seglen] = '\0';
            if (tom_substr_ci_bounded(response, response_len, seg)) {
                matched_long++;
            }
        }
        if (*p == '_') {
            p++;
        }
    }

    if (long_segments > 0) {
        return matched_long == long_segments;
    }

    return tom_substr_ci_bounded(response, response_len, gold_answer);
}

static int tom_json_pack_score_gold_one(const char *premise, const char *question, const char *category,
                                        const char *gold) {
    hu_theory_of_mind_t tom;
    hu_tom_scenario_synthesize(premise, strlen(premise), question, strlen(question), category,
                               strlen(category), 1735689600000LL, &tom);
    char hay[640];
    int n = snprintf(hay, sizeof(hay), "%s %s %s %s %s", premise, question, tom.user_thinks_we_are,
                     tom.user_expects_we_can, tom.user_expects_we_cannot);
    if (n <= 0 || (size_t)n >= sizeof(hay)) {
        return 0;
    }
    return hu_tom_scenario_gold_matches_response(gold, hay, (size_t)n, 3) ? 1 : 0;
}

hu_error_t hu_tom_b8_synthetic_pack_score_gold(hu_allocator_t *alloc, const char *json_path,
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
        const char *gold = hu_json_get_string(it, "gold_answer");
        if (!premise || !question || !category || !gold) {
            continue;
        }
        total++;
        if (tom_json_pack_score_gold_one(premise, question, category, gold)) {
            pass++;
        }
    }
    hu_json_free(alloc, root);
    *pass_out = pass;
    *total_out = total;
    return HU_OK;
}

static const hu_tom_b8_response_t *tom_response_find(const hu_tom_b8_response_t *responses,
                                                     size_t responses_count, const char *id) {
    if (!responses || responses_count == 0 || !id) {
        return NULL;
    }
    for (size_t i = 0; i < responses_count; i++) {
        if (responses[i].id && strcmp(responses[i].id, id) == 0) {
            return &responses[i];
        }
    }
    return NULL;
}

hu_error_t hu_tom_b8_synthetic_pack_score_responses(hu_allocator_t *alloc, const char *json_path,
                                                    const hu_tom_b8_response_t *responses,
                                                    size_t responses_count,
                                                    int count_unanswered_as_failed,
                                                    unsigned *pass_out, unsigned *total_out) {
    if (!alloc || !json_path || !pass_out || !total_out) {
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (!responses && responses_count > 0) {
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
        const char *id = hu_json_get_string(it, "id");
        const char *gold = hu_json_get_string(it, "gold_answer");
        if (!id || !gold) {
            continue;
        }
        const hu_tom_b8_response_t *r = tom_response_find(responses, responses_count, id);
        if (!r || !r->response || r->response_len == 0) {
            if (count_unanswered_as_failed) {
                total++;
            }
            continue;
        }
        total++;
        if (hu_tom_scenario_gold_matches_response(gold, r->response, r->response_len, 3)) {
            pass++;
        }
    }
    hu_json_free(alloc, root);
    *pass_out = pass;
    *total_out = total;
    return HU_OK;
}
