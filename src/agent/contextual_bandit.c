#include "human/agent/contextual_bandit.h"
#include "human/core/log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* FNV-1a hash for 64-bit contact_handle. */
static uint64_t fnv1a_hash(uint64_t x) {
    const uint64_t FNV_PRIME = 0x100000001B3ULL;
    const uint64_t FNV_OFFSET = 0xCBF29CE484222325ULL;
    uint64_t h = FNV_OFFSET;
    for (int i = 0; i < 8; i++) {
        h ^= (x >> (i * 8)) & 0xFF;
        h *= FNV_PRIME;
    }
    return h;
}

/* Linear-probe insertion. On collision, try (idx + 1) % capacity. */
static hu_contextual_bandit_arm_t *lookup_or_insert(hu_contextual_bandit_t *bandit, uint64_t handle,
                                                    hu_error_t *out_err) {
    uint64_t idx = fnv1a_hash(handle) % bandit->capacity;
    for (size_t i = 0; i < bandit->capacity; i++) {
        size_t probe = (idx + i) % bandit->capacity;
        if (bandit->arms[probe].contact_handle == 0 ||
            bandit->arms[probe].contact_handle == handle) {
            if (bandit->arms[probe].contact_handle == 0) {
                /* Initialize new arm */
                bandit->arms[probe].contact_handle = handle;
                bandit->arms[probe].alpha = 1.0;
                bandit->arms[probe].beta = 1.0;
                bandit->arms[probe].updates = 0;
                bandit->count++;
            }
            *out_err = HU_OK;
            return &bandit->arms[probe];
        }
    }
    *out_err = HU_ERR_OUT_OF_MEMORY; /* table full */
    return NULL;
}

/* Standard Exponential via LCG. Returns next sample and updates seed. */
static double standard_exponential(uint32_t *inout_seed) {
    uint32_t u = *inout_seed * 1103515245u + 12345u;
    *inout_seed = u;
    double u_norm = (double)u / (double)0x100000000ULL;
    if (u_norm < 1e-10)
        u_norm = 1e-10; /* avoid log(0) */
    return -log(u_norm);
}

/* Gamma(α, 1) using Marsaglia-Tsang for α ≥ 1. For α < 1, use exponential
 * with ratio-of-uniforms adjustment. */
static double gamma_sample_marsaglia(double alpha, uint32_t *inout_seed) {
    if (alpha < 1.0) {
        /* For α < 1, use exponential times (U^(1/α)). */
        double e = standard_exponential(inout_seed);
        uint32_t u = *inout_seed * 1103515245u + 12345u;
        *inout_seed = u;
        double u_norm = (double)u / (double)0x100000000ULL;
        if (u_norm < 1e-10)
            u_norm = 1e-10;
        return e * pow(u_norm, 1.0 / alpha);
    }

    /* Marsaglia method for α ≥ 1. */
    double d = alpha - 1.0 / 3.0;
    double c = 1.0 / sqrt(9.0 * d);

    for (int attempts = 0; attempts < 1000; attempts++) {
        /* Box-Muller: sample two exponentials for normal via
         * z = (E1 - E2) / sqrt(2) approximately. Simpler: use
         * (U1 - 0.5) as proxy for N(0,1). */
        uint32_t u1 = *inout_seed * 1103515245u + 12345u;
        *inout_seed = u1;
        uint32_t u2 = *inout_seed * 1103515245u + 12345u;
        *inout_seed = u2;
        double u1_norm = (double)u1 / (double)0x100000000ULL;
        double u2_norm = (double)u2 / (double)0x100000000ULL;

        /* Approximate normal from two uniforms. */
        double z = sqrt(-2.0 * log(u1_norm)) * cos(2.0 * 3.14159265358979323846 * u2_norm);

        double v = 1.0 + c * z;
        if (v > 0.0) {
            double v3 = v * v * v;
            uint32_t u3 = *inout_seed * 1103515245u + 12345u;
            *inout_seed = u3;
            double u3_norm = (double)u3 / (double)0x100000000ULL;
            if (u3_norm < 1e-10)
                u3_norm = 1e-10;

            double lhs = log(u3_norm);
            double rhs = 0.5 * z * z + d - d * v3 + d * log(v3);
            if (lhs < rhs) {
                return d * v3;
            }
        }
    }

    /* Fallback: exponential approximation. */
    return standard_exponential(inout_seed);
}

