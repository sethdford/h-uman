/* QMD retrieval: header and types compile; in SC_IS_TEST all logic is compiled out.
 * Functional tests not needed since code is SC_IS_TEST-guarded. */
#include "seaclaw/core/allocator.h"
#include "seaclaw/memory/retrieval/qmd.h"
#include "test_framework.h"
#include <string.h>

static void test_qmd_header_compiles(void) {
    /* Verify header exists and sc_qmd_keyword_candidates is callable. */
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_entry_t *out = NULL;
    size_t count = 99;
    sc_error_t err = sc_qmd_keyword_candidates(&alloc, "/tmp", 4, "query", 5, 10, &out, &count);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(count, 0u);
}

void run_qmd_tests(void) {
    SC_TEST_SUITE("QMD");
    SC_RUN_TEST(test_qmd_header_compiles);
}
