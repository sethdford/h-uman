# Audit Follow-ups — 2026-05-16

A six-agent parallel audit on 2026-05-16 surfaced a punch list of structural,
security, and architectural gaps. The two highest-impact fixes were landed
in the same session ([commit-ready in this worktree]):

- **Unknown-tool permission masquerading** — introduced `HU_PERM_DENY` sentinel
  ([commit ref: src/security/permission.c]). 4 adversarial tests were rewritten because
  they had been pinning the buggy contract.
- **Sandbox deny-by-default** — `src/tools/shell.c` fork path no longer falls
  through to bare `/bin/sh` when sandbox is configured but unavailable. Pure
  predicate `hu_shell_must_deny_unsandboxed` extracted for unit testing.

This directory holds the specs and plans for the remaining audit items. Each
file is self-contained — assume the implementer has not seen the audit.

## Order of execution

| # | File | Type | Effort | Risk | Why this order |
|---|---|---|---|---|---|
| 1 | [01-persona-overlay-wiring.md](01-persona-overlay-wiring.md) | Spec | M (1–2 weeks) | Med | Unblocks M1 thesis ("channel-aware persona"). 43 channels declare overlay; 0 use it. |
| 2 | [02-vault-encryption-migration.md](02-vault-encryption-migration.md) | Spec | M (1 week) | High | Privacy thesis depends on real encryption. Adds libsodium dep. |
| 3 | [03-hook-pipeline-invocation.md](03-hook-pipeline-invocation.md) | Spec | S (2–3 days) | Med | Closes the second permission-tier gap (hooks skipped on sequential path). |
| 4 | [04-daemon-decomposition.md](04-daemon-decomposition.md) | Plan | L (3–4 weeks) | High | 12,262-line god-object. Big payoff for testability, but high blast radius. |
| 5 | [05-provider-dispatch-cleanup.md](05-provider-dispatch-cleanup.md) | Plan | M (1 week) | Low | Replace 15× `strcmp` provider routing in onboard.c/voice.c/config_merge.c with vtable lookup. |

## Items intentionally NOT in this set

- **M3 frontier-model bridge** — already specified in
  [docs/plans/2026-05-10-m3-frontier-model-bridge.md](../2026-05-10-m3-frontier-model-bridge.md).
  The audit verified that plan accurately represents the gap; no new spec needed.
- **Voice subsystem "stubs"** — audit false positive. The `NOT_SUPPORTED`
  returns in `gemini_live.c` and `realtime.c` are correctly behind `#else`
  branches for `!HU_HTTP_CURL` builds. No action.
- **31 TODO/FIXME markers** — suspiciously low count flagged in audit; spot-
  check did not reveal scrubbing. Low priority. Mine in a future pass.

## How these docs were authored

This index and its child docs were synthesized from a parallel audit of six
read-only `Explore` agents covering: unimplemented surfaces, M3 personalization
integration, channel implementations, security wiring, test health, and
architecture. Findings and refs are preserved verbatim in each spec under
"Audit evidence."
