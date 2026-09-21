#ifndef HU_COMPUTER_USE_H
#define HU_COMPUTER_USE_H
#include "human/provider.h"
#include "human/security.h"
#include "human/tool.h"

typedef enum {
    HU_CU_SCREENSHOT = 0,
    HU_CU_CLICK,
    HU_CU_TYPE,
    HU_CU_SCROLL,
    HU_CU_NAVIGATE,
    HU_CU_KEY
} hu_computer_use_action_t;

/**
 * Create the computer_use tool. In non-test builds, @p policy must be non-NULL and
 * policy->autonomy must be >= HU_AUTONOMY_SUPERVISED. Test builds ignore autonomy when
 * returning mocks.
 */
hu_error_t hu_computer_use_create(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                  hu_tool_t *out);

/* Write a PNG screenshot to @p path (validated like tool path). For daemon/visual pipeline.
 * Non-test macOS: requires supervised+ autonomy. Test builds return failure (use visual mocks). */
hu_error_t hu_computer_use_screenshot_to_path(hu_allocator_t *alloc, hu_security_policy_t *policy,
                                              const char *path, hu_tool_result_t *out);

#endif
