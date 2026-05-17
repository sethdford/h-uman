# 04 — Emergence detection (VERIFY)

This example demonstrates the `VERIFY` opcode: a post-condition check
on a preceding node's output. VERIFY is HuLa's verification-driven
execution primitive (arXiv:2603.11445 VMAO-inspired) and is the hook
that emergence analysis uses to surface hot, high-confidence paths in
the trace log.

## What it demonstrates

- The `op: "verify"` opcode and its required `verify_node_id` field,
  which names the node whose output is to be checked.
- The shared `pred` / `match` fields, which describe the predicate the
  output must satisfy. Here we use `pred: "contains"` with the literal
  match string `"summary"`.
- Composition: VERIFY sits at the end of a `SEQ` so the node it
  references (`fetch_summary`) has already produced an output by the
  time the predicate is evaluated.

## Program structure

The root is a `SEQ` named `verified_pipeline` with two children:

1. `fetch_summary` — a `CALL` to the `summarize` tool that produces a
   one-sentence summary of recent activity.
2. `check_summary_nonempty` — a `VERIFY` node that names
   `fetch_summary` via `verify_node_id` and asserts the output
   contains the literal string `"summary"`. If the predicate fails the
   executor halts the program and the failure is recorded in the
   trace log, where emergence analysis can pick it up.

## How to run

`tests/test_hula_examples.c` parses this program with
`hu_hula_parse_json` and asserts the parsed tree contains at least one
`VERIFY` node. Running the program end-to-end requires a tool registry
that supplies `summarize`; see the bindings examples.
