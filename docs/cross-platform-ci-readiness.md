# Cross-Platform CI Readiness

This document tracks platform support, known platform-specific code locations, and build flags for Windows, Linux, and macOS.

## Platforms Tested

| Platform                 | Status    | Notes                                           |
| ------------------------ | --------- | ----------------------------------------------- |
| **macOS aarch64**        | Confirmed | Primary development target, full test suite     |
| **Linux x86_64/aarch64** | CI        | `ci.yml` runs on Ubuntu                         |
| **Windows x86_64**       | Partial   | Builds; many features require POSIX (fork/exec) |

## Platform-Specific Code Locations

### Centralized Helpers (`src/platform.c`, `include/seaclaw/platform.h`)

All cross-platform time, sleep, and filesystem helpers live here:

- `sc_platform_localtime_r` / `sc_platform_gmtime_r` — thread-safe time conversion (localtime_s/gmtime_s on Windows)
- `sc_platform_sleep_ms` — Sleep(ms) on Windows, nanosleep on POSIX
- `sc_platform_mkdir` — \_mkdir on Windows, mkdir on POSIX
- `sc_platform_realpath` — \_fullpath on Windows, realpath on POSIX
- `sc_platform_parse_datetime` — sscanf fallback on Windows (strptime not available)
- `sc_platform_get_home_dir` — USERPROFILE/HOMEDRIVE+HOMEPATH on Windows, HOME on POSIX
- `sc_platform_get_temp_dir` — TEMP/TMP on Windows, TMPDIR/`/tmp` on POSIX

### POSIX-Only (by design)

| Path                         | Reason                                   |
| ---------------------------- | ---------------------------------------- |
| `src/security/landlock.c`    | Linux Landlock LSM                       |
| `src/security/bubblewrap.c`  | Linux bwrap                              |
| `src/security/seatbelt.c`    | macOS sandbox-exec                       |
| `src/security/firecracker.c` | Linux Firecracker VM                     |
| `src/core/process_util.c`    | fork/exec (SC_GATEWAY_POSIX)             |
| `src/tools/shell.c`          | fork/exec                                |
| `src/tools/git.c`            | fork/exec                                |
| `src/tools/browser.c`        | fork/exec                                |
| `src/channels/imessage.c`    | macOS JXA/osascript                      |
| `src/daemon.c`               | signal handling, cron (SC_GATEWAY_POSIX) |

### Hardcoded Paths (platform-specific)

| Location                     | Path                                   | Platform        |
| ---------------------------- | -------------------------------------- | --------------- |
| `src/security/seatbelt.c`    | `/tmp`, `/private/tmp`, `/var/folders` | macOS           |
| `src/security/firecracker.c` | `/var/lib/firecracker/*`               | Linux           |
| `src/security/bubblewrap.c`  | `/tmp`                                 | Linux           |
| `src/channels/email.c`       | `/tmp/sc_email_*`                      | POSIX (mkstemp) |
| `src/channels/nostr.c`       | `/tmp/sc_nostr_*`                      | POSIX (mkstemp) |

Use `sc_platform_get_temp_dir()` for portable temp paths.

## Build Flags per Platform

### macOS / Linux (POSIX)

```bash
cmake -B build -DSC_ENABLE_ALL_CHANNELS=ON -DSC_ENABLE_SQLITE=ON -DSC_ENABLE_PERSONA=ON
cmake --build build -j$(nproc)
```

- `SC_GATEWAY_POSIX=1` set when `UNIX AND NOT WIN32`
- Full fork/exec, signals, cron, TUI available

### Windows

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

- `SC_GATEWAY_POSIX` not set — process spawning stubbed
- Use `#if defined(_WIN32) && !defined(__CYGWIN__)` for Windows-specific code
- Cygwin treated as POSIX (excluded from Windows branches)

### WASM/WASI

```bash
cmake -B build-wasm -DCMAKE_TOOLCHAIN_FILE=../cmake/wasm32-wasi.cmake -DSC_BUILD_WASM=ON
```

## Guard Pattern

Use consistently:

```c
#if defined(_WIN32) && !defined(__CYGWIN__)
    /* Windows implementation */
#else
    /* POSIX implementation */
#endif
```

## Validation

Before commit:

```bash
cmake --build build -j$(nproc) && ./build/seaclaw_tests
```

All 3629+ tests must pass with 0 ASan errors.
