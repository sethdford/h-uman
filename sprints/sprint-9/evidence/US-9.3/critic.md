# Critic findings — US-9.3 iMessage non-allowlisted courtesy reply

## HIGH (3)

- `src/channels/imessage.c:4209–4219` — **TOCTOU on dedup check vs record.** `hu_imessage_courtesy_dedup_check` reads the log, then `hu_imessage_should_courtesy_reply` returns true, then `imessage_send` is called, then `hu_imessage_courtesy_dedup_record` writes. Two poll-loop invocations arriving within the same ms window (e.g. a burst of stored messages replayed after a BUSY episode) will both pass the check before either writes the record, resulting in two courtesy replies to the same handle in the same bucket. The `flock(LOCK_NB)` on the truncation path does not cover the check-then-record gap. Fix: record first (O_EXCL advisory or atomic `open(O_CREAT|O_EXCL)` on a per-handle-bucket sentinel file), then send; or hold the flock across the entire check-send-record triple.

- `src/channels/imessage.c:4204–4205` — **Day-boundary double-reply is undocumented and untested.** `bucket = time(NULL) / 86400` means a handle that messages at 23:59:59 gets a reply logged to bucket N, and at 00:00:01 the next day gets a fresh reply logged to bucket N+1. This is by design (dedup resets daily), but the verifier comment calls the cap "50/day aggregate" while the actual window is a UTC midnight rollover, not a rolling 24h window. An attacker sending at boundary times gets two replies in under 2 seconds from distinct handles. No test covers the boundary-straddle case and no design doc documents it as accepted. Fix: either document this as intentional in the header comment and add a test, or switch to a rolling window.

- `src/channels/imessage.c:476–503` — **`hu_imessage_courtesy_dedup_check` is fail-open and the fail-open direction is wrong for a rate-limit.** The function returns `false` (meaning "not yet replied") on any I/O failure: missing file, unreadable file, corrupt line, HOME unset. The comment in the header says "we prefer send anyway to silent drop forever after a transient FS hiccup." On a machine where `~/.human/` is temporarily unavailable (NFS home, Time Machine snapshot, disk full), every single non-allowlisted DM will pass the dedup check and attempt a courtesy send — up to 50 per bucket, every poll cycle, for the duration of the outage. The aggregate cap limits total daily sends but does not prevent burst-sends within a single poll. Fix: if the I/O failure is on `fopen` (file does not exist), fail-open is correct (first time); if the file exists but is unreadable, fail-closed (return `true`) is safer.

## MED (3)

- `src/channels/imessage.c:497` — **`strcasecmp` in dedup check vs raw handle written by record.** The record writes the handle verbatim from `chat.db`; the check compares via `strcasecmp`. This is correct for ASCII phone/email handles. However, the allowlist comparison at line 4195 uses `strncasecmp` on the raw `handle` pointer from SQLite (not normalized). A handle with leading/trailing whitespace or a Unicode lookalike (e.g. fullwidth `＋` instead of `+`) will fail the allowlist check, trigger courtesy logic, but may or may not match a prior dedup record depending on what iMessage stored. No normalization step exists between SQLite read and dedup key. Fix: normalize the handle (trim whitespace, NFKC for non-ASCII) once at the SQLite read site and use the normalized form throughout.

- `src/channels/imessage.c:553–560` — **`write()` return value discarded with `(void)` cast but partial-write is possible.** The record write is `(void)write(fd, rec, n)`. On a nearly-full filesystem `write` can return a short count. The entry is silently truncated in the log and the subsequent `imessage_courtesy_parse_line` will fail to parse that line (no space separator), treating it as a corrupt entry and not counting it toward the aggregate. A disk-full attacker-controlled environment could suppress all dedup records while the send still goes out. Fix: check the return value; if short, truncate the file back or at minimum log a warning.

- `src/channels/imessage.c` — **`courtesy_replies_enabled` defaults to `true` with no config-file toggle.** The header comment at line 3581 says "Operators with a non-empty allowlist will immediately start emitting one-per-handle-per-24h courtesy replies." There is no config-file key to disable this and no `human config set imessage.courtesy_replies_enabled false` path documented anywhere in the diff. The field exists on `hu_imessage_ctx_t` but is only settable via `hu_imessage_test_*` test seams. An operator who does not want courtesy replies has no production path to turn them off. Fix: wire `courtesy_replies_enabled` to a config key (`imessage.courtesy_replies`) and document it, or explicitly call out in the design doc that operator opt-out is deferred.

## LOW (1)

- `src/channels/imessage.c:569–574` — **`flock(LOCK_NB)` silently skips truncation on contention.** The comment says "non-fatal on failure (truncation just deferred to the next record)." On a high-spoof-volume day the dedup log may never be truncated if every record attempt races and loses the lock, causing unbounded file growth past `HU_IMESSAGE_COURTESY_LOG_MAX_LINES`. `256 * 256 = 64 KB` is the stated worst case but that assumes the truncation runs; if it never runs the file grows without bound until the daily cap stops new writes. Fix: add a blocking fallback or a periodic truncation job; document the pathological bound.

## Cross-agent regression risk

None identified. The US-9.3 changes are confined to `src/channels/imessage.c` and `include/human/channels/imessage.h`. No shared utilities were modified.

---

RESULT_critic=HAS_FINDINGS story=US-9.3 severity=HIGH
