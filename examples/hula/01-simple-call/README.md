# 01 — Simple CALL

This example is the smallest possible HuLa program: a single `CALL` node
that invokes a registered tool with a JSON argument object.

## What it demonstrates

- The `op: "call"` opcode, which is HuLa's primary way of invoking a
  registered tool via the `hu_tool_t` vtable.
- The `tool` field, which selects the tool by name from the executor's
  registry.
- The `args` field, a free-form JSON object that the executor passes
  through to the tool's `execute()` callback as a `hu_json_value_t *`.

Every HuLa program ultimately bottoms out in `CALL` nodes — the other
opcodes (`SEQ`, `PAR`, `BRANCH`, `TRY`, `VERIFY`, `LOOP`, `DELEGATE`,
`EMIT`) only sequence, gate, or react to results of `CALL`s. Reading
this example first makes the others legible.

## Program structure

The single node has `id: "greet"`, `tool: "echo"`, and a single string
argument `text` whose value is the canonical greeting `"hello from
HuLa"`. This shape is sufficient for the example to parse cleanly and
to satisfy `hu_hula_validate` against an empty tool registry — the
validator only enforces "tool name not empty" when no tool list is
supplied. Tool reference checking is opt-in by supplying a tool name
list to the validator.

## How to run

The example is validated structurally by `tests/test_hula_examples.c`,
which parses `program.json` with `hu_hula_parse_json` and confirms it
passes `hu_hula_validate`. Executing the program with a live tool
registry is out of scope for US-10.4; see `bindings/python/examples/`
(landing with US-10.5) for an end-to-end example.
