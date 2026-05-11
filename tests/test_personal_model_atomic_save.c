/* Phase 0 Task 6 — proves that hu_personal_model_save uses an atomic
 * tmp+rename pattern, so a crash mid-save can never leave the on-disk
 * file in a partially-written state.
 *
 * The naive fork+SIGKILL approach (proposed in the original plan) is
 * unreliable: by the time `raise(SIGKILL)` runs in the child, the save
 * has already returned and the file at <path> is already the new
 * intact file. The window between fwrite and intended rename is
 * microseconds, so stochastic SIGKILL during a tight loop catches it
 * only sometimes and gives flaky CI runs.
 *
 * Instead we use a deterministic FIX-SHAPE PROBE: pre-create
 * <path>.tmp as a directory before calling save. The atomic
 * implementation must open <path>.tmp via fopen("wb"), which fails
 * with EISDIR — so save returns an IO error AND leaves the original
 * <path> untouched. The non-atomic implementation calls
 * fopen(<path>, "wb") directly, which truncates <path> immediately
 * regardless of what's at <path>.tmp, and the file is corrupted
 * before any error is returned.
 *
 * This pins the contract: "if writing the new state fails for any
 * reason, the prior state is preserved." That contract is what
 * survives a real crash (SIGKILL, power loss, ENOSPC, etc.) — the
 * directory blocker is just a cheap, deterministic way to drive the
 * write-fail path.
 *
 * See spec §1.5.2 issue #4 and the May 11 2026 audit baseline. */

#include "test_framework.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void test_personal_model_save_preserves_prior_state_when_tmp_blocked(void) {
    char tmpl[] = "/tmp/hu_pm_atomic_XXXXXX";
    char *dir = mkdtemp(tmpl);
    HU_ASSERT_NOT_NULL(dir);

    char path[256];
    char tmp_path[260];
    snprintf(path, sizeof(path), "%s/personal_model.bin", dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    /* Step 1 — write a known-good model so <path> has a previous state to
     * preserve. fact_count = 1 with a recognizable subject so any later
     * mix-up (or a partial write that still happens to read back as a
     * valid header) shows up as an unexpected fact_count. */
    hu_personal_model_t known_good;
    hu_personal_model_init(&known_good);
    known_good.fact_count = 1;
    known_good.facts[0].confidence = 1.0f;
    snprintf(known_good.facts[0].subject, sizeof(known_good.facts[0].subject),
             "known-good-state");
    HU_ASSERT_EQ(hu_personal_model_save(&known_good, path), HU_OK);

    /* Step 2 — block the atomic tmp slot by creating a directory at
     * <path>.tmp. fopen("wb") on a directory always fails with EISDIR
     * (POSIX), which is what we want: it forces the atomic save into
     * its error path before any data has been written to <path>. */
    HU_ASSERT_EQ(mkdir(tmp_path, 0755), 0);

    /* Step 3 — try to overwrite with a much larger, distinguishable
     * model. After Task 7's fix, save returns HU_ERR_IO without
     * touching <path>. Pre-fix, save returns HU_OK because it opens
     * <path> directly via fopen("wb"), truncating the prior state. */
    hu_personal_model_t big;
    hu_personal_model_init(&big);
    big.fact_count = HU_PM_MAX_FACTS;
    for (size_t i = 0; i < big.fact_count; i++) {
        big.facts[i].confidence = 1.0f;
        snprintf(big.facts[i].subject, sizeof(big.facts[i].subject),
                 "fact-%zu", i);
    }
    /* We deliberately don't assert on the return value — both atomic and
     * non-atomic implementations could legitimately return HU_OK or
     * HU_ERR_IO depending on their internal layering. The contract
     * being tested is FILE STATE, not the error return. */
    (void)hu_personal_model_save(&big, path);

    /* Step 4 — the file at <path> must still contain the known-good
     * state. Pre-fix this fails because fopen(<path>, "wb") truncated
     * the file at the start of the save attempt. */
    hu_personal_model_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    HU_ASSERT_EQ(hu_personal_model_load(&loaded, path), HU_OK);
    HU_ASSERT_EQ(loaded.fact_count, (size_t)1);
    HU_ASSERT(strcmp(loaded.facts[0].subject, "known-good-state") == 0);

    /* Cleanup */
    (void)rmdir(tmp_path);
    (void)unlink(path);
    (void)rmdir(dir);
}

void run_personal_model_atomic_save_tests(void) {
    HU_TEST_SUITE("personal-model-atomic-save");
    HU_RUN_TEST(test_personal_model_save_preserves_prior_state_when_tmp_blocked);
}