/* Public Beta sampler using two Gamma variates. */
double hu_contextual_bandit_sample_beta(double alpha, double beta, uint32_t *inout_seed) {
    double g_alpha = gamma_sample_marsaglia(alpha, inout_seed);
    double g_beta = gamma_sample_marsaglia(beta, inout_seed);
    double sum = g_alpha + g_beta;
    if (sum < 1e-10)
        return 0.5; /* Both tiny, neutral. */
    return g_alpha / sum;
}

hu_error_t hu_contextual_bandit_create(hu_allocator_t *alloc, size_t capacity,
                                       hu_contextual_bandit_t **out) {
    if (!alloc || !out || capacity == 0)
        return HU_ERR_INVALID_ARGUMENT;

    hu_contextual_bandit_t *bandit =
        (hu_contextual_bandit_t *)alloc->alloc(alloc->ctx, sizeof(*bandit));
    if (!bandit)
        return HU_ERR_OUT_OF_MEMORY;

    bandit->arms =
        (hu_contextual_bandit_arm_t *)alloc->alloc(alloc->ctx, capacity * sizeof(bandit->arms[0]));
    if (!bandit->arms) {
        alloc->free(alloc->ctx, bandit, sizeof(*bandit));
        return HU_ERR_OUT_OF_MEMORY;
    }

    memset(bandit->arms, 0, capacity * sizeof(bandit->arms[0]));
    bandit->alloc = alloc;
    bandit->capacity = capacity;
    bandit->count = 0;
    bandit->threshold = 0.3;

#ifdef HU_IS_TEST
    bandit->rng_seed = 42; /* Fixed seed for determinism in tests. */
#else
    bandit->rng_seed = (uint32_t)time(NULL);
#endif

    *out = bandit;
    return HU_OK;
}

void hu_contextual_bandit_destroy(hu_contextual_bandit_t *bandit) {
    if (!bandit)
        return;
    if (bandit->arms) {
        bandit->alloc->free(bandit->alloc->ctx, bandit->arms,
                            bandit->capacity * sizeof(bandit->arms[0]));
    }
    bandit->alloc->free(bandit->alloc->ctx, bandit, sizeof(*bandit));
}

hu_error_t hu_contextual_bandit_decide_send(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                            bool *out_should_send) {
    if (!bandit || !out_should_send)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err;
    hu_contextual_bandit_arm_t *arm = lookup_or_insert(bandit, contact_handle, &err);
    if (err != HU_OK)
        return err;

    double theta = hu_contextual_bandit_sample_beta(arm->alpha, arm->beta, &bandit->rng_seed);
    *out_should_send = (theta > bandit->threshold);
    return HU_OK;
}

hu_error_t hu_contextual_bandit_update(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                       hu_bandit_outcome_t outcome) {
    if (!bandit)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err;
    hu_contextual_bandit_arm_t *arm = lookup_or_insert(bandit, contact_handle, &err);
    if (err != HU_OK)
        return err;

    switch (outcome) {
    case HU_BANDIT_REPLY:
        arm->alpha += 1.0;
        break;
    case HU_BANDIT_IGNORED:
        arm->beta += 1.0;
        break;
    case HU_BANDIT_BLOCKED:
        arm->beta += 3.0;
        break;
    default:
        return HU_ERR_INVALID_ARGUMENT;
    }
    arm->updates++;
    return HU_OK;
}

hu_error_t hu_contextual_bandit_get_arm(hu_contextual_bandit_t *bandit, uint64_t contact_handle,
                                        hu_contextual_bandit_arm_t *out_arm) {
    if (!bandit || !out_arm)
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err;
    hu_contextual_bandit_arm_t *arm = lookup_or_insert(bandit, contact_handle, &err);
    if (err != HU_OK)
        return err;

    *out_arm = *arm;
    return HU_OK;
}

/* Serialization format (little-endian, no padding):
 * [0:4]   magic      = 0x425B4E44  ("BAN\0D" in little-endian)
 * [4:8]   version    = 1
 * [8:16]  num_arms   = count (uint64_t)
 * [16:24] rng_seed   = seed at save time (uint64_t)
 * [24:N]  for each arm:
 *   [0:8]   contact_handle (uint64_t)
 *   [8:16]  alpha (double, IEEE 754)
 *   [16:24] beta  (double, IEEE 754)
 *   [24:32] updates (uint64_t)
 */

#define HU_BANDIT_MAGIC   0x425B4E44UL
#define HU_BANDIT_VERSION 1UL

