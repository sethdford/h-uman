/* Proactive check-in policy predicates — DDD Phase 2b characterization scaffold.
 *
 * Pure leaves extracted from hu_service_run_proactive_checkins so the test
 * suite can pin the giant's decision logic. Intentionally NOT wrapped in
 * #ifndef HU_IS_TEST — these compile in every build so tests reach them.
 * See include/human/daemon/proactive_policy.h for the rationale. */
#include "human/daemon/proactive_policy.h"

bool hu_daemon_proactive_is_social_hour(int hour) {
    return hour >= 9 && hour <= 21;
}

int hu_daemon_proactive_ymd_from_tm(const struct tm *tm) {
    if (!tm)
        return 0;
    return (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
}
