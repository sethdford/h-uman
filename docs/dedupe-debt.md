# Duplicated utility functions — migration tracker

Snapshot from FIX 7 of the post-audit punch list. Several private static
helpers were copy-pasted across the codebase, hitting the project's
"Rule of Three" engineering principle the hardest in three places:

| Helper | Copies | Status |
|--------|-------:|--------|
| `escape_sql_string` (SQL single-quote escape) | 18 | **All 18 migrated to forwarder** (FIX 7 + FIX 13); see below |
| `to_lower` (ASCII lowercasing) | 7 | Trivial 4-liner; net win is small. Deferred. |
| `dev_urandom_bytes` (security-internal CSPRNG) | 2 | Security subsystem internal; consolidating crosses module boundaries. Deferred. |
| `parse_iso_timestamp` (RFC-3339 -> epoch) | 2 | Different return types (`time_t` vs `int64_t`); consolidate when callers harmonize. Deferred. |

## The escape_sql_string migration

The 18 private copies fell into two near-identical signatures:

* **Dominant (12 callers):** `static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len)` — silently truncates, writes byte count to out param.
* **Minority (5 callers):** `static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap)` — returns 0 on overflow OR empty input.
* **Outliers (1 caller):** `escape_sql_string(s, len, buf, cap, *out_len)` with custom buffer-truncation semantics.

The canonical helper is `hu_sql_quote_escape_into` in `include/human/core/string.h` /
`src/core/string.c`. Behavior contract is pinned by 7 unit tests in
`tests/test_string.c::test_sql_quote_escape_*`:

1. No quotes → bytes copied verbatim.
2. Quotes → each `'` doubled.
3. Empty/NULL src → `*out_len = 0`, dst is `""`.
4. Truncation → silent, `*out_len` matches what was written.
5. Invalid args → `HU_ERR_INVALID_ARGUMENT`.

### Migration pattern

The minimum-risk dedupe replaces each module's static body with a one-line
forwarder so existing call sites don't change:

```c
/* core/string.h is already included. */
static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap,
                              size_t *out_len) {
    (void)hu_sql_quote_escape_into(s, len, buf, cap, out_len);
}
```

For the 5 minority callers (`return size_t`), the forwarder shape is:

```c
static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap) {
    size_t n = 0;
    (void)hu_sql_quote_escape_into(s, len, out, out_cap, &n);
    return n;
}
```

Note: the minority signature returns 0 on overflow as a sentinel; the
canonical helper truncates silently and reports the partial count. If a
caller depends on the `0 == overflow` sentinel for SQL safety, audit it
before applying the forwarder.

### Migration progress

Migrated to the forwarder pattern in FIX 7:

- `src/agent/proactive_ext.c`
- `src/memory/cognitive.c`

Migrated to the forwarder pattern in FIX 13:

- `src/agent/collab_planning.c`
- `src/agent/conv_goals.c`
- `src/agent/planning.c`
- `src/agent/rel_dynamics.c`
- `src/context/authentic.c`
- `src/context/context_ext.c`
- `src/context/intelligence.c`
- `src/context/rel_dynamics.c`
- `src/context/self_awareness.c`
- `src/feeds/processor.c`
- `src/intelligence/reflection.c`
- `src/memory/compression.c`
- `src/memory/deep_memory.c`
- `src/memory/knowledge.c`
- `src/persona/training.c`
- `src/visual/content.c`

Pending: none. All 18 callers now route through `hu_sql_quote_escape_into`.
A future sweep commit can delete the static forwarders and inline the
canonical name at each call site.

### How to finish the migration

For each remaining file:

1. Replace the body of the static helper with a single call to
   `hu_sql_quote_escape_into` (template above).
2. Run the relevant test suite (e.g. `--suite=Memory`, `--suite=Feeds`).
3. Run the SQL-escape unit tests: `--suite=string`.
4. When all 18 are migrated, delete the static forwarders in one sweep
   commit and replace each call site with the canonical name.

## When to revisit the deferred helpers

* `to_lower` — revisit when adding a Unicode-aware variant; collapsing all
  ASCII copies at that point gives a real reason to centralize.
* `dev_urandom_bytes` — revisit when adding a third caller, or when adding
  a system random abstraction (`hu_random_*`) that the security subsystem
  can depend on without inverting the dependency graph.
* `parse_iso_timestamp` — revisit when callers harmonize on `int64_t`
  epoch-ms (the `time_t` caller is in
  `src/memory/consolidation.c` and is already comparing against
  `time(NULL)`, so a small rewrite would let it use `int64_t` too).
