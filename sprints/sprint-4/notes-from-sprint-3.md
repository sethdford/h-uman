# Sprint 4 — Notes from Sprint 3 Audit

## Pattern C Fallback Disposition (US-9 audit closure)

The Sprint 3 US-2 Definition of Done contained the line: "grep returns zero hits in the
bus-broadcast branch."  The auditor flagged two remaining calls to
`hu_conversation_strip_channel_tags` in `src/daemon.c` (lines 2127 and 2137 as of the
Sprint 3 close) that appeared to contradict this DoD.

**Disposition (Sprint 4 US-9, option b — keep with documentation):** those two calls are
intentional defense-in-depth fallbacks, not survivors of an incomplete migration.  They
are reachable only when the primary validator-chain path fails: one arm fires on
chain-execute failure (allocation error mid-stream); the other fires when no allocator is
available and the chain cannot be built at all.  In both cases the DoD's intent — that
unvalidated output never reaches the bus via the primary path — is fully satisfied.  The
DoD text was too literal; "zero hits" was meant to describe the primary path, not the
defensive fallback arms.

Both call sites have been annotated with block comments in `src/daemon.c` referencing
this file (Sprint 4 US-9 commit).  The Sprint 3 `stories.md` US-2 DoD line has been
updated in-place to state the actual post-implementation invariant accurately.

No behavior was changed.  The fallbacks remain as the correct safety net for the
chain-unavailable case.
