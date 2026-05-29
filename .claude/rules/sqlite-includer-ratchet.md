# SQLite Includer Ratchet — Never Add a New `#include <sqlite3.h>`

The count of `src/` files (excluding `src/memory/engines/` and
`src/memory/repos/`) that include `<sqlite3.h>` is frozen at a baseline and
may only **decrease**.

## The hazard (T1)

SQLite is woven through the codebase as an *ambient* dependency: domain code
grabs the raw `sqlite3*` via `hu_sqlite_memory_get_db()` and runs SQL inline,
bypassing the `hu_memory_t` vtable. This makes the vtable a fake abstraction
and blocks the "runs anywhere / privacy-by-architecture" moat — a non-SQL
backend (Redis, pure on-device) cannot run code that hardcodes SQLite.

Baseline at Phase 0 (2026-05-29): **110** includers outside the engine layer.

## The rule

New domain code MUST reach persistence through a memory **repository**
(`include/human/memory/<aggregate>_repo.h` + `src/memory/repos/<aggregate>_repo_sqlite.c`),
where SQLite is legal. It must NOT `#include <sqlite3.h>` or call
`hu_sqlite_memory_get_db()` / `hu_memory_facade_sqlite_db()` directly.

As each Phase-3 aggregate migrates and a file drops its sqlite include, lower
the `BASELINE` constant to lock the gain — the ratchet only tightens.

## Enforcement

`scripts/check-sqlite-includer-ratchet.sh`, wired into `.githooks/pre-commit`
(fires when a `src/` C/H file is staged). Engine + repo layers are exempt.

## Related

- `docs/plans/2026-05-29-ddd-bounded-contexts/phase-3-memory-query-interface.md` — the repository pattern that closes T1
- `~/.claude/rules/quality-gates.md` — "No silent failures"; this is its structural form for the SQLite boundary
