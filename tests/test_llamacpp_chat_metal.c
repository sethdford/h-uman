/* Phase 1 (RL SOTA) — link-mirror pin test.
 *
 * Why this exists: CMakeLists.txt wires the vendored llama.cpp library
 * into BOTH human_core (production) AND human_core_test (test build).
 * The test-side wiring is fragile — if a future refactor drops
 * `target_link_libraries(human_core_test PRIVATE llama)` from
 * CMakeLists.txt, the production build still works but EVERY test that
 * ever calls a llama_* symbol from a test source will silently fail to
 * link. This file exists to fail loudly in that case.
 *
 * The single test calls llama_print_system_info() — a cheap, side-
 * effect-free llama symbol — purely to prove the link landed. If the
 * test binary still links, the mirror works.
 *
 * Coverage for actual chat-with-Metal correctness lives in
 * tests/test_llamacpp_lora_hotswap.c (which loads Gemma, runs three
 * real chat calls, and asserts behavior). This file is the link
 * sentinel; that file is the behavior pin.
 *
 * Skip semantics: the body is gated behind HU_ENABLE_LLAMACPP. When
 * the umbrella build is run without llama.cpp linked (the default dev
 * preset), the runner just prints "[skip]" and returns. With the
 * rl_sota preset (HU_ENABLE_LLAMACPP=ON), the test executes and
 * either passes (link works) or fails to link entirely (link broken).
 */

#include "test_framework.h"

#include <stdio.h>

/* The CI feature-flags matrix passes -DHU_ENABLE_LLAMACPP=ON to
 * exercise the link path, but Linux runners don't init the vendored
 * `third_party/llama.cpp` submodule (size). When llama.cpp's header
 * isn't reachable, the CMake discovery chain at CMakeLists.txt:1605
 * falls through to the stub provider with empty HU_LLAMACPP_INCLUDE_DIRS,
 * so the test compiles but emits NOT_SUPPORTED at runtime. Guard the
 * llama.h include with __has_include so we don't fatal-error in that
 * path while still proving the link mirror works when the header IS
 * available (rl_sota preset locally, Apple macOS + Metal CI). */
#if defined(HU_ENABLE_LLAMACPP) && defined(__has_include)
#  if __has_include("llama.h")
#    include "llama.h"
#    define HU_TEST_LLAMA_H_AVAILABLE 1
#  endif
#endif

static void test_human_core_test_links_llama_when_enabled(void) {
#if defined(HU_ENABLE_LLAMACPP) && defined(HU_TEST_LLAMA_H_AVAILABLE)
    /* Calling any llama_* symbol from this test object proves the
     * test-binary link mirror at CMakeLists.txt:2191-2211 is intact.
     * llama_print_system_info() is a one-line printf-style helper
     * that needs no model and has no side effects beyond returning a
     * pointer to a static string. */
    const char *info = llama_print_system_info();
    HU_ASSERT_NOT_NULL(info);
    /* Sanity-check the string isn't empty — proves the function
     * actually executed and didn't get inlined to a NULL stub. */
    HU_ASSERT_TRUE(info[0] != '\0');
#elif defined(HU_ENABLE_LLAMACPP)
    fprintf(stderr, "[skip] HU_ENABLE_LLAMACPP defined but llama.h "
                    "not reachable — submodule not initialized\n");
#else
    fprintf(stderr, "[skip] HU_ENABLE_LLAMACPP not defined — link-mirror "
                    "test only meaningful in rl_sota preset\n");
#endif
}

void run_llamacpp_chat_metal_tests(void) {
    HU_RUN_TEST(test_human_core_test_links_llama_when_enabled);
}
