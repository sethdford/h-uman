# Code That Reports Success While Doing Nothing

The most expensive bug shape this codebase produces. Every instance compiles,
exits 0, logs success, and passes the full suite. Five landed in one week
(2026-07-27 → 08-02), each found only by measuring the **artifact** rather than
reading the **report**.

## The five

| # | What reported success | What actually happened | Caught by |
|---|---|---|---|
| 1 | `mlx-lm-lora` ORPO exited 0, saved an adapter, loss curve moved | Forward pass ran OUTSIDE `nn.value_and_grad`, so every gradient was structurally zero. **Two adapters trained to completion as perfect no-ops** (`lora_b` 0/80) | inspecting the weights |
| 2 | `proactive check-in sent to <name>` ×12 | Send's `hu_error_t` discarded. **Zero rows in chat.db.** Also recorded send-recency (suppressing later reactive replies), trained the bandit on undelivered text, and charged the governor | querying `chat.db` |
| 3 | `INSERT OR IGNORE INTO opinions` | No UNIQUE constraint to violate, so OR IGNORE never fired. **9,533,051 rows over 2,962 distinct pairs**, 4.3 GB | `COUNT(*)` vs `COUNT(DISTINCT …)` |
| 4 | `Iter N: Val loss 0.108` at 9 checkpoints | Bit-identical every time because the adapter was a no-op — it measured the frozen base plus batch noise | noticing values identical to 3dp |
| 5 | `embedder_local.c` returns a 384-dim vector | It is a hash projection of word hashes. No synonymy, no paraphrase. Semantically inert | reading it |

## The tell

**A metric that does not move while the thing it measures demonstrably does is not
a weak effect — it is the treatment never being applied.** Identical values to full
precision, a "sent" log with no delivery, `COUNT(*)` far above `COUNT(DISTINCT)`,
`lora_b` all zero: these are the same fact wearing different clothes.

## What to do

1. **Measure the artifact, never the report.** Weights, `chat.db` rows, table
   counts, file bytes. `grep -c` on a log line proves the line was printed, not
   that the work happened.
2. **Size the observation window to the phenomenon.** "Delta 0 over 100 s" against
   a 5-minute cadence proves nothing. That mistake nearly closed the opinions bug
   twice; a 7-minute window is what caught it still running at +5,934.
3. **Prove a guard discriminates.** Run it against a reconstruction of the bug and
   confirm it fails *for the right reason*. `scripts/check-silent-success.sh` was
   written with an over-broad `PRIMARY KEY \(` alternative that matched something
   in every build — so it passed unconditionally and caught nothing. The guard had
   the very bug it was written to detect, and only testing it against a fake
   offender revealed that.
4. **A scoped fix is not automatically the right fix.** Scoping `cycle.c`'s SELECT
   to `acted_at >= now_ts` was correct, deployed, and verified in the binary — and
   inserts continued at 2,967 every 5 minutes from another path. The
   writer-agnostic UNIQUE index is what actually worked. When the artifact still
   moves after your fix, the fix is incomplete regardless of how right it looks.

## Enforcement

`scripts/check-silent-success.sh`, wired into `.githooks/pre-commit`, catches the
two mechanically detectable shapes:

- `INSERT OR IGNORE|REPLACE INTO <t>` where no UNIQUE constraint on `<t>` exists
- a `->send(` / `->store(` / `->write(` whose return is silently discarded

Both are baselined (tables by name, discards by file) so the guard is green today
and fails only on growth — the ratchet convention used by `clone-ratchet` and
`file-size-ceiling`. Known limitation, recorded in the script: a constraint split
across adjacent C string literals is invisible to a line-oriented grep, so a
baselined table is not proof it lacks a constraint.

Shapes 1, 4 and 5 are not statically detectable. For those the trainer asserts
`lora_b` is non-zero before accepting a run, and `register_v6_adapter.py` repeats
the check at registration, because a registry row is what a promotion gate reads.

## Three wrong allegations, same cure

Verification cuts both ways. In the same week I claimed a street-address leak
(confabulated — nothing leaked), claimed proposal text was sent to a contact
(never delivered), and declared the faucet fixed off too short a window. One
`chat.db` query and one longer window settled all three. **Check before alleging,
not only before claiming done** — see `~/.claude/rules/verify-before-you-claim.md`.

## Related

- `.claude/rules/no-number-without-a-measurement.md` — the parent rule; this is
  its code-side twin
- `.claude/rules/ground-truth-over-proxy-signals.md` — a green build is a proxy
- `~/.claude/rules/verify-before-you-claim.md` — both directions of the claim
