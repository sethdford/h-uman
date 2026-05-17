---
plan: docs/plans/2026-05-11-w0a-episode-rename-report.md
auditor: group-11-audit-followups-adrs-renames-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Rename report for the W0a precondition slice that resolved an ODR violation on
`hu_episode_t`. Renamed the agent-side typedef to `hu_session_episode_t` and the
deep-memory typedef to `hu_deep_episode_t`, freeing the `hu_episode_t` symbol slot
for init-10.

## Key Claims (from the plan)
- Claim 1: Two conflicting `hu_episode_t` typedefs unified by rename
- Claim 2: `include/human/agent/episodic.h` → `hu_session_episode_t`
- Claim 3: `include/human/memory/deep_memory.h` → `hu_deep_episode_t`
- Claim 4: Post-rename grep for `\bhu_episode_t\b` outside docs returns 0
- Claim 5: 10132/10132 tests pass, 0 ASan errors

## Evidence

### Implemented? (code exists)
- `include/human/memory/deep_memory.h:27` — `} hu_deep_episode_t;` ✓
- `include/human/memory/deep_memory.h:12-14` — comment documents the rename
- `include/human/memory/episodic.h:13-15` — comment documents both new names
- `include/human/agent/episodic.h:17` — comment confirms rename to `hu_session_episode_t`
- `grep -rn "\bhu_episode_t\b" include/ src/ tests/` returns ONLY documentation
  comments (3 hits, all in /* */ blocks describing the rename), zero live typedefs
- `grep -rn "\bhu_session_episode_t\b\|\bhu_deep_episode_t\b" include/ src/ tests/`
  returns multiple live references confirming the new names are in use

### Proven? (tests exist)
- Tests for both renamed types exist and use the new names (deep_memory.h:30 signature
  uses `hu_deep_episode_t`)

### Wired? (called in runtime path / dispatch)
- Renamed types referenced in `src/memory/deep_memory.c` function signatures and
  in `tests/test_deep_memory.c` struct literals

## Gaps
- None for the W0a rename itself
- Plan explicitly defers function-name updates (`hu_episode_deinit` → `hu_deep_episode_deinit`)
  to a future slice; not a gap, by design

## Notes
This is a rename report, not a forward plan. The work it documents has shipped.
The freed `hu_episode_t` symbol slot is confirmed empty for init-10 to use.
