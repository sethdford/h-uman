#!/usr/bin/env python3
"""Pins for the training resource preflight (2026-07-26 crash-loop fix).

Four reboots on 2026-07-26 (04:01, 05:02, 06:45, 14:38) came from LoRA training
running concurrently with the mlx-server holding the SAME base resident: 11 runs
that day, six inside 28 minutes, driving a 128 GB machine to 154 MB free /
28 GB compressed / 13.2 GB swap. The trainer exiting recovered 53 GB instantly.

These pin the DECISION predicate (pure, fact-injected) plus the two parsers, so
the truth table is covered without a 56 GB model, a real flock, or a live server.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import training_loop  # noqa: E402

fails = 0


def ok(name, cond, detail=""):
    global fails
    print(("  PASS  " if cond else "  FAIL  ") + name + ("  " + detail if detail and not cond else ""))
    if not cond:
        fails += 1


GB = 1024 ** 3
decide = training_loop.training_preflight_decision

# ── 1. The crash condition itself: same base already being served ────────────
allowed, why = decide(need_bytes=76 * GB, available_bytes=100 * GB, serving_conflict=True)
ok("refuses when the production server holds the same base", not allowed)
ok("names co-residency as the reason", "co-reside" in why, why)

# Serving conflict must win even when memory looks plentiful — that is precisely
# the case vm_stat reads optimistically (server pages still counted reclaimable).
allowed, _ = decide(need_bytes=1 * GB, available_bytes=120 * GB, serving_conflict=True)
ok("co-residency outranks a healthy memory reading", not allowed)

# ── 2. Memory precondition ───────────────────────────────────────────────────
allowed, why = decide(need_bytes=76 * GB, available_bytes=25 * GB)
ok("refuses when memory is short", not allowed)
ok("reports both need and available", "76" in why and "25" in why, why)

allowed, _ = decide(need_bytes=76 * GB, available_bytes=90 * GB)
ok("allows when memory is sufficient", allowed)

# need_bytes == 0 means "could not estimate" and must NOT refuse: an unknown or
# local base would otherwise be permanently un-trainable.
allowed, _ = decide(need_bytes=0, available_bytes=1 * GB)
ok("unknown model size does not block training", allowed)

# Boundary: available exactly equals need is allowed (refuse only when strictly less).
allowed, _ = decide(need_bytes=76 * GB, available_bytes=76 * GB)
ok("need == available is allowed (boundary)", allowed)

# ── 3. Single-flight lock ────────────────────────────────────────────────────
allowed, why = decide(need_bytes=0, available_bytes=999 * GB, lock_held=True)
ok("refuses when another run holds the lock", not allowed)
ok("names single-flight as the reason", "single-flight" in why, why)

# ── 4. Training window ───────────────────────────────────────────────────────
NIGHT = (120, 300)  # 02:00-05:00
allowed, _ = decide(need_bytes=0, available_bytes=999 * GB, now_minutes=180, window=NIGHT)
ok("allows inside the window (03:00 in 02:00-05:00)", allowed)

allowed, why = decide(need_bytes=0, available_bytes=999 * GB, now_minutes=14 * 60 + 41, window=NIGHT)
ok("refuses outside the window (14:41 — the run that was live)", not allowed)
ok("window refusal names the window", "02:00-05:00" in why, why)

# Boundaries: start is inclusive, end exclusive.
ok("window start is inclusive",
   decide(need_bytes=0, available_bytes=999 * GB, now_minutes=120, window=NIGHT)[0])
ok("window end is exclusive",
   not decide(need_bytes=0, available_bytes=999 * GB, now_minutes=300, window=NIGHT)[0])

# A window crossing midnight must work — 22:00-04:00 contains 23:00 and 01:00.
CROSS = (22 * 60, 4 * 60)
ok("midnight-crossing window contains 23:00",
   decide(need_bytes=0, available_bytes=999 * GB, now_minutes=23 * 60, window=CROSS)[0])
ok("midnight-crossing window contains 01:00",
   decide(need_bytes=0, available_bytes=999 * GB, now_minutes=60, window=CROSS)[0])
ok("midnight-crossing window excludes 12:00",
   not decide(need_bytes=0, available_bytes=999 * GB, now_minutes=12 * 60, window=CROSS)[0])

# No window configured = no time restriction (back-compat for manual/CI runs).
ok("window=None imposes no time restriction",
   decide(need_bytes=0, available_bytes=999 * GB, now_minutes=14 * 60, window=None)[0])

# ── 5. Window parser ─────────────────────────────────────────────────────────
ok("parses 02:00-05:00", training_loop.parse_train_window("02:00-05:00") == (120, 300))
ok("parses midnight-crossing 22:00-04:00",
   training_loop.parse_train_window("22:00-04:00") == (1320, 240))
ok("tolerates surrounding whitespace",
   training_loop.parse_train_window(" 02:00 - 05:00 ") == (120, 300))
for bad in ("", "nonsense", "02:00", "25:00-05:00", "02:60-05:00", "abc-def", None):
    ok(f"rejects malformed window {bad!r}", training_loop.parse_train_window(bad) is None)
# A zero-width window would refuse every run; treat it as unset instead.
ok("zero-width window reads as unset", training_loop.parse_train_window("03:00-03:00") is None)

# ── 6. Serving-base detection is PORT-FILTERED ───────────────────────────────
# A spare eval server must never be mistaken for production, or training would
# refuse (or worse, target the wrong base) because of an unrelated process.
PS = (
    "python mlx-server.py --model mlx-community/gemma-4-31b-it-8bit --port 8747 --realtime\n"
    "python mlx-server.py --model mlx-community/GLM-4.5-Air-4bit --port 8741 --realtime\n"
)
ok("reads the base of the server on the production port",
   training_loop.serving_base_from_ps(PS, port="8741") == "mlx-community/GLM-4.5-Air-4bit")
ok("ignores a spare server on another port",
   training_loop.serving_base_from_ps(PS, port="8743") is None)
ok("a server with no --port defaults to 8741",
   training_loop.serving_base_from_ps(
       "python mlx-server.py --model foo/bar --realtime\n", port="8741") == "foo/bar")
ok("no mlx-server at all reads as None (not a config fallback)",
   training_loop.serving_base_from_ps("some other process\n", port="8741") is None)

print(("FAILED" if fails else "PASSED") + f" ({fails} failures)")
sys.exit(1 if fails else 0)
