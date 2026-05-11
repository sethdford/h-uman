---
title: "W0b — MCP type rename precondition (hu_mcp_server_t → hu_mcp_client_t; hu_mcp_host_t → hu_mcp_engine_t)"
date: 2026-05-11
sprint: SOTA-2026-01
slice: W0b
status: complete
risk: low
---

# W0b — MCP type rename precondition report

This slice closes the API-contract-watcher finding that init #12 (MCP server
mode) wants the symbol `hu_mcp_server_t` for its new public server vtable,
but the symbol is already in use as the MCP **client** type in
`include/human/mcp.h`. It also closes the secondary collision found by the
adversarial-review critic: init #12 wants `hu_mcp_server.h` for its new
public surface, but the file currently houses the `hu_mcp_host_t` server-
mode engine.

Scope is **rename-only**. No new server-mode behaviour is introduced here —
init #12 will land that in its own implementation slice on top of this
precondition.

## 1. Typedefs before / after

| Old name | New name | Header | Notes |
|---|---|---|---|
| `hu_mcp_server_t` | `hu_mcp_client_t` | `include/human/mcp.h` | The MCP client connection (child-process stdio JSON-RPC). The old name is now **free** for init #12. |
| `hu_mcp_server_config_t` | `hu_mcp_client_config_t` | `include/human/mcp.h` | Renamed for symmetry; init #12 will want `hu_mcp_server_config_t` / `hu_mcp_server_options_t` for the new vtable. |
| `hu_mcp_server_create / _connect / _list_tools / _call_tool / _destroy / _reconnect / _refresh_tools` | `hu_mcp_client_create / _connect / _list_tools / _call_tool / _destroy / _reconnect / _refresh_tools` | `include/human/mcp.h` | Public function family. |
| `hu_mcp_host_t` | `hu_mcp_engine_t` | `include/human/mcp_server.h` | The JSON-RPC dispatcher inside h-uman that serves tools to MCP clients. Per init #12 design intent, this is exactly the "engine underneath the new server vtable". |
| `hu_mcp_host_create / _set_resources / _set_prompts / _run / _destroy` | `hu_mcp_engine_create / _set_resources / _set_prompts / _run / _destroy` | `include/human/mcp_server.h` | Public function family. Deprecation shims left for the old names — see §3. |

### Left intentionally unrenamed

| Symbol | Why kept |
|---|---|
| `hu_mcp_server_entry` (`include/human/config.h`) | User-config entry describing an external MCP server (the remote thing we connect to). Name correctly describes a server. Does not collide with `hu_mcp_server_t`. |
| `hu_mcp_server_info_t` (`include/human/mcp_manager.h`) | Info struct describing a connected external MCP server. Same reasoning. Does not collide with `hu_mcp_server_t`. |
| `hu_mcp_init_tools` / `hu_mcp_free_tools` | Module-level helpers that don't carry "server" or "host" in their names. |

The freed slot is specifically the `hu_mcp_server_t` **typedef** identifier
plus the matching `hu_mcp_server_*` function family on the client struct.
The string token `hu_mcp_server` still legitimately prefixes config / info
structs that describe external servers, and init #12 can freely add new
`hu_mcp_server_*` symbols on top of that prefix without collision.

## 2. Call-site coverage

Fresh grep counts after the rename:

| Pattern | Hits in `*.c`/`*.h` | Where |
|---|---|---|
| `hu_mcp_server_t` (identifier) | **0** code references | 2 comment-only mentions in `mcp.h` + `mcp_server.h` (historical / forward-pointer comments). |
| `hu_mcp_server_config_t` | **0** | All call sites migrated to `hu_mcp_client_config_t`. |
| `hu_mcp_server_{create,connect,destroy,list_tools,call_tool,reconnect,refresh_tools}` | **0** code references | All renamed to `hu_mcp_client_*`. |
| `hu_mcp_host_t` (identifier) | **2** — the `typedef hu_mcp_engine_t hu_mcp_host_t;` shim line + 1 comment in `mcp_server.h`. | No `.c` callers. |
| `hu_mcp_host_{create,set_resources,set_prompts,run,destroy}` | **5** — the five `static inline` deprecation shim definitions in `mcp_server.h`. | No external `.c` callers. |
| `hu_mcp_client_t` | **23** (typedef + 7 public signatures in `mcp.h` + ~15 call sites in `src/mcp.c`/`src/mcp_manager.c` + tests) | All migrated. |
| `hu_mcp_engine_t` | **19** (typedef + 5 public signatures + ~13 in `src/mcp_server.c` + 1 in `src/main.c` + tests) | All migrated. |

The api-contract-watcher's pre-slice estimate of "28 `hu_mcp_server_t` call
sites + 5 `hu_mcp_host_*` call sites in main.c" matched what fresh grep
found. All 33 are now either migrated to the new names or forwarded
through a deprecation shim.

### Deprecation shims emitted

`include/human/mcp_server.h` ships five `static inline` forwarders, each
tagged `__attribute__((deprecated("renamed to hu_mcp_engine_<verb>")))`:

- `hu_mcp_host_create` → `hu_mcp_engine_create`
- `hu_mcp_host_set_resources` → `hu_mcp_engine_set_resources`
- `hu_mcp_host_set_prompts` → `hu_mcp_engine_set_prompts`
- `hu_mcp_host_run` → `hu_mcp_engine_run`
- `hu_mcp_host_destroy` → `hu_mcp_engine_destroy`

