# 05 — Multi-step pipeline (SEQ + PAR)

This example demonstrates how `SEQ` and `PAR` compose to express a
classic multi-step orchestration: a serial preparation step, a
parallel fan-out that gathers data from two sources concurrently, and
a serial aggregation step that combines the parallel results.

## What it demonstrates

- The `op: "seq"` opcode for in-order execution that short-circuits on
  failure.
- The `op: "par"` opcode for concurrent execution with join-all
  semantics.
- The `$node_id` argument substitution form: the final `aggregate`
  call's `text` arg references `$fetch_a` and `$fetch_b`, instructing
  the executor to splice in those nodes' outputs before invoking the
  tool.
- Composition: nesting a `PAR` inside a `SEQ` is the canonical pattern
  for a fan-out-then-join pipeline.

## Program structure

The root is a `SEQ` named `pipeline` with three children:

1. `prepare` — a `CALL` to `echo` that stands in for any setup step
   that must complete before the fan-out begins.
2. `gather` — a `PAR` node with two `CALL` children, `fetch_a` and
   `fetch_b`. They both invoke `web_fetch` and run concurrently; the
   `SEQ` only advances after both complete.
3. `aggregate` — a `CALL` to `summarize` whose `text` argument
   references the parallel results via `$fetch_a` and `$fetch_b`.

## How to run

`tests/test_hula_examples.c` parses this program and asserts the
parsed tree contains both a `SEQ` node and a `PAR` node anywhere
within it. Executing the example end-to-end requires a tool registry
that supplies `echo`, `web_fetch`, and `summarize`; see the bindings
examples.
