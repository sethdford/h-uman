#ifndef HU_ML_SCRIPTS_DIR_H
#define HU_ML_SCRIPTS_DIR_H

/* Phase D Task D-1 (CF-7) — popen relative-CWD hardening.
 *
 * Resolves the absolute path to a helper Python script under the project's
 * scripts/ directory, so MLX popen wrappers (dpo_real_mlx, kto_mlx, grpo_mlx,
 * reward_model_mlx) no longer invoke `python3 scripts/<name>.py` with a
 * CWD-relative path that an attacker could shadow by starting the daemon
 * from an attacker-controlled directory.
 *
 * Resolution order:
 *   1. getenv("HU_ML_SCRIPTS_DIR") if non-empty (highest priority — dev
 *      override, also used by tests to point at a temp dir for CWD-shadow
 *      regression tests).
 *   2. HU_ML_SCRIPTS_DIR compile-time macro (set by CMakeLists.txt to
 *      "${HU_ROOT}/scripts" for dev/test builds).
 *   3. getenv("HU_PROJECT_ROOT") + "/scripts" — compat with src/feeds/apple.c
 *      style resolution used elsewhere in the codebase.
 *
 * The resolver NEVER falls back to getcwd() — that is exactly the CF-7 bug
 * we are closing. If none of the three sources provide a path, the resolver
 * returns HU_ERR_NOT_SUPPORTED (and the caller fails loudly rather than
 * silently invoking a CWD-relative script).
 *
 * Single quotes in the resolved path are rejected because the popen command
 * strings single-quote the path; embedded single-quotes would break the
 * shell parse and could enable injection. (Legitimate paths don't contain
 * single quotes.)
 */

#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve absolute path to <scripts dir>/<script_name>.
 * - script_name: e.g. "grpo_mlx_train.py". MUST be non-NULL, non-empty,
 *   and free of single quotes / shell metacharacters; rejected with
 *   HU_ERR_INVALID_ARGUMENT otherwise.
 * - out: caller-allocated buffer of `cap` bytes; receives a NUL-terminated
 *   absolute path.
 * Returns:
 *   HU_OK                      — *out populated with absolute path.
 *   HU_ERR_INVALID_ARGUMENT    — null/empty/unsafe script_name, null out,
 *                                cap=0, or buffer too small for the result.
 *   HU_ERR_NOT_SUPPORTED       — no resolution source available
 *                                (no env vars set + no compile-time macro).
 */
hu_error_t hu_ml_resolve_script_path(const char *script_name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HU_ML_SCRIPTS_DIR_H */
