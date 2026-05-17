# 03 — Error recovery

This example demonstrates the `TRY` opcode: structured error handling
with a `body` child that runs first and a `catch` child that runs only
if the body fails.

## What it demonstrates

- The `op: "try"` opcode and its two named children: `body` and
  `catch`.
- The shorthand JSON keys `body` and `catch`, which
  `hu_hula_parse_json` lowers to `children[0]` and `children[1]` at
  parse time.
- The recovery pattern: a primary call that might fail (a remote
  fetch) plus a deterministic fallback that always succeeds (an echo
  of a cached value).

## Program structure

The root is a `TRY` node with id `guarded`:

- `body`: a `CALL` to `web_fetch` against
  `https://example.com/api/status`. In production this would be a real
  remote call that can fail with network errors, non-2xx responses, or
  policy denials.
- `catch`: a `CALL` to `echo` that returns a cached fallback string.
  When the body fails, the executor invokes the catch and the program
  as a whole succeeds with the fallback output.

## How to run

`tests/test_hula_examples.c` parses this program with
`hu_hula_parse_json` and confirms it passes `hu_hula_validate`. The
test additionally asserts that the root op is `TRY` and that the
parsed tree contains both a body and a catch (`children_count >= 2`).
Executing the example requires a tool registry that supplies
`web_fetch` and `echo`; see the bindings examples for an end-to-end
run.
