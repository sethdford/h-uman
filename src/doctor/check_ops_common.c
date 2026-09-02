/* src/doctor/check_ops_common.c — shared helpers for the operational-truth
 * doctor checks (path resolution under $HOME). One place, so the five checks
 * do not each carry the same prologue. */
#include "human/doctor/check_ops.h"

#include <stdio.h>
#include <stdlib.h>

const char *hu_doctor_ops_home_path(const char *explicit, char *buf, size_t cap, const char *rel) {
    if (explicit)
        return explicit;
    const char *home = getenv("HOME");
    if (!home || !*home || !rel)
        return NULL;
    if (snprintf(buf, cap, "%s/%s", home, rel) >= (int)cap)
        return NULL;
    return buf;
}

hu_doctor_check_result_t hu_doctor_ops_result(hu_doctor_verdict_t verdict, const char *reason,
                                              const char *detail_json) {
    return (hu_doctor_check_result_t){verdict, reason, detail_json};
}
