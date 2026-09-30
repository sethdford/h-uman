#ifndef HU_EVAL_COMMITMENT_SAMPLE_H
#define HU_EVAL_COMMITMENT_SAMPLE_H

/* Precision of commitment detection (life-admin slice 3).
 *
 * The morning briefing tells the owner "Dana told you: '…'", so every
 * detected commitment it shows must really be one. This measures that:
 * `human commitments sample` writes recent detections to a TSV with an empty
 * label column, the owner marks each row y or n, and `human commitments
 * score` reports precision per direction. No labels means no number: the
 * score says so rather than printing 0 or 100%. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/superhuman.h"
#include <stddef.h>
#include <stdio.h>

struct hu_persona;

typedef struct hu_commitment_score {
    size_t labeled_mine, correct_mine;     /* the owner promised */
    size_t labeled_theirs, correct_theirs; /* someone promised the owner */
    size_t labeled_dated, correct_dated;   /* rows with a deadline — the only ones the
                                            * morning briefing shows */
    size_t unlabeled;                      /* rows left blank */
    size_t bad_rows;                       /* unreadable label or direction */
} hu_commitment_score_t;

/* Write rows as the labelling sheet: comment lines, a header, then one
 * "label\tid\tdirection\tcontact\tdeadline\tsaid" row each with an empty
 * label. Tabs and newlines inside text become spaces. Contact names come
 * from the persona when known. Returns rows written, or -1 on write error. */
int hu_commitment_sample_write(FILE *f, const hu_superhuman_commitment_t *rows, size_t n,
                               const struct hu_persona *persona);

/* Score a labelled sheet. Labels: y/yes/1 = a real commitment, n/no/0 = not
 * one, blank = skipped. */
hu_error_t hu_commitment_sample_score(const char *tsv, size_t len, hu_commitment_score_t *out);

/* `human commitments sample|score …` */
hu_error_t cmd_commitments(hu_allocator_t *alloc, int argc, char **argv);

#endif /* HU_EVAL_COMMITMENT_SAMPLE_H */
