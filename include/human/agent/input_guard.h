#ifndef HU_INPUT_GUARD_H
#define HU_INPUT_GUARD_H
#include "human/core/error.h"
#include <stddef.h>
typedef enum hu_injection_risk {
    HU_INJECTION_SAFE = 0,
    HU_INJECTION_SUSPICIOUS = 1,
    HU_INJECTION_HIGH_RISK = 2,
} hu_injection_risk_t;
hu_error_t hu_input_guard_check(const char *message, size_t message_len,
                                hu_injection_risk_t *out_risk);
#endif
