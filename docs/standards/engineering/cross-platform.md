---
title: Cross-Platform Development
---

# Cross-Platform Development

Standards for platform-specific code patterns in the human C11 runtime.

**Cross-references:** [principles.md](principles.md), [testing.md](testing.md), [../operations/monitoring.md](../operations/monitoring.md)

---

## Supported Platforms

| Platform  | Arch            | CI Status    | Notes                                    |
| --------- | --------------- | ------------ | ---------------------------------------- |
| macOS     | aarch64         | Primary      | Primary development and benchmark target |
| Linux     | x86_64, aarch64 | Full CI      | Cross-compiled ARM64 for release         |
| Windows   | x86_64          | Partial      | Compiles; limited runtime testing        |
| WASM/WASI | wasm32          | Experimental | Via HU_BUILD_WASM flag                   |

## Platform Detection

Standard guards for platform-specific code:

```c
// Preferred: use human's platform macros
#if defined(__APPLE__) && defined(__MACH__)
    // macOS-specific code
#elif defined(_WIN32) || defined(_WIN64)
    // Windows-specific code
#elif defined(__linux__)
    // Linux-specific code
#endif

// Build-system flags for optional subsystems
#ifdef HU_GATEWAY_POSIX     // fork/exec, signals, cron (non-Windows)
#ifdef HU_BUILD_WASM        // WASM/WASI target
#ifdef HU_ENABLE_CURL       // libcurl HTTP client
#ifdef HU_ENABLE_SQLITE     // SQLite memory backend
```

## Platform Abstraction Layer

`src/app/platform.c` / `include/human/platform.h` provides cross-platform wrappers:

- `hu_platform_is_windows()` / `hu_platform_is_unix()` -- runtime detection
- `hu_platform_get_home_dir()` -- HOME or USERPROFILE
- `hu_platform_get_temp_dir()` -- /tmp or %TEMP%
- `hu_platform_get_shell()` / `hu_platform_get_shell_flag()` -- /bin/sh -c or cmd.exe /c
- `hu_platform_localtime_r()` / `hu_platform_gmtime_r()` -- thread-safe time
- `hu_platform_mkdir()` -- mode ignored on Windows
- `hu_platform_realpath()` -- resolves symlinks, handles Windows paths

## Rules

1. New platform-specific code MUST go through platform.h abstractions when possible
2. If no abstraction exists, use #ifdef guards (not runtime checks) for compile-time exclusion
3. Unsupported platforms return `HU_ERR_NOT_SUPPORTED` -- never silent no-ops
4. Guard naming: prefer `HU_*` build flags over raw compiler macros for subsystem features
5. `HU_IS_TEST` guard bypasses side effects on ALL platforms (network, spawning, hardware)
6. Tests must pass on both macOS and Linux; Windows is best-effort

## Platform-Specific Subsystems

| Subsystem            | Guard            | Platforms            | Fallback              |
| -------------------- | ---------------- | -------------------- | --------------------- |
| Sandbox (Landlock)   | **linux**        | Linux 5.13+          | HU_ERR_NOT_SUPPORTED  |
| Sandbox (Seatbelt)   | **APPLE**        | macOS                | HU_ERR_NOT_SUPPORTED  |
| Sandbox (Bubblewrap) | **linux**        | Linux                | HU_ERR_NOT_SUPPORTED  |
| iMessage channel     | **APPLE**        | macOS                | Channel not available |
| Gateway (fork/exec)  | HU_GATEWAY_POSIX | POSIX (macOS, Linux) | HU_ERR_NOT_SUPPORTED  |
| Cron scheduling      | HU_GATEWAY_POSIX | POSIX                | HU_ERR_NOT_SUPPORTED  |

## Hardcoded Paths

- Never hardcode `/tmp` -- use `hu_platform_get_temp_dir()`
- Never hardcode `~` -- use `hu_platform_get_home_dir()`
- Never hardcode `/bin/sh` -- use `hu_platform_get_shell()`

---

## Anti-Patterns

```c
// WRONG -- hardcode platform path
char *path = "/tmp/human-cache";
FILE *f = fopen(path, "w");

// RIGHT -- use platform abstraction
char *tmp = hu_platform_get_temp_dir(alloc);
if (tmp) {
    char full[512];
    snprintf(full, sizeof(full), "%s/human-cache", tmp);
    FILE *f = fopen(full, "w");
    hu_allocator_free(alloc, tmp);
}
```

```c
// WRONG -- silent no-op on unsupported platform
int run_sandbox(void) {
#ifdef __linux__
    return landlock_restrict();
#endif
    return 0;  /* Windows: silently does nothing */
}

// RIGHT -- explicit error on unsupported platform
hu_error_t run_sandbox(void) {
#ifdef __linux__
    return landlock_restrict();
#else
    return HU_ERR_NOT_SUPPORTED;
#endif
}
```

```c
// WRONG -- runtime check for compile-time platform
if (strcmp(platform, "darwin") == 0) {
    use_apple_api();
}

// RIGHT -- compile-time guard
#if defined(__APPLE__) && defined(__MACH__)
    use_apple_api();
#endif
```

```
WRONG -- use raw __linux__ for optional feature (curl, sqlite)
RIGHT -- use HU_ENABLE_CURL, HU_ENABLE_SQLITE build flags

WRONG -- assume /bin/sh exists on Windows
RIGHT -- use hu_platform_get_shell() and hu_platform_get_shell_flag()

WRONG -- skip HU_IS_TEST guard because "it works on my Mac"
RIGHT -- HU_IS_TEST bypasses side effects on all platforms
```
