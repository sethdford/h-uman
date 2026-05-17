#include "human/eval/competitive_harness.h"

#include "human/core/json.h"

#include <stdio.h>
#include <string.h>

static hu_error_t write_text_file(const char *path, const char *body) {
    if (!path || !body)
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fputs(body, f);
    fclose(f);
    return HU_OK;
}

hu_error_t hu_competitive_harness_run_with_test_judges(
    hu_allocator_t *alloc, const hu_competitive_harness_config_t *cfg,
    const hu_competitive_harness_judge_slot_t *judges, size_t n_judges,
    hu_competitive_harness_result_t *out) {
    if (!alloc || !cfg || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->n_columns = n_judges;

    size_t avail = 0;
    for (size_t i = 0; i < n_judges; i++) {
        if (judges && judges[i].available)
            avail++;
    }
    out->n_available = avail;

    size_t min_avail = cfg->min_available > 0 ? cfg->min_available : 1;
    snprintf(out->summary, sizeof(out->summary),
             "Run summary: %zu of %zu competitors available", avail, n_judges);

    char md[16384];
    size_t off = 0;
    off += (size_t)snprintf(md + off, sizeof(md) - off, "# Competitive scorecard\n\n%s\n\n",
                            out->summary);
    off += (size_t)snprintf(md + off, sizeof(md) - off,
                            "| column | persona_fidelity | status |\n");
    off += (size_t)snprintf(md + off, sizeof(md) - off, "|--------|------------------|--------|\n");

    for (size_t i = 0; judges && i < n_judges; i++) {
        const hu_competitive_harness_judge_slot_t *j = &judges[i];
        if (j->available) {
            off += (size_t)snprintf(md + off, sizeof(md) - off, "| %s | (v2 scorer) | ok |\n",
                                    j->column_name ? j->column_name : "?");
        } else {
            const char *reason =
                j->unavailable_reason ? j->unavailable_reason : "unavailable";
            off += (size_t)snprintf(md + off, sizeof(md) - off,
                                    "| %s | — | %s: %s |\n",
                                    j->column_name ? j->column_name : "?",
                                    j->column_name ? j->column_name : "judge", reason);
        }
    }

    if (cfg->out_markdown) {
        hu_error_t e = write_text_file(cfg->out_markdown, md);
        if (e != HU_OK)
            return e;
    }

    if (cfg->out_json) {
        char json[4096];
        snprintf(json, sizeof(json),
                 "{\"summary\":\"%s\",\"available\":%zu,\"columns\":%zu}\n", out->summary, avail,
                 n_judges);
        hu_error_t e = write_text_file(cfg->out_json, json);
        if (e != HU_OK)
            return e;
    }

    if (avail < min_avail)
        return HU_ERR_NOT_SUPPORTED;
    (void)alloc;
    (void)cfg->prompt_fixture;
    return HU_OK;
}
