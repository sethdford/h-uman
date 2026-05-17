# Sprint 39 Retro

## Keep

- Closing the quality-gate gap (5×) in the same sprint as guard hardening — defense in depth is only real when layers align.
- `--self-test` on the audit script so CI validates signatures without `chat.db`.

## Improve

- Full-suite ASan caught a bad `free()` on stack `raw` in a new test — follow existing pass-through tests (no free on `HU_GUARD_OK`).

## Next

- Wire `hu_guard_reject_stats_*` to gateway admin RPC for dashboard visibility.
- Measure REJECT rate in production logs before further G7/G8 threshold tweaks.
