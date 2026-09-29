#include "human/version.h"

#ifndef HU_VERSION
#define HU_VERSION "0.5.0"
#endif

/* Generated at build time by cmake/GenGitSha.cmake (defines HU_GIT_SHA).
 * Guarded so non-CMake builds (or first-pass tooling like clangd against a
 * stale index) still compile — they fall back to "unknown", which the
 * installer treats as advisory rather than a hard refusal. */
#if defined(__has_include)
#if __has_include("hu_git_sha.h")
#include "hu_git_sha.h"
#endif
#endif

#ifndef HU_GIT_SHA
#define HU_GIT_SHA "unknown"
#endif

/* The "HU_BUILD_SHA=" prefix is a stable extraction anchor: the installer
 * greps `strings` output for it, so provenance survives even when the
 * binary cannot be executed on the probing machine. */
#define HU_BUILD_SHA_PREFIX "HU_BUILD_SHA="
static const char hu_build_sha_tagged[] = HU_BUILD_SHA_PREFIX HU_GIT_SHA;

const char *hu_version_string(void) {
    return HU_VERSION;
}

const char *hu_build_sha(void) {
    return hu_build_sha_tagged + sizeof(HU_BUILD_SHA_PREFIX) - 1;
}
