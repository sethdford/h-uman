#ifndef HU_CLI_EVAL_W16_INTERNAL_H
#define HU_CLI_EVAL_W16_INTERNAL_H

/* Test-only entry points for the W16 dispatcher attached to
 * `human eval --w16 ...`. The production CLI path inside cmd_eval calls
 * the dispatcher with a stack-local status struct that is freed before
 * cmd_eval returns; tests need to introspect that struct, so this header
 * exposes the struct shape and a thin wrapper that lets a test pass its
 * own status buffer.
 *
 * NOT a public API. Only included from `src/cli_commands.c` and the
 * matching test translation unit. */

#include "human/core/allocator.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_w16_cli_status {
    char *suite_name;
    bool suite_name_owned;
    hu_allocator_t *alloc;
    bool requested;
    bool offline;
    bool dispatched;
    bool report_emitted;
    hu_error_t status;
} hu_w16_cli_status_t;

/* Test entry. Mirrors the production dispatcher but takes an
 * out-parameter so tests can assert on the populated struct after the
 * call returns. The caller frees the struct via
 * `hu_w16_cli_status_free`. */
hu_error_t hu_cmd_eval_w16_dispatch_for_test(hu_allocator_t *alloc, int argc, char **argv,
                                             hu_w16_cli_status_t *status);

void hu_w16_cli_status_init(hu_w16_cli_status_t *s, hu_allocator_t *alloc);
void hu_w16_cli_status_free(hu_w16_cli_status_t *s);

#ifdef __cplusplus
}
#endif

#endif /* HU_CLI_EVAL_W16_INTERNAL_H */
