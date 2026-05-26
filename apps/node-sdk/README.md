# @human/hula-sdk — Node.js SDK for HuLa

Node.js wrapper for [HuLa (Human Language)](../../include/human/hula_sdk.h)
programs. Sibling of the [Python SDK](../python-sdk/) — same API surface,
different language. Both are part of the M5 "HuLa as Platform" mission.

## Status: Phase 1 (subprocess boundary)

This release wraps the `human hula` CLI via `node:child_process`. Apps
using the SDK do NOT need native bindings or `node-gyp` — they just
need the `human` binary on PATH (or pointed at via `HUMAN_BIN`).

Phase 2 (planned) will add a native N-API binding once the project
ships a shared libhuman dylib. Both phases expose the same JS API so
existing consumers won't need to change anything.

## Install

This is a stdlib-only ESM package; no `npm install` needed for Phase 1.
Either symlink it into your project's `node_modules/` or import it
directly from the checkout:

```js
import { HuLa } from "/path/to/h-uman/apps/node-sdk/src/index.js";
```

You also need a `human` binary. Build it from the repo root:

```bash
cmake --build build --target human
export HUMAN_BIN="$(pwd)/build/human"
```

Requires Node.js >= 18 (uses ESM, `node:test`, `node:fs/promises`).

## Quick start

```js
import { HuLa } from "@human/hula-sdk";

const program = {
  name: "hello",
  version: 1,
  root: {
    id: "n1",
    op: "emit",
    emit_key: "greeting",
    emit_value: "hello from node",
  },
};

const hula = new HuLa(); // uses HUMAN_BIN env var, or `human` on PATH

const v = await hula.validate(program);
if (!v.ok) {
  console.error(`validate failed: ${v.stderr}`);
} else {
  const r = await hula.run(program);
  console.log(r.stdout);
}
```

See [`examples/hello_hula.js`](examples/hello_hula.js) for a runnable copy.

## API

### `new HuLa(humanBin?: string)`

Constructor argument precedence:

1. Explicit `humanBin` argument
2. `HUMAN_BIN` environment variable
3. `human` on `PATH`

### Methods (all return `Promise<HulaResult>`)

| Method | What |
|---|---|
| `validate(program)` | structure, refs, depth, cycles |
| `run(program)` | execute with the CLI's demo tools |
| `schema()` | canonical hula-program JSON Schema |
| `expand(template, vars)` | substitute `{{keys}}` in `template` |
| `compile(source, { lite? })` | lite-syntax or JSON → canonical JSON |
| `replay(trace, { configPath? })` | re-run from a captured trace |

### `HulaResult`

```ts
type HulaResult = {
  ok: boolean;        // true if subprocess exited 0
  returncode: number; // raw exit status
  stdout: string;     // captured stdout
  stderr: string;     // captured stderr (failure reason)
};
```

Promises never reject on non-zero exit — check `.ok` and read `.stderr`.
They reject only on spawn-level failures (binary not found, etc.).

### Versioning

```js
import { HULA_SDK_VERSION_STRING } from "@human/hula-sdk";
console.log(HULA_SDK_VERSION_STRING); // e.g. "0.1.0"
```

Mirrored from `include/human/hula_sdk.h` `HU_HULA_SDK_VERSION_STRING`.
Bumped in lock-step with the C macros and the Python SDK.

## Testing

```bash
cmake --build build --target human
HUMAN_BIN=./build/human node --test apps/node-sdk/test/
```

Tests use `node:test` (stdlib). They skip cleanly if no `human` binary
is found.

## Roadmap

| Phase | What | Status |
|---|---|---|
| 1 | subprocess wrapper, full CLI parity | ✅ |
| 2 | N-API native binding (requires shared libhuman dylib) | 📋 |
| 3 | LLM-driven program synthesis wrapper | 📋 |
| 4 | npm publish + version-pinned binary auto-download | 📋 |

## See also

- [`include/human/hula_sdk.h`](../../include/human/hula_sdk.h) — the
  C SDK header this wraps
- [`schemas/hula-program.schema.json`](../../schemas/hula-program.schema.json)
  — JSON Schema for HuLa programs (verify your object against it before
  passing to `validate`)
- [`apps/python-sdk/`](../python-sdk/) — sister SDK; same API in Python
- `human hula` — the underlying CLI (`schema`, `expand`, `compile`,
  `replay`, `run`, `validate`)
