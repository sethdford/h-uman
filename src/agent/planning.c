#include "human/agent/planning.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#define HU_PLANNING_ESCAPE_BUF 2048

const char *hu_plan_status_str(hu_plan_status_t status) {
    switch (status) {
    case HU_PLAN_PROPOSED:
        return "proposed";
    case HU_PLAN_ACCEPTED:
        return "accepted";
    case HU_PLAN_CONFIRMED:
        return "confirmed";
    case HU_PLAN_COMPLETED:
        return "completed";
    case HU_PLAN_CANCELLED:
        return "cancelled";
    case HU_PLAN_DECLINED:
        return "declined";
    case HU_PLAN_EXPIRED:
        return "expired";
    default:
        return "proposed";
    }
}

bool hu_plan_status_from_str(const char *str, hu_plan_status_t *out) {
    if (!str || !out)
        return false;
    if (strcmp(str, "proposed") == 0) {
        *out = HU_PLAN_PROPOSED;
        return true;
    }
    if (strcmp(str, "accepted") == 0) {
        *out = HU_PLAN_ACCEPTED;
        return true;
    }
    if (strcmp(str, "confirmed") == 0) {
        *out = HU_PLAN_CONFIRMED;
        return true;
    }
    if (strcmp(str, "completed") == 0) {
        *out = HU_PLAN_COMPLETED;
        return true;
    }
    if (strcmp(str, "cancelled") == 0) {
        *out = HU_PLAN_CANCELLED;
        return true;
    }
    if (strcmp(str, "declined") == 0) {
        *out = HU_PLAN_DECLINED;
        return true;
    }
    if (strcmp(str, "expired") == 0) {
        *out = HU_PLAN_EXPIRED;
        return true;
    }
    return false;
}