# Silent Config-Gated Subsystems Must Log Once on Disable

Any poll-loop, scheduler, collector, or background-tick subsystem whose
activation is config-gated (typically `if (!cfg->X.enabled) return HU_OK;`)
MUST emit one operator-visible log line on the first invocation when the
subsystem is disabled — so that "this is silently not running" is a
discoverable fact in the service log, not a runtime mystery.

## The hazard

Most config-gated subsystems in this repo follow the same shape:

  hu_error_t hu_daemon_tick_X(const hu_X_config_t *cfg, ...) {
      if (!cfg || !cfg->enabled)
          return HU_OK;          /* ← silent no-op */
      /* … real work … */
  }

Returning HU_OK with no log entry is correct when the operator
INTENDED to disable the subsystem. But when the subsystem is
disabled by **accident** — config block missing entirely, typo in
the key name, default-zero struct, parser rejecting the key as
"unknown" — the operator has no signal that the work isn't
happening. The daemon log looks healthy. The system feels
underpowered. The cause is one missing line of JSON.

Real-world cost of this pattern on 2026-05-18 (h-uman trusting-tharp
session): the `reaction_collection` config block was missing from
`~/.human/config.json`. The daemon had been silently no-op'ing the
iMessage tapback → `dpo_pairs` collector for **months**. 367 DPO
pairs accumulated, ZERO from `source = "imessage_tapback"`. The DPO
judge ran on synthetic pairs only, returning loss=0.6931 (random
baseline) and alignment=0.00. None of this was visible in
`~/.human/logs/service-loop-error.log` — the subsystem returned
HU_OK with no warning. The bug surfaced only via a SQL count audit
of the `dpo_pairs` table.

The same shape (missing config → silent no-op → invisible regression)
applies to: reaction_collection, feeds, proactive throttle,
plugin-discovery polling, the visual-attention governor, and any
future subsystem that adds a `cfg->X.enabled` gate.

## Why the obvious fix is wrong

Switching `return HU_OK` to `return HU_ERR_NOT_SUPPORTED` would
break callers that legitimately expect a disabled subsystem to
return success ("nothing to do this tick"). The control-flow
contract is correct as written.

The fix is **operator visibility**, not control flow: emit one
log line per process lifetime when the subsystem is disabled,
then continue returning HU_OK as before.

## The rule

For every config-gated subsystem tick/poll function:

1. At first invocation when disabled, emit:

   hu_log_info_once(agent_or_subsystem_name,
                    "X subsystem disabled by config "
                    "(cfg->X.enabled=false); set X.enabled=true "
                    "in config.json to activate");

2. `hu_log_info_once` is a `static bool warned = false;` guard
   so the line fires once per process, not once per tick. (If
   the project lacks this helper, add it to
   `include/human/core/log.h` first — it's a 6-line `static
   atomic_bool` wrapper around `hu_log_info`.)

3. When the subsystem is enabled, emit one similar one-shot
   line confirming activation at first tick. Operators
   appreciate the positive confirmation as much as the negative.

4. The disabled-warn message MUST name the config key that
   would enable it. "X disabled" is not enough — operators
   need to know how to fix it without grepping the source.

## Bonus: catch parser-level rejection too

The 2026-05-18 bug had a second silent failure: when the user
DID add `reaction_collection` to config.json but the running
binary predated the parser code, the config parser logged
`[config] unknown key: 'reaction_collection' (ignored)`. This
line went to stderr but was easy to miss in a multi-thousand-line
service log.

For top-level config keys, the parser's `unknown key` warnings
should be ELEVATED to a banner at daemon startup:

  [config] WARNING: 3 unknown config keys ignored: reaction_collection,
  proactivity_gate, foo_collector — these are either deprecated or your
  binary predates the schema. Rebuild from source to use them.

This is a `src/config_parse.c` change, not per-subsystem. Add
the banner once and every subsystem benefits.

## Audit checklist for existing code

Run this audit when this rule lands, OR after any new
config-gated subsystem ships:

  grep -rn '!cfg->.*\.enabled\b' src/ | grep -v '_test\.c'

For each hit, verify the function emits a one-shot info-level
log when the gate trips. Add the line if missing. Pair with a
test:

  static void test_X_disabled_emits_one_shot_warning(void) {
      /* arrange: enabled=false, mock log sink */
      /* act: call hu_daemon_tick_X */
      /* assert: log captured exactly one line containing
         "X subsystem disabled" + the config key name */
  }

## When this rule does NOT apply

- **Per-tick debounce gates** (e.g. "is it time to poll yet?"
  inside a frequency-gated function). Those are temporal, not
  config; logging would spam.
- **Per-request feature flags** (e.g. "should this turn use
  HuLa?"). Those are per-call decisions, not subsystem-on/off.
- **Build-time `#ifdef` gates** (e.g. HU_ENABLE_ML). Those
  fail at link time when missing; no runtime log needed.
- **Subsystems that NEVER no-op** (always either succeed or
  return a specific error). Those already give operators a
  signal.

## Related

- `~/.claude/rules/audit-verify-before-allege.md` — the same
  failure mode at audit time (claim "missing" without verifying
  the gate state).
- `~/.claude/rules/quality-gates.md` — "No silent failures:
  return values checked, errors propagated or logged" — this
  rule is the operational specialization of that principle for
  the specific case of config-gated subsystems.
- `~/.claude/rules/self-improvement.md` — the rule says "if the
  mistake is repeatable, suggest creating a hookify rule to
  prevent it." This file is the rule; a follow-up hookify rule
  could pattern-match `!cfg->.*\.enabled` plus the missing
  `hu_log_info_once` and refuse the commit.
