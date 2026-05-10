#include "human/agent/verifier_metrics.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

bool hu_verifier_metrics_path(char *out, size_t cap) {
    if (!out || cap == 0)
        return false;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    int n = snprintf(out, cap, "%s/.human/verifier_metrics.json", home);
    return n > 0 && (size_t)n < cap;
}

hu_error_t hu_verifier_metrics_load(hu_verifier_metrics_t *out) {
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    char path[512];
    if (!hu_verifier_metrics_path(path, sizeof(path)))
        return HU_ERR_INVALID_ARGUMENT;
    FILE *f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? HU_ERR_NOT_FOUND : HU_ERR_IO;
    }
    /* Schema is fixed and tiny — just parse with sscanf instead of pulling
     * in the JSON parser. The order in the file mirrors the order written
     * by save(); a missing field falls through as 0 which matches the
     * "fresh install" semantic the doctor expects. */
    unsigned long long runs = 0, claims = 0, flagged = 0;
    long long last_epoch = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        (void)sscanf(line, " \"total_runs\": %llu", &runs);
        (void)sscanf(line, " \"total_claims_extracted\": %llu", &claims);
        (void)sscanf(line, " \"total_claims_flagged\": %llu", &flagged);
        (void)sscanf(line, " \"last_update_epoch\": %lld", &last_epoch);
    }
    fclose(f);
    out->total_runs = (uint64_t)runs;
    out->total_claims_extracted = (uint64_t)claims;
    out->total_claims_flagged = (uint64_t)flagged;
    out->last_update_epoch = (int64_t)last_epoch;
    return HU_OK;
}

hu_error_t hu_verifier_metrics_save(hu_verifier_metrics_t *metrics) {
    if (!metrics)
        return HU_ERR_INVALID_ARGUMENT;
    char path[512];
    if (!hu_verifier_metrics_path(path, sizeof(path)))
        return HU_ERR_INVALID_ARGUMENT;
    /* mkdir ~/.human (and HOME) idempotently; mirrors the imessage
     * poll-status writer so test fixtures with a fresh HOME don't fail. */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        (void)mkdir(home, 0700);
        char dir[512];
        int dn = snprintf(dir, sizeof(dir), "%s/.human", home);
        if (dn > 0 && (size_t)dn < sizeof(dir))
            (void)mkdir(dir, 0700);
    }
    metrics->last_update_epoch = (int64_t)time(NULL);
    FILE *f = fopen(path, "w");
    if (!f)
        return HU_ERR_IO;
    fprintf(f,
            "{\n"
            "  \"total_runs\": %llu,\n"
            "  \"total_claims_extracted\": %llu,\n"
            "  \"total_claims_flagged\": %llu,\n"
            "  \"last_update_epoch\": %lld\n"
            "}\n",
            (unsigned long long)metrics->total_runs,
            (unsigned long long)metrics->total_claims_extracted,
            (unsigned long long)metrics->total_claims_flagged,
            (long long)metrics->last_update_epoch);
    fclose(f);
    return HU_OK;
}

double hu_verifier_metrics_flagged_rate(const hu_verifier_metrics_t *m) {
    if (!m || m->total_claims_extracted == 0)
        return 0.0;
    return (double)m->total_claims_flagged / (double)m->total_claims_extracted;
}
