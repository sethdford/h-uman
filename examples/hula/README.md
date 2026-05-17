# HuLa examples gallery

Five self-contained, structurally-valid HuLa programs that demonstrate
the opcodes most commonly used in agent orchestration. Each example is
a directory containing a `program.json` (the HuLa IR) and a `README.md`
(what it demonstrates and how to run it).

| Example                                                         | Primary opcode(s)      | What it shows                                                  |
|-----------------------------------------------------------------|------------------------|----------------------------------------------------------------|
| [01-simple-call](./01-simple-call/)                             | `CALL`                 | Invoke a single tool with JSON args.                           |
| [02-branching](./02-branching/)                                 | `SEQ`, `BRANCH`        | Conditional flow based on the preceding node's outcome.        |
| [03-error-recovery](./03-error-recovery/)                       | `TRY` (body + catch)   | Recover from a failed call with a deterministic fallback.      |
| [04-emergence-detection](./04-emergence-detection/)             | `SEQ`, `VERIFY`        | Post-condition assertion on a preceding node's output.         |
| [05-multi-step-pipeline](./05-multi-step-pipeline/)             | `SEQ`, `PAR`, `CALL`   | Sequential prep, parallel fan-out, sequential aggregation.     |

## How to run

These programs are validated structurally by
`tests/test_hula_examples.c`, which parses each `program.json` with
`hu_hula_parse_json` and asserts that `hu_hula_validate` returns
`val.valid == true` against an empty tool registry (structural and
opcode rules only — tool-name resolution is skipped).

Executing the programs end-to-end against a live tool registry is out
of scope for US-10.4. End-to-end examples that bind to real tools will
ship with the Python and TypeScript bindings under
`bindings/<lang>/examples/` (see US-10.5 for the MCP bridge example).

## Adding a new example

1. Create a new directory under `examples/hula/` with a numeric
   prefix (e.g. `06-...`) and add `program.json` plus `README.md`.
2. Append a row to the `k_cases[]` table in
   `tests/test_hula_examples.c` so the new program is parsed and
   validated as part of the suite. The comment at the top of that
   table is a reminder.
3. Update the table in this README.
