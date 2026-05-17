# Critic findings — US-9.6 chat.db locked diagnostic

## HIGH (2)

- `src/doctor.c:658-668` — **Live-sqlite error path bypasses the shared predicate.**
  When `sqlite3_prepare_v2` or `sqlite3_step` returns an error (i.e., the probe
  opens but the query fails — the common FDA path on macOS 13+), the code
  constructs a free-form `hu_sprintf` string inline rather than calling
  `doctor_imessage_present`. The inline message hard-codes "Full Disk Access likely
  revoked" regardless of the actual `cls` — so a BUSY or CANTOPEN error from the
  query leg produces the wrong user instruction. This is the half-fix the story
  was meant to close: the poll-status path goes through the predicate; the
  live-sqlite path does not.
  Fix: replace lines 658-668 with `doctor_imessage_present(alloc,
  hu_imessage_error_class_name(cls), consecutive=0, tripped=false, &pres)` and
  push `pres` the same way the poll-status branch does at line 769.

- `src/doctor.c:673-683` — **`sqlite3_open` failure path also bypasses the
  predicate and has a hard-coded BUSY=FDA misrouting.** The ternary at line 677
  maps `cls == HU_IMESSAGE_ERR_AUTH` to "Full Disk Access denied" but for all
  other classes — including BUSY (SQLITE_LOCKED=6 reaches `sqlite3_open_v2`
  before any query, e.g., WAL writer holds exclusive lock) — it says "see sqlite
  docs". The predicate would give the user the correct BUSY message; the inline
  string does not.
  Fix: same as above — delegate to `doctor_imessage_present`.

## MED (2)

- `src/main.c:615-653` — **`error_class` field missing from `--json` output.**
  AC-9.6.5 in the design document specifies
  `{"check": "imessage_fda", "ok": false, "error_class": "AUTH", "message": "..."}`.
  The JSON serializer at line 628 emits `category` (the `imessage_fda` value) but
  never emits `error_class`. The verifier's contract 8 only verified that
  `category` exists; the AC-9.6.5 `error_class` field was called out as "trivial
  5-line change" in the design and explicitly listed in the DoD but was not
  implemented. Monitoring consumers keying off `error_class` will silently receive
  no field.
  Fix: after emitting `category`, emit `"error_class":"<last_error_class_string>"`
  — the value is already extracted into `err_class[]` by the poll-status parse
  (line 752); it needs to be threaded into the `hu_diag_item_t` or emitted
  separately per item.

- `tests/test_doctor_imessage_diagnose.c:293-299` — **Symbol-reference test does
  not exercise the live-sqlite predicate bypass.** The rule-compliance touchpoint
  takes a function pointer to `hu_doctor_check_imessage` but deliberately avoids
  calling it (correct for `$HOME` safety). The result is that neither the live
  query-error path (HIGH-1) nor the open-failure path (HIGH-2) has any test
  coverage. Both misbehaving branches were reachable via a fixture `chat.db` or a
  stub that returns a controlled `rc`. The test gap is what allowed HIGH-1/2 to
  ship undetected.
  Fix: add two fixture-based tests that call `hu_doctor_check_imessage` with a
  `$HOME` pointing at a temp directory containing a minimal (writable but
  SQLITE_BUSY-returning) sqlite3 file.

## LOW (1)

- `src/doctor.c:553-562` — **Unknown/empty class returns `HU_DIAG_WARN` but
  `category` is `"imessage_other"`.** The design contract lists `"imessage_other"`
  as severity ERR (the OTHER enum branch). The empty-class branch uses WARN and
  the same category string, making the category ambiguous — a monitoring consumer
  cannot distinguish "unclassified sqlite error" (ERR) from "daemon has not polled
  yet" (WARN) because both carry `category=imessage_other`.
  Fix: use a distinct category for the not-yet-polled state, e.g.
  `"imessage_unknown"`, and update the header doc comment and the test in
  `test_doctor_imessage_diag_corrupt_json_is_safe` (line 272) to assert
  `"imessage_unknown"` rather than implicitly accepting whatever the predicate
  returns.

## Cross-agent regression risk

- `src/channels/imessage.c:126-139` — `hu_imessage_classify_sqlite_error` is the
  single authority for the enum-to-string path the predicate depends on. Any
  sprint-9 agent that extends this switch (e.g., to add `SQLITE_CORRUPT` → a new
  class) will produce a string the predicate's `strcmp` chain does not recognize,
  silently falling through to the `imessage_other` / WARN bucket. No guard exists.
  If a future sqlite code gets a new enum variant, `hu_imessage_error_class_name`
  will return it as a new string and the predicate will treat it as unknown/OTHER.
  This is acceptable only because the predicate's `is_known` check covers the
  unknown case — document this coupling in the predicate comment.

RESULT_critic=HAS_FINDINGS story=US-9.6 severity=HIGH