hu_error_t hu_contextual_bandit_save(hu_contextual_bandit_t *bandit, const char *path) {
    if (!bandit || !path)
        return HU_ERR_INVALID_ARGUMENT;

    /* Write to temp file first for atomic save. */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%u", path, (unsigned int)time(NULL));

    FILE *f = fopen(tmp_path, "wb");
    if (!f)
        return HU_ERR_IO;

    /* Write header. */
    uint32_t magic = HU_BANDIT_MAGIC;
    uint32_t version = HU_BANDIT_VERSION;
    uint64_t num_arms = bandit->count;
    uint64_t seed_save = bandit->rng_seed;

    if (fwrite(&magic, sizeof(magic), 1, f) != 1 || fwrite(&version, sizeof(version), 1, f) != 1 ||
        fwrite(&num_arms, sizeof(num_arms), 1, f) != 1 ||
        fwrite(&seed_save, sizeof(seed_save), 1, f) != 1) {
        fclose(f);
        remove(tmp_path);
        return HU_ERR_IO;
    }

    /* Write each arm. */
    for (size_t i = 0; i < bandit->capacity; i++) {
        if (bandit->arms[i].contact_handle == 0)
            continue; /* Skip uninitialized slots. */

        hu_contextual_bandit_arm_t *arm = &bandit->arms[i];
        if (fwrite(&arm->contact_handle, sizeof(arm->contact_handle), 1, f) != 1 ||
            fwrite(&arm->alpha, sizeof(arm->alpha), 1, f) != 1 ||
            fwrite(&arm->beta, sizeof(arm->beta), 1, f) != 1 ||
            fwrite(&arm->updates, sizeof(arm->updates), 1, f) != 1) {
            fclose(f);
            remove(tmp_path);
            return HU_ERR_IO;
        }
    }

    if (fflush(f) != 0 || fclose(f) != 0) {
        remove(tmp_path);
        return HU_ERR_IO;
    }

    /* Atomic rename. */
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        return HU_ERR_IO;
    }

    return HU_OK;
}

hu_error_t hu_contextual_bandit_load(hu_allocator_t *alloc, const char *path,
                                     hu_contextual_bandit_t **out) {
    if (!alloc || !path || !out)
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;

    /* Read header. */
    uint32_t magic, version;
    uint64_t num_arms, seed_load;

    if (fread(&magic, sizeof(magic), 1, f) != 1 || fread(&version, sizeof(version), 1, f) != 1 ||
        fread(&num_arms, sizeof(num_arms), 1, f) != 1 ||
        fread(&seed_load, sizeof(seed_load), 1, f) != 1) {
        fclose(f);
        return HU_ERR_IO;
    }

    if (magic != HU_BANDIT_MAGIC || version != HU_BANDIT_VERSION) {
        fclose(f);
        return HU_ERR_IO;
    }

    /* Create bandit with capacity for num_arms + slack. */
    size_t capacity = (num_arms > 0) ? (num_arms + 10) : 64;
    hu_contextual_bandit_t *bandit;
    hu_error_t err = hu_contextual_bandit_create(alloc, capacity, &bandit);
    if (err != HU_OK) {
        fclose(f);
        return err;
    }

    bandit->rng_seed = (uint32_t)seed_load;

    /* Read arms. */
    for (uint64_t i = 0; i < num_arms; i++) {
        hu_contextual_bandit_arm_t arm;
        if (fread(&arm.contact_handle, sizeof(arm.contact_handle), 1, f) != 1 ||
            fread(&arm.alpha, sizeof(arm.alpha), 1, f) != 1 ||
            fread(&arm.beta, sizeof(arm.beta), 1, f) != 1 ||
            fread(&arm.updates, sizeof(arm.updates), 1, f) != 1) {
            hu_contextual_bandit_destroy(bandit);
            fclose(f);
            return HU_ERR_IO;
        }

        /* Insert arm into the bandit. */
        hu_contextual_bandit_arm_t *slot;
        err = HU_OK;
        slot = lookup_or_insert(bandit, arm.contact_handle, &err);
        if (err != HU_OK) {
            hu_contextual_bandit_destroy(bandit);
            fclose(f);
            return err;
        }

        /* Copy over the loaded state. */
        slot->alpha = arm.alpha;
        slot->beta = arm.beta;
        slot->updates = arm.updates;
    }

    fclose(f);
    *out = bandit;
    return HU_OK;
}
