# 02 — Branching

This example demonstrates the `BRANCH` opcode: conditional execution
that selects between a `then` subtree and an `else` subtree based on a
predicate evaluated against the most recent result.

## What it demonstrates

- The `op: "branch"` opcode and its `pred` field.
- The `then` / `else` shorthand JSON keys, which the parser converts to
  `children[0]` and `children[1]` during `hu_hula_parse_json`.
- The `pred: "success"` predicate, which evaluates true if the previous
  sibling node completed successfully and false otherwise.
- Composition: the branch sits inside a `SEQ` so it has a preceding
  node (`probe`) whose result drives the predicate.

## Program structure

The root is a `SEQ` named `pipeline` with two children:

1. `probe` — a `CALL` to the `search` tool that returns the
   most-recent health-check result.
2. `decide` — a `BRANCH` node whose `pred` is `success`. The `then`
   branch invokes `echo` with a positive message; the `else` branch
   invokes `echo` with a halt message.

Because HuLa predicates inspect the result of the preceding node in
execution order, the branch reacts to whether `probe` succeeded. No
explicit conditional comparison is needed — the executor handles the
plumbing.

## How to run

This program is parsed and structurally validated by
`tests/test_hula_examples.c`. The test asserts the root op is `BRANCH`
when the entire tree is walked (a `BRANCH` node is reachable from the
root). Executing this example requires a tool registry that supplies
both `search` and `echo`; see future bindings examples for that.
