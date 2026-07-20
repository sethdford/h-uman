# src/core/ — Core Utilities

Foundational libraries used across the entire codebase. These are the lowest-level building blocks — everything depends on them, they depend on nothing else.

## Modules

```
allocator.c          Custom allocator (hu_allocator_t) — wraps malloc/free with tracking
arena.c              Arena allocator — bulk allocation with single-free semantics
error.c              Error codes (hu_error_t) and hu_error_string() formatting
file_util.c          Whole-file read helper (hu_file_read_all) — replaces the fopen/fseek/ftell clone
json.c               JSON parser and builder (zero-dependency, streaming)
string.c             String utilities (hu_string_t, split, join, trim, format)
http.c               HTTP client abstraction (wraps libcurl when available)
process_util.c       Process utilities (spawn, wait, signal handling)
```

## Key Types

```c
hu_allocator_t   — allocator interface (alloc, realloc, free, ctx)
hu_error_t       — error code enum (HU_OK, HU_ERR_*, see docs/error-codes.md)
hu_string_t      — length-prefixed string (data, len)
```

## Dependencies

- `allocator.c` and `arena.c` — no dependencies beyond libc
- `error.c` — no dependencies
- `json.c` — depends on allocator
- `string.c` — depends on allocator
- `http.c` — depends on allocator, conditionally on libcurl (`HU_ENABLE_CURL`)
- `process_util.c` — depends on POSIX APIs (`fork`, `exec`, `waitpid`)

## Rules

- These files are included by nearly every other module — changes have maximum blast radius
- Never add external dependencies to core modules
- `allocator.c`: every `alloc` must have a matching `free`
- `arena.c`: arena_destroy frees everything — don't free individual allocations
- `json.c`: parser must handle malformed input without crashing (fuzz-tested)
- `string.c`: all functions must handle NULL input and zero-length strings
- `error.c`: add new error codes in `include/human/core/error.h`, update `docs/error-codes.md`
- `http.c`: HTTPS-only, never log request/response bodies
- Maintain strict C11 compliance — no compiler extensions
- All functions must be testable with `HU_IS_TEST` (no real network in tests)
