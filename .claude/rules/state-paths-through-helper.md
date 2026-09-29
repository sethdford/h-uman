# State Paths Go Through `hu_paths_*` — Never Format `$HOME/.human` by Hand

Every path under the state directory or to the iMessage database is resolved
by `include/human/core/paths.h`. Formatting `"%s/.human/…"` (or joining
`home` with a `.human` macro) inline is refused by pre-commit.

## The hazard (measured 2026-09-06)

Two overrides were *documented* and *ignored*: `HU_CHATDB` (`config.h`) was
honored by **4 of 17** files that open chat.db, `HU_STATE_DIR` (`doctor.h`) by
**1 of 62** files that touch `~/.human`. 188 raw `getenv("HOME")` sites each
re-derived the path, in five syntactic shapes — inline literal, multi-line
literal, `"%s/%s", home, HU_X_DIR`, `"%s%s", home, X_SUFFIX`, and adjacent-
literal concatenation. A test that pointed `HU_STATE_DIR` at a scratch dir
still read and wrote the real `~/.human` through most of the daemon.

## Why the obvious fix is wrong

❌ **Grep for the literal and rewrite it.** A source grep found 155 sites and
missed 47: 20 multi-line formats and 27 assembled from macros. `strings
build/human | grep -c '%s/.human'` is the oracle — the linker pools every
compile-time-assembled literal into a string the source never contains.

❌ **Drop the `"."` / `"/tmp"` fallbacks as cruft.** They were load-bearing:
removing them turned a pre-existing test leak (`unsetenv("HOME")` without
restore) into three `daemon_lifecycle` failures. Fallbacks are now explicit
and tested, not scattered.

## The right shape

| Need | Call |
|---|---|
| `$HU_STATE_DIR` or `$HOME/.human`, plus a relative path | `hu_paths_state(buf, cap, "rel/%s", arg)` |
| the bare state dir | `hu_paths_state_dir(buf, cap)` — never pass a NULL/`""` format (GCC 15 rejects `""` under `-Werror`) |
| `$HU_CHATDB` or `~/Library/Messages/chat.db` | `hu_paths_chatdb(buf, cap)` |
| the same, with the pre-migration fallback home | `hu_paths_state_or(buf, cap, "/tmp", …)`, `hu_paths_state_dir_or`, `hu_paths_chatdb_or` |

All return snprintf-shaped lengths; on failure `-1` with `buf[0] == '\0'`.
`tests/test_paths.c` pins the override precedence, the empty-override
fall-through, truncation, and the fallback contracts.

## When this applies / does NOT

- **APPLIES**: any `src/` or `include/` code that names a file under the
  state dir or opens chat.db.
- **DOES NOT apply**: APIs that take `home` as a *parameter* and are tested
  with fakes (`hu_response_guard_dpo_path_for_day`); the cwd-relative
  workspace config in `config_merge.c` (`"%s/%s", cwd, HU_CONFIG_DIR` — not
  the home dir); the public `HU_INBOX_DEFAULT_DIR` constant.

## Enforcement

`scripts/check-state-path-literals.sh`, wired into `.githooks/pre-commit`
(fires when a `src/` or `include/` C/H file is staged). Ceiling 0, freeze-only.

## Follow-up (closed 2026-09-06)

The 146 `getenv("HOME")` guards that sat in front of a helper call were
reviewed one function at a time; 101 were redundant and are gone. A guard is
redundant only when the helper's failure is checked and produces the same
outcome the guard did. The 45 that remain are either (a) a real use of `home`
for a non-state path (launchd/systemd, gcloud ADC, AddressBook, repo
candidates) or (b) marked with a one-line `Kept:` comment naming the outcome
that would change — usually a stderr diagnostic the helper's silent `-1`
would lose, or a different error code. Do not re-sweep them.

One trap from that pass: unwrapping two sibling `if (home) { char sp[]; … }`
blocks into a shared scope shadows the second declaration, and `-Wshadow` is
`-Werror` in the ML build. Keep a bare block when the names repeat.

## Related

- `.claude/rules/ratchet-decay.md` — why this gate has no decay row
- `.claude/rules/reports-success-does-nothing.md` — "measure the artifact":
  the binary, not the source grep
