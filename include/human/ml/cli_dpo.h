/* include/human/ml/cli_dpo.h — Phase 2 Task 8
 *
 * Splits the legacy `human ml dpo-train` CLI verb into two handlers:
 *
 *   - hu_ml_cli_dpo_judge: the Phase 0 provider-scored "judge step" path
 *     (was the body of hu_ml_cli_dpo_train pre-Phase 2). Reachable via
 *     `human ml dpo-judge` or via `human ml dpo-train --legacy-judge`.
 *
 *   - hu_ml_cli_dpo_real:  the Phase 2 real-DPO path. Dispatches to the
 *     hu_rl_trainer_t vtable from Tasks 1/4/6 (HUML in-process backend
 *     or MLX subprocess backend). Default route for `human ml dpo-train`.
 *
 * Plan deviation note: the canonical plan snippet (lines 1951–1959)
 * `#include`s `"human/allocator.h"` / `"human/error.h"`. Those headers
 * do not exist in this repo — the real paths are `"human/core/allocator.h"`
 * and `"human/core/error.h"` (matches the convention used by every other
 * Phase 2 header, e.g. include/human/ml/rl_trainer.h:21–22).
 */
#ifndef HU_ML_CLI_DPO_H
#define HU_ML_CLI_DPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

hu_error_t hu_ml_cli_dpo_judge(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_dpo_real(hu_allocator_t *alloc, int argc, const char **argv);

#ifdef __cplusplus
}
#endif
#endif /* HU_ML_CLI_DPO_H */
