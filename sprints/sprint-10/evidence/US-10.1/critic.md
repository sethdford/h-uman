# Critic findings — US-10.1 SDK v0.2.0 surface

## CRITICAL (0)

## HIGH (2)

- `CMakeLists.txt:1415` — `libhuman_hula.dylib` exports **1,934 non-`hu_hula_*` text symbols** (e.g. `_hu_agent_*`, `_hu_anthropic_create`, `_hu_arena_*`, `_hu_affect_*`, etc.) because `human_hula SHARED` links `human_core` as a whole archive with no `fvisibility=hidden` on `human_hula` itself. The `-fvisibility=hidden` flags at lines 2112–2113 only apply to the `human` executable and `human_core` static lib **in Release/MinSizeRel builds**; the `dev` preset used by bindings development is `Debug`, so every TU pulled in transitively through `human_core` leaks its entire public symbol table into the SDK dylib. Any of these ~1,900 symbols becomes a de-facto ABI commitment the moment a language binding `dlsym`s it — and they will, because Python ctypes `CDLL` imports by name. Fix: add `set_target_properties(human_hula PROPERTIES C_VISIBILITY_PRESET hidden)` (and `VISIBILITY_INLINES_HIDDEN ON`) to the `human_hula` target unconditionally, then annotate the 13 intended-public `hu_hula_*` symbols with `__attribute__((visibility("default")))` via a `HU_HULA_SDK_API` macro in `hula_sdk.h`.

- `include/human/hula_sdk.h` (entire file) — No `extern "C"` guard. The header is the SDK surface document that US-10.2 (Python ctypes) and US-10.3 (TypeScript N-API) will include via `ffi.cdef` or native addon compilation. C++ callers (N-API addon code is C++) will see C++ linkage on the three `extern` declarations (`hu_hula_ctx_create`, `hu_hula_ctx_destroy`, `hu_hula_error_string`), producing mangled symbol names that do not match the `nm` output bindings will `dlsym`. Fix: wrap the non-inline declarations in `#ifdef __cplusplus extern "C" { ... } #endif`.

## MED (2)

- `src/agent/hula_sdk.c:197–199` — `HU_ERR_COUNT` is handled with `break` (falls through to `return "HU_ERR_UNKNOWN"`), which silences the `-Wswitch-enum` diagnostic for it. The comment at line 67 says `-Wswitch-enum` "still fires if a future enum value is added without a matching case." That is true for genuinely new values, but the sentinel `HU_ERR_COUNT` is already handled silently. If a downstream caller ever passes `HU_ERR_COUNT` (value equals the count of codes) expecting a distinct error string, it gets `"HU_ERR_UNKNOWN"` — same as garbage input — with no test distinguishing the two. The test at `tests/test_hula_sdk_v2.c:100` only probes value `9999`; it does not probe `HU_ERR_COUNT`. Low impact today; a binding that maps `HU_ERR_COUNT` as a sentinel will silently misname it. Fix: add a `case HU_ERR_COUNT: return "HU_ERR_COUNT";` arm and add a test that asserts `hu_hula_error_string(HU_ERR_COUNT)` equals `"HU_ERR_COUNT"`.

- `CMakeLists.txt:1384–1413` — The export anchor list names only 13 of the 40 exported `hu_hula_*` symbols. Symbols like `_hu_hula_exec_init_full`, `_hu_hula_exec_set_budget`, `_hu_hula_exec_cancel`, `_hu_hula_exec_set_spawn`, `_hu_hula_compiler_chat_compile_execute`, `_hu_hula_auto_verify`, `_hu_hula_emergence_scan`, etc. are exported by accident (pulled in transitively from `human_core`) rather than by explicit intent. The verifier recorded this as "40 >= 14 required; PASS" without flagging the 27 unintentional exports. Without visibility control (see HIGH-1), these become silent ABI commitments — removing any of them in v0.3.0 will be a breaking change. Fix: either add `fvisibility=hidden` + explicit `visibility("default")` annotations (closes HIGH-1 and this), or at minimum document each of the 40 in `bindings/sdk-changelog.md` as intended or unintended and file a story to trim before v0.3.0.

## LOW (0)

## Cross-agent regression risk

- `include/human/hula_sdk.h` will be included by US-10.2 (Python ctypes, `bindings/python/`) and US-10.3 (TypeScript N-API, `bindings/typescript/`). The missing `extern "C"` guard (HIGH-2) will cause a link error in the N-API C++ compilation before US-10.3 can produce a working `.node` addon. US-10.2's ctypes path bypasses this (pure `dlopen`), but N-API is blocked.

RESULT_critic=HAS_FINDINGS story=US-10.1 severity=HIGH