The typedef `typedef hu_mcp_engine_t hu_mcp_host_t;` keeps the old type
name resolvable so out-of-tree call sites that still spell
`hu_mcp_host_t *srv` keep compiling for one release.

The `deprecated` attribute is feature-gated on `__GNUC__ || __clang__`. On
toolchains that lack the attribute the macro expands to nothing — source
compatibility survives, only the deprecation **warning** is dropped.

**No deprecation shim for `hu_mcp_server_t` is provided** (intentional, per
mandate): that name must be free for init #12, so reintroducing it as a
typedef alias would re-collide with the upcoming server vtable.

## 3. Build / test gates

```text
cmake --preset dev                                      # 0 errors
cmake --build --preset dev -j$(sysctl -n hw.ncpu)       # 0 errors, -Werror clean
./build/human_tests                                     # 9715/9715 PASS, 0 ASan leaks
```

Targeted spot-checks:

```text
./build/human_tests --suite=MCP             # 135 / 135 PASS
./build/human_tests --filter=mcp_client     #  27 /  27 PASS
./build/human_tests --filter=mcp_engine     #  24 /  24 PASS
```

### -Werror cleanliness of the shim

The deprecation shim was verified to emit only `-Wdeprecated-declarations`,
which is **not** in the project's `-Werror` set:

```text
$ clang -Iinclude -fsyntax-only -Wdeprecated-declarations \
        /tmp/w0b_shim_check.c
warning: 'hu_mcp_host_destroy' is deprecated:
         renamed to hu_mcp_engine_destroy [-Wdeprecated-declarations]
1 warning generated.   # build still succeeds
```

Internal callers have all been migrated to `hu_mcp_engine_*`, so the full
internal build emits **zero** deprecation warnings. The shim is purely a
courtesy for out-of-tree consumers during one release cycle.

## 4. Files modified

W0b-owned (mechanical rename only):

```text
M  include/human/mcp.h
M  include/human/mcp_server.h
M  src/mcp.c
M  src/mcp_manager.c
M  src/mcp_server.c
M  src/main.c                 (5-call MCP block at lines 2407–2421)
M  src/tools/factory.c        (one type-name update on the local mcp_configs[] array)
M  tests/test_mcp.c           (client + engine tests; mock vtable + suite labels)
M  tests/test_modules_coverage.c   (two client test functions + their HU_RUN_TEST lines)
M  PROJECT_STATUS.md          (status line for engine server mode)
```

No new files. No deletions. No CMakeLists.txt changes (no new sources to
register).

### Not touched by W0b (but present in the dirty tree)

The repository was already heavily dirty before this slice started — W7/W9
bridge work, persona-context wiring, sprint-1 evidence logs, plan-doc
revisions, and parallel ML work were already in flight. None of those
files contain MCP type references, so W0b deliberately did **not** touch
them. They remain in their pre-slice state.

Additional concurrent agents continued modifying shared files (notably
`CMakeLists.txt`, `src/agent/world_model_bridge.c`, `src/agent/agent_stream.c`,
`src/persona/examples.c`, and a transient `src/providers/llamacpp_decode.c`)
during the slice. None overlap with the MCP rename. The final build/test
pass was performed against the latest tree state with all parallel work
still in place, confirming the rename composes cleanly with the surrounding
W7/W9/persona work.

## 5. Confirmation: `hu_mcp_server_t` slot is free for init #12

```text
$ rg -n "hu_mcp_server_t\b" --type c --type h
include/human/mcp_server.h:17: *   `hu_mcp_server_t` vtable that layers consent + audit + rate-limit policy
include/human/mcp.h:15:        * Historical note: this type used to be called `hu_mcp_server_t`. That name

$ rg -n "typedef.*hu_mcp_server\b" --type c --type h
(no matches)

$ rg -n "struct hu_mcp_server\b" --type c --type h
(no matches)
```

Both remaining matches are documentation comments pointing forward to
init #12 and backward to the historical client type. The C identifier
`hu_mcp_server_t` resolves to **nothing** in the post-W0b tree, so init
#12's design (which intends to introduce
`typedef struct hu_mcp_server { void *ctx; const hu_mcp_server_vtable_t *vtable; } hu_mcp_server_t;`)
can land without symbol collision.

## 6. Open follow-ups (out of scope for W0b)

1. Init #12's M0 milestone can now ship the new server vtable + its own
   `hu_mcp_server_config_t` / `hu_mcp_server_options_t` types under the
   freed name.
2. The five `hu_mcp_host_*` deprecation shims should be removed at the
   next major version bump after init #12 ships — track in init #12's
   "remove deprecation shims" task.
3. The doc-fleet should pick up a one-line update to
   `docs/plans/2026-05-11-init-12-mcp-server-mode.md` § "M0 precondition"
   noting that this slice has landed (the typedef shim it originally
   specified for `hu_mcp_server_t` is intentionally **omitted** here, per
   the W0b mandate that the slot must be free).

## 7. Build/test result line

```
build:  cmake --build --preset dev   → 0 errors, -Werror clean
tests:  ./build/human_tests          → 9715 / 9715 PASS, 0 ASan leaks
slot:   hu_mcp_server_t              → FREE (no definition, comments only)
shims:  hu_mcp_host_* (5 functions + typedef) emit -Wdeprecated-declarations only
```
