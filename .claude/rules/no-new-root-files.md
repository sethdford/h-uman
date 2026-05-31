# No-New-Root-Files Ratchet — Loose .c Files May Only Shrink

The count of `.c` files at `src/` root (not inside a bounded-context subdirectory)
is frozen at a baseline and may only **decrease**.

## The hazard (E0 + E1)

Currently, 101 source files live loose at `src/*.c` with no bounded-context directory.
This violates the Domain-Driven Design principle that every source belongs to exactly
one context — `src/channels/`, `src/persona/`, `src/agent/`, etc. The 101 loose files
are the refactor target for **Phase E1** (Bounded-Context Relocation).

Baseline at Phase E0 (2026-05-31): **101** loose files at `src/` root.

## The rule

New source code MUST be placed inside a bounded-context directory, not at `src/` root.
If a file is not logically part of an existing context, the context boundary must be
clarified FIRST — then the file lands in the right `src/<context>/` directory.

As each Phase-E1 relocation chip moves a file from `src/X.c` to `src/<context>/X.c`,
lower the `ROOT_BASELINE` constant to lock the gain — the ratchet only tightens.
When `ROOT_BASELINE` reaches 0, the gate flips to a hard "reject any loose .c file"
rule wired into `.githooks/pre-commit`.

## Why a ratchet, not an absolute gate at 0

The Phase-E1 plan relocates all 101 files into their contexts. That work is staged
in parallel chips over weeks. A hard "0 loose files allowed" gate would fail CI today
and block active work. A ratchet allows the build to stay green now (baseline = 101)
while forcing monotonic improvement: every commit that lands must shrink or hold the
count, never grow it.

## Enforcement

`scripts/check-no-new-root-files.sh`, wired into `.githooks/pre-commit`
(fires when a `src/` C file is staged). When baseline reaches 0, the enforcement
gates flip to an absolute "no .c at src/ root" predicate.

## Related

- `docs/plans/2026-05-29-ddd-bounded-contexts/phase-E1-root-sprawl-to-modules.md` — the
  relocation schedule that drives `ROOT_BASELINE` down to 0
- `docs/standards/engineering/bounded-contexts.md` — the context definitions
  that tell you where each loose file should go
- `scripts/check-no-new-root-files.sh` — the enforcing script; flips to a hard gate once
  `ROOT_BASELINE=0` (Phase E1 complete)
