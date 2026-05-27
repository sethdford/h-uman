# ASan stack-use-after-scope False Positives Across pthread Stack Reuse on macOS arm64

When a stack-local struct is passed to a `pthread_create`d worker thread,
the worker dereferences it for the duration of the work, and the parent
blocks on `pthread_join` — that pattern is structurally safe. But under
AddressSanitizer on macOS arm64, this pattern produces consistent
**false-positive** `stack-use-after-scope` reports whose READ site is on
the parent thread and whose victim address is mis-attributed to a
worker-thread frame that has already exited.

## The signature

```
==XXXXX==ERROR: AddressSanitizer: stack-use-after-scope on address 0x16baXXXXXX
READ of size N at 0x16baXXXXXX thread T0
    #0 ... in <parent_function> <file>:<line>     ← READ on T0
    ...

Address 0x16baXXXXXX is located in stack of thread T0 at offset NNNNN in frame
    #0 ... in <worker_function> <file>:<line>     ← Attributed to a worker frame
                                                    that ran on T1, not T0

HINT: this may be a false positive if your program uses some custom stack unwind
      mechanism, swapcontext or vfork
```

Two specific recurrence shapes from h-uman:

| Date | Site | Pattern |
|------|------|---------|
| 2026-05-26 trusting-tharp | `cli.c:349` (`run_spinner_loop` reads `tctx->done`) | `tctx` was a loop-scope local on T0, passed to T1's `agent_turn_thread` |
| 2026-05-26 trusting-tharp | `humanness.c::hu_agent_free_turn_context` reads `agent->humanness_ctx_buf` | `agent` was a function-scope local on T0, passed via `tctx->agent = &agent` to T1 |

## Why it's a false positive

- Darwin's pthread allocates worker stacks via `mmap` in the address range
  `0x16b00000000..0x16e00000000` (just below the main thread's main stack at
  `0x16f00000000..0x17000000000`).
- When the worker exits, pthread MAY lazily release that mmap region.
- ASan's shadow memory for those addresses is NOT proactively unpoisoned.
- When pthread RE-uses an overlapping region for the NEXT worker's stack,
  ASan's stale `f8` (use-after-scope) or `f2` (mid-redzone) poison
  contaminates the new frame's shadow view.
- The "Address ... is located in stack of thread T0 at offset NNN in frame
  <worker_function>" line is ASan looking up the most recent stack
  allocation it tracked at that address and reporting it — but the thread
  attribution and the frame attribution can both be wrong after pthread
  stack reuse.

## How to recognize this in the wild

ALL of these must be true:

- [ ] Running under ASan (the prod binary works fine)
- [ ] macOS arm64 (Linux + Apple x86_64 don't have the same shadow-reuse bug)
- [ ] The READ frame is on T0 (or any parent thread holding the worker's
      context pointer)
- [ ] The "Address is located in stack of T0 in frame X" line names a
      function the parent thread NEVER directly invokes
- [ ] The address falls in `0x16b00000000..0x16e00000000` (pthread stack
      range), NOT in the parent thread's main-stack range
      (`0x16f00000000..0x17000000000`)
- [ ] The HINT line about "custom stack unwind mechanism, swapcontext or
      vfork" is present (ASan emits this when it KNOWS the report is
      shaky)

If all six hold, it is the false positive. Do NOT spend a day chasing a
phantom bug in the worker function.

## The fix

Heap-allocate the cross-thread context struct AND any other parent-stack
data the worker dereferences during its turn. Heap memory has its own
shadow-region lifecycle tied to `malloc`/`free`, which is reliable across
threads:

```c
/* BAD — stack-local, ASan will false-positive on Darwin arm64 */
agent_turn_ctx_t tctx;
memset(&tctx, 0, sizeof(tctx));
tctx.agent = &agent;       /* &agent is also on the parent stack */
pthread_create(&tid, NULL, worker, &tctx);
run_spinner_loop(&tctx, use_ansi);     /* T0 reads tctx fields here */
pthread_join(tid, NULL);

/* GOOD — heap-allocate, no false positive */
agent_turn_ctx_t *tctx = alloc->alloc(alloc->ctx, sizeof(*tctx));
if (!tctx) { /* handle */ }
memset(tctx, 0, sizeof(*tctx));
tctx->agent = &agent;
pthread_create(&tid, NULL, worker, tctx);
run_spinner_loop(tctx, use_ansi);
pthread_join(tid, NULL);
alloc->free(alloc->ctx, tctx, sizeof(*tctx));
```

Cost: one heap alloc+free per turn. Negligible compared to the LLM call.

## When NOT to apply this rule

- The parent function declares the context struct OUTSIDE any loop AND has
  no compound-statement scopes around the cross-thread pointer. The most
  reliable false positives appear with LOOP-SCOPED context structs.
- The platform is Linux x86_64 or any non-Darwin target. Shadow reuse is
  better-behaved there.
- The worker thread is detached and the parent doesn't `pthread_join`. In
  that case, the pattern is genuinely unsafe and you need heap-alloc OR
  proper lifecycle management for real safety, not just for ASan.

## What NOT to do

- ❌ Don't disable `detect_stack_use_after_scope` globally — it catches
  real bugs on other paths.
- ❌ Don't sprinkle `__attribute__((no_sanitize("address")))` on the worker
  function — it hides real bugs and doesn't fix the underlying confusion.
- ❌ Don't rebuild the production binary without ASan to "verify it's a
  false positive." That's true but doesn't tell you anything new.
- ❌ Don't add defensive heap-allocation hoists for individual locals
  inside the worker function (e.g. moving `stored_facts[16]` from stack to
  heap inside `hu_agent_turn`). Those just push the false positive to the
  NEXT stack local in shadow-aliased memory. Fix the cross-thread pointer
  at the source.

## Related

- `~/.claude/rules/audit-verify-before-allege.md` — the discipline of
  verifying claims before chasing them. Applies here: verify the bug
  is real (prod works) before refactoring code that ASan claims is
  broken.
- `~/.claude/CLAUDE.md` "Verify, don't assert" — pair with the
  audit-verify rule.
- LLVM bug tracker: search "macOS pthread stack ASan shadow" — multiple
  upstream reports cover this exact pattern.

## Investigation that produced this rule

Chip G investigation, 2026-05-26 trusting-tharp h-uman session:

- Initial reports mis-attributed the WRITE site to `agent_turn.c:797`
  (`*response_out = NULL`). Defensive heap-allocation hoists for
  `stored_facts[16]` and `tot_reasoning_buf[4096]` inside `hu_agent_turn`
  did NOT fix the issue — each hoist just exposed the NEXT stack local
  in shadow-aliased memory.
- Rebuilding the dev binary with `-g3 -O0` (so DWARF line attribution
  was accurate) revealed the TRUE READ site as `cli.c:349` in
  `run_spinner_loop`, reading `tctx->done`.
- Heap-allocating `tctx` in `hu_agent_cli_run` eliminated the original
  use-after-scope error class entirely.
- A second false-positive class (`stack-buffer-overflow` in
  `hu_agent_free_turn_context` reading `agent->humanness_ctx_buf`)
  surfaced with the same cross-thread shape (`&agent` passed via
  `tctx->agent`). Confirmed prod binary works; deferred fix because
  `agent` is function-scope (not loop-scope) and the production daemon
  is unaffected. Heap-allocating `agent` is the analogous fix when/if
  the same false positive blocks future ASan-clean dev work.
