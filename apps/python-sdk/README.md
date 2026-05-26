# human — Python SDK for HuLa

Python wrapper for [HuLa (Human Language)](../../include/human/hula_sdk.h)
programs. First brick of the M5 "HuLa as Platform" mission.

## Status: Phase 1 (subprocess boundary)

This release wraps the `human hula` CLI via subprocess. Apps using the
SDK do NOT need to link the C library or build a shared dylib — they
just need the `human` binary on PATH.

Phase 2 (planned) will add an in-process `ctypes` binding once the
project ships a shared `libhuman.dylib`. Both phases expose the same
Python API so existing consumers won't need to change anything.

## Install

This is a stdlib-only single-file package; no `pip install` needed for
Phase 1. Add the SDK directory to `PYTHONPATH`:

```bash
export PYTHONPATH="$PYTHONPATH:/path/to/h-uman/apps/python-sdk"
```

You also need a `human` binary. Build it from the repo root:

```bash
cmake --build build --target human
export HUMAN_BIN="$(pwd)/build/human"
```

## Quick start

```python
from human import HuLa

prog = {
    "version": 1,
    "nodes": [
        {"id": "n1", "op": "emit",
         "emit_key": "greeting",
         "emit_value": "hello from python"},
    ],
}

hula = HuLa()  # uses HUMAN_BIN env var, or `human` on PATH

result = hula.validate(prog)
if not result.ok:
    print(f"validate failed: {result.stderr}")
else:
    run_result = hula.run(prog)
    print(run_result.stdout)
```

See [`examples/hello_hula.py`](examples/hello_hula.py) for a runnable
copy.

## API

### `class HuLa(human_bin: Optional[str] = None)`

The SDK entry point. Spawns a fresh `human` subprocess per call;
thread-safe (no shared state).

Constructor argument precedence:

1. Explicit `human_bin` argument
2. `HUMAN_BIN` environment variable
3. `human` on `PATH`

#### Methods

- `validate(program: dict) -> HulaResult` — structure, refs, depth, cycles
- `run(program: dict) -> HulaResult` — execute with the CLI's demo tools
- `schema() -> HulaResult` — return the canonical hula-program JSON Schema
- `expand(template: str, vars: dict) -> HulaResult` — substitute `{{keys}}`
- `compile(source: str, lite: bool = False) -> HulaResult` — lite-syntax
  or canonical-JSON → canonical JSON. NOTE: this is a syntactic transform,
  not LLM-driven synthesis. Phase 3 will add the synthesis wrapper.
- `replay(trace: dict, config_path: str | None = None) -> HulaResult` —
  re-run an embedded program from a captured trace. Pair with the
  `HU_HULA_TRACE_DIR` env var on a prior `run` call to capture traces.

### `class HulaResult`

Dataclass returned by every `HuLa` call:

| Field | Type | Description |
|---|---|---|
| `ok` | `bool` | True if subprocess exited 0 |
| `returncode` | `int` | Raw exit status |
| `stdout` | `str` | Captured stdout |
| `stderr` | `str` | Captured stderr (failure reason) |

### Versioning

```python
from human import HULA_SDK_VERSION_STRING
print(HULA_SDK_VERSION_STRING)  # e.g. "0.1.0"
```

Mirrored from `include/human/hula_sdk.h` `HU_HULA_SDK_VERSION_STRING`.
Bumped in lock-step.

## Testing

```bash
cmake --build build --target human
PYTHONPATH=apps/python-sdk \
  HUMAN_BIN=./build/human \
  python3 -m unittest apps/python-sdk/tests/test_smoke.py
```

Tests use stdlib `unittest` (no pytest dep). They skip cleanly if no
`human` binary is found.

## Roadmap

| Phase | What | Status |
|---|---|---|
| 1   | subprocess wrapper, `validate` + `run` | ✅ |
| 1.5 | full CLI parity: `schema`/`expand`/`compile`/`replay` | ✅ |
| 2   | `ctypes` in-process binding (requires shared libhuman.dylib) | 📋 |
| 3   | LLM-driven program synthesis wrapper | 📋 |
| 4   | Hosted docs + PyPI package + version-pinned binary auto-download | 📋 |

## See also

- [`include/human/hula_sdk.h`](../../include/human/hula_sdk.h) — the
  C SDK header this wraps
- [`schemas/hula-program.schema.json`](../../schemas/hula-program.schema.json)
  — JSON Schema for HuLa programs (verify your dict against it before
  passing to `validate`)
- `human hula --help` — the underlying CLI's full surface (`compile`,
  `replay`, `expand`, etc. — Phase 3 will expose these)
