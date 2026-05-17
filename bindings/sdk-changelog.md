# HuLa SDK Changelog

All notable changes to the public HuLa SDK surface
(`include/human/hula_sdk.h` + the `libhuman_hula` shared library) are
documented here.

The SDK follows semver. **MAJOR** bumps signal breaking changes to any
signature in this file; **MINOR** bumps are additive only; **PATCH**
bumps are documentation or build-system fixes that cannot change a
caller's compile or link behavior.

Language bindings (`bindings/python/`, `bindings/typescript/`, ...) bind
to the symbols listed here at runtime via FFI and have **no compile-time
check** across that boundary. Any change to a published signature
therefore requires a MAJOR bump and a coordinated binding release.

---

## 0.2.0 — 2026-05-17 (Sprint 10, US-10.1)

Additive release. **Zero v0.1.0 signatures change, zero v0.1.0 helpers
are removed.** Embedders that compiled against v0.1.0 continue to
compile unchanged.

### Added

#### `typedef struct hu_hula_ctx hu_hula_ctx_t;`

Opaque per-binding context handle, forward-declared in the public
header; full layout lives in `src/agent/hula_sdk.c`.

- **Rationale:** language bindings (Python ctypes / TypeScript N-API /
  ...) want a single opaque pointer to attach a finalizer to. Threading
  an `hu_allocator_t *` through every call (the v0.1.0 pattern) works
  for C callers but is hostile to FFI.
- **ABI promise:** opaque. Bindings only see the typedef; the internal
  field layout can evolve in v0.3+ without bumping MAJOR.
- **Threading:** not thread-safe in v0.2.0. Bindings that need
  concurrency create one ctx per thread. Documented and out of scope
  for this sprint.

#### `hu_error_t hu_hula_ctx_create(hu_allocator_t *alloc, hu_hula_ctx_t **out);`

Allocate and initialize a new SDK context. The context stores `*alloc`
by value.

- **Rationale:** bindings need a constructor that maps cleanly to their
  language's "open a HulaContext()" idiom.
- **Error contract:** returns `HU_OK` and writes `*out` on success;
  returns `HU_ERR_INVALID_ARGUMENT` if `alloc`, `out`, `alloc->alloc`,
  or `alloc->free` is NULL; returns `HU_ERR_OUT_OF_MEMORY` if the
  underlying allocation fails. On any error, `*out` is **left
  unmodified** so callers may pre-seed it with a sentinel and trust it
  survives.
- **ABI promise:** signature stable in v0.2.x. A future v0.3 may add
  more constructors; this one will not change shape.

#### `void hu_hula_ctx_destroy(hu_hula_ctx_t *ctx);`

Destroy a context allocated by `hu_hula_ctx_create`.

- **Rationale:** symmetric finalizer for the constructor above.
  Bindings attach this to their language's garbage-collection /
  finalizer mechanism.
- **NULL-safe:** passing NULL is a no-op. Binding finalizers may run
  twice on a torn-down object, and that must not crash.
- **ABI promise:** signature stable in v0.2.x.

#### `const char *hu_hula_error_string(hu_error_t err);`

Map an `hu_error_t` value to a human-readable, NUL-terminated ASCII
string.

- **Rationale:** bindings render exception messages via this function
  rather than reimplementing the enum-to-string switch in every
  language. Single source of truth.
- **Return contract:**
  - For known codes (`HU_OK` and every `HU_ERR_*` declared in
    `human/core/error.h`): the C identifier as a string literal,
    e.g. `"HU_ERR_INVALID_ARGUMENT"`. Distinct codes return distinct
    strings.
  - For any value not covered by the switch: the sentinel literal
    `"HU_ERR_UNKNOWN"`.
  - Never NULL. Always NUL-terminated. Pointer is to a static literal
    and remains valid for the lifetime of the program.
- **Why an extern symbol, not `static inline`:** v0.1.0 helpers are
  inline because they have no `.c` companion and need no link. Inlining
  a 60-case switch into every TU would bloat embedders' binaries; one
  extern symbol gives FFI a single `dlsym` target.
- **ABI promise:** signature stable in v0.2.x. The set of *return
  values* may grow (new error codes → new identifier strings) but the
  sentinel `"HU_ERR_UNKNOWN"` and the "never NULL" guarantee are
  permanent.

### Added — build

#### `libhuman_hula.{so,dylib}`

New shared-library build target wired into `CMakeLists.txt`. Bindings
load this at runtime via `ctypes.CDLL` / `koffi.load` / `process.dlopen`
and bind to the symbols above via FFI.

- **Rationale:** before v0.2.0, embedders linked the static `human_core`
  archive directly. ctypes / N-API need a real `.so` / `.dylib`.
- The shared library uses whole-archive linking against `human_core` so
  every `hu_hula_*` symbol the bindings need (parse, validate, exec,
  serialize, ...) is reachable in addition to the three new v0.2.0
  symbols.
- `human_core` is now compiled with `POSITION_INDEPENDENT_CODE ON` so
  this link works on Linux and macOS.

### Unchanged from 0.1.0

- `hu_hula_sdk_call`, `hu_hula_sdk_sequence`, `hu_hula_sdk_run_json`
  remain `static inline` in `hula_sdk.h`. No behavior or signature
  change.
- All `hu_hula_*` IR symbols in `human/agent/hula.h` are untouched.
- Version macros are bumped (`HU_HULA_SDK_VERSION_MINOR` 1 → 2,
  `HU_HULA_SDK_VERSION_STRING` "0.1.0" → "0.2.0") but the MAJOR
  number is unchanged.

### Removed

Nothing.

---

## 0.1.0 — initial release

- Inline ergonomic helpers (`hu_hula_sdk_call`, `hu_hula_sdk_sequence`,
  `hu_hula_sdk_run_json`).
- Re-export of `human/agent/hula.h` (the underlying IR header).
- Version macros.
