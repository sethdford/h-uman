#ifndef HU_VERSION_H
#define HU_VERSION_H

/** human version string (e.g. "0.3.0"). */
const char *hu_version_string(void);

/**
 * Git commit SHA the binary was built from (40 lowercase hex chars), or
 * "unknown" when the build tree was not a git checkout. Stamped at build
 * time by cmake/GenGitSha.cmake; the installer uses it to refuse deploying
 * a binary whose commit is not a descendant of the installed one.
 */
const char *hu_build_sha(void);

#endif
