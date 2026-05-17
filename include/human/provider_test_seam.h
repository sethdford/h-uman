#ifndef HU_PROVIDER_TEST_SEAM_H
#define HU_PROVIDER_TEST_SEAM_H

/* HU_IS_TEST-only mock provider for eval stock-baseline tests. */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"

#ifdef HU_IS_TEST

hu_error_t hu_provider_create_for_test_with_canned_response(hu_allocator_t *alloc,
                                                            const char *canned,
                                                            hu_provider_t **out);
void hu_provider_destroy_for_test(hu_provider_t *provider, hu_allocator_t *alloc);
int hu_provider_unload_called_count_for_test(const hu_provider_t *provider);
int hu_provider_load_adapter_called_count_for_test(const hu_provider_t *provider);

#endif /* HU_IS_TEST */

#endif /* HU_PROVIDER_TEST_SEAM_H */
