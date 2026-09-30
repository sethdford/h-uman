#ifndef HU_CLI_PROSPECTIVE_H
#define HU_CLI_PROSPECTIVE_H
/* `human prospective` — the probe and one-time backfill for prospective
 * memory v2 (docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md
 * §4.5, rollout step 2). src/app/cli_prospective.c. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef enum hu_cli_prospective_op {
    HU_CLI_PM_NONE = 0,
    HU_CLI_PM_INIT,     /* open (create + migrate) --db, print "ok" */
    HU_CLI_PM_INBOUND,  /* probe --inbound TEXT: keyword pass */
    HU_CLI_PM_TICK,     /* probe --tick: time pass */
    HU_CLI_PM_DELIVER,  /* probe --deliver TEXT: after-delivery evidence */
    HU_CLI_PM_BACKFILL, /* backfill [--write] */
} hu_cli_prospective_op_t;

typedef struct hu_cli_prospective_args {
    hu_cli_prospective_op_t op;
    const char *db;           /* required: the probe never defaults to ~/.human */
    const char *contact;      /* required for probe */
    const char *text;         /* --inbound / --deliver */
    const char *history_path; /* --history FILE, "them: …\nme: …" lines */
    const char *judge;        /* a verdict word or "model"; default "not_now" */
    long long now;            /* --now EPOCH; 0 = time(NULL) */
    bool full, shadow, group, self, write;
} hu_cli_prospective_args_t;

/* argv[0]="human", argv[1]="prospective", argv[2]=init|probe|backfill.
 * Rejects: no --db, a probe without exactly one of --inbound/--tick/
 * --deliver or without --contact, an unknown flag, a flag missing its value,
 * a --judge that is neither a verdict word nor "model", a bad --now. */
bool hu_cli_prospective_parse(int argc, char **argv, hu_cli_prospective_args_t *out);

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/prospective_v2.h"
/* Runs a parsed probe/init/backfill against an open memory and writes the
 * output contract (see the plan's Task 6 Interfaces) to `out`. */
hu_error_t hu_cli_prospective_run(hu_allocator_t *alloc, hu_memory_t *mem,
                                  const hu_cli_prospective_args_t *a, const char *history,
                                  size_t history_len, const hu_prospective_judge_t *judge,
                                  FILE *out);
/* Reads the LAST up to `cap - 1` bytes of the file at `path` into a buffer
 * of `cap` bytes allocated from `alloc`, NUL-terminated. `--history` wants
 * the most-recent turns, not the earliest ones, so a file over `cap` is
 * read from its tail, never its head. NULL on any I/O error or bad
 * argument (`*len` set to 0). Exposed (not just used internally by
 * cmd_prospective) so this tail-vs-head contract has a direct test. */
char *hu_cli_prospective_read_tail(hu_allocator_t *alloc, const char *path, size_t cap,
                                   size_t *len);
#endif

#endif /* HU_CLI_PROSPECTIVE_H */
