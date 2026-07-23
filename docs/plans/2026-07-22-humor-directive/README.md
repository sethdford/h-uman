---
title: Humor/teasing directive — gate, measurement, promotion decision
status: active
created: 2026-07-22
last_audit: 2026-07-22
owner: Modeled Person (persona)
related:
  - docs/plans/2026-07-18-sota-roadmap/README.md
  - .claude/rules/feature-gate-requires-measurement.md
  - .claude/rules/classifier-score-plus-flag-gate.md
---

# Humor/teasing directive — gate, measurement, promotion decision

Goal (operator, 2026-07-21): *"as human as possible with all the quirks and fun
and joking and teasing personality I have."*

Three steps. Step 1 mined real teasing exemplars from `chat.db`. Step 2 gave the
conversation arena a way to **measure** humor. Step 3 added a persona directive
and gated its activation on that measurement.

## Why this needed a gate at all

h-uman has already shipped ungated humanness mutators once and regretted it:
disfluency and filler injection produced output like *"Wish wait no I could"*
(see the `humanness_injectors_off` memory and `measured_style_card`). Humor is
the same hazard with a worse failure mode — forced humor does not read as a bug,
it reads as a person trying too hard, which is exactly the thing the product
exists to avoid.

So the directive ships OFF→SHADOW→LIVE per
`.claude/rules/feature-gate-requires-measurement.md`, and "we shipped it and the
tests are green" is explicitly **not** sufficient to promote it.

## What was built

### Step 2 — the HUMOR axis (`scripts/conversation_arena.py`)

The arena judged `overall_humanness`, `voice_consistency`, `engagement`. None of
those detect *"the humor is weak"* or *"the humor is try-hard"*, so a humor
change had nothing to be gated on.

Added **two** signals, deliberately not one:

| signal | meaning | direction |
|---|---|---|
| `humor` | did the humor/teasing LAND, given the openings the conversation offered | higher better |
| `humor_forced` | how try-hard / corny / joke-crammed | **lower better** |

A single `humor` score is gameable: instruct the model to joke more and a naive
judge rates the try-hard higher. `humor_forced` is the independent counter-signal
— the same reasoning as `.claude/rules/classifier-score-plus-flag-gate.md`.

Four `humor_probe` scenarios ship with it:

- `roast_volley` — friend roasts Seth; does he volley or go flat/defensive?
- `straight_line` — earnest absurd setups begging for a deadpan reply
- `self_own` — invites self-deprecation; does he get defensive instead?
- `humor_wrong_moment` — **the important one**: bad family news, then a weak
  deflecting joke from the contact. Does Seth pile on jokes or read the room?

Without `humor_wrong_moment` the axis only ever rewards more jokes, and the
promotion decision would be rigged.

**Restraint scores neutral-high (0.6–0.8), never low.** Not joking when nothing
opened — or when someone is sharing bad news — is correct behaviour, not a miss.
Operator-confirmed 2026-07-22.

### Step 3 — the directive (`src/persona/persona.c`)

`persona.humor.style` was parsed by `analyzer.c` and never rendered into the
system prompt. `hu_persona_build_humor_directive` now renders it, and
`hu_persona_build_prompt` emits it under the gate.

The directive has two halves and the second is **not optional**:

1. *Lean in* — take the opening when there is one: a dry aside, a tease that
   shows you know them, self-deprecation instead of defensiveness, playing along
   rather than answering straight. Short, thrown away, never explained.
2. *Do not force it* — if nothing opened, say the plain thing; **no humor at all
   is a correct reply** and beats a joke you had to reach for; don't open every
   message with a quip; never joke past someone's bad news.

A test asserts half 2 is present whenever the directive is, so a future wording
edit that drops it fails the suite.

Gate: `HU_HUMOR_DIRECTIVE` via `hu_gate_mode_from_env(..., HU_GATE_OFF)`.

| mode | behaviour |
|---|---|
| unset / `off` (default) | nothing emitted |
| `shadow` | directive built, size logged once, **prompt unchanged** |
| `live` | directive appended ahead of the few-shot examples |
| unknown value | fails closed to OFF |

### Also changed — curated exemplars (ungated persona data)

5 of 9 mined exemplars survived review (4 dropped as flat, mis-paired, or
generic). Backup: `~/.human/personas/seth.json.bak-humor-20260722`.

Two were flagged to the operator and **included at their explicit direction**:
the Florida/CIO reply (embeds identifying bio; long for texting) and
*"I mean you climbed on top just fine haha"* (reads as innuendo out of context,
and few-shot exemplars steer replies to every contact).

They were merged into `example_banks[imessage]`, **not** `humor.examples`:
nothing in the C code parses `humor.examples`, so writing there would have been
inert. They were also moved to the **front** of the bank — with `style=NULL` and
an empty topic, `hu_persona_select_examples` scores every example equally, the
sort is a no-op, and only the **first five** reach the prompt. Appending would
have changed nothing.

This displaced the previous first five, which were mined from dating-app-adjacent
threads and contained real handles. See the follow-up chip *"Audit iMessage
example bank for handles + register"* — the remaining ~52 entries were not
reviewed.

**Note:** the exemplars are ungated persona data present in BOTH A/B arms, so the
measurement below isolates the *directive*, not the exemplars.

## Measurement method

Both arms run the **same worktree binary** against the **same four probes** in
deterministic order (`--humor-only` uses a fixed list, not `random.sample`, so
the delta cannot be scenario luck). The only difference is the env var.

```bash
# arm 1
env -u HU_HUMOR_DIRECTIVE python3 scripts/conversation_arena.py \
    --humor-only --conversations 4 --turns 8 --no-dpo \
    --human-bin build/human --tag humor-off
# arm 2
HU_HUMOR_DIRECTIVE=live python3 scripts/conversation_arena.py \
    --humor-only --conversations 4 --turns 8 --no-dpo \
    --human-bin build/human --tag humor-live
```

`--no-dpo` keeps A/B artifacts out of `dpo_pairs` and the nightly learning loop.

### Promotion rule (`compare_arms`)

Vetoes, not a weighted score. A composite would let a large humor gain paper over
a voice loss — and "humor up, voice down" *is* the forced-humor signature.

| veto | threshold |
|---|---|
| humor gain too small | `< +0.02` |
| voice regressed | `voice_consistency` drops `> 0.10` |
| overall humanness regressed | drops `> 0.10` |
| try-hard | `humor_forced > 0.50` |
| missing arm data | either arm `n == 0` |

Thresholds are the operator's **bias-to-shipping** setting (2026-07-22). The
vetoes themselves are structural and not tunable away.

## Results

Run 2026-07-22, 4 humor probes per arm, 8 turns each, `gemma-4-31b-it-8bit`
via local MLX, judged by `gemini-3.1-pro-preview`.

**A third arm had to be added.** The first OFF-vs-LIVE pair produced deltas too
large to believe (+0.51 overall_humanness from ~680 bytes of instruction) and a
self-contradiction: `humor_forced = 0.80` in the arm with *no humor directive*.
Reading the transcripts explained it — see "The confound" below. `humor-baseline`
is the honest comparison; `humor-off` is retained because it is evidence in its
own right.

| arm | exemplars | directive | what it represents |
|---|---|---|---|
| `humor-baseline` | original order | OFF | production as of 2026-07-22 |
| `humor-off` | curated, front-loaded | OFF | the intermediate state — **never ship this** |
| `humor-live` | curated, front-loaded | LIVE | the proposed shipping unit |

| axis | baseline | off | live | live − baseline |
|---|---|---|---|---|
| `humor` | 0.500 | 0.350 | **0.700** | **+0.200** |
| `humor_forced` (lower better) | 0.300 | 0.600 | **0.150** | **−0.150** |
| `overall_humanness` | 0.637 | 0.300 | **0.812** | **+0.175** |
| `voice_consistency` | 0.700 | 0.400 | **0.812** | **+0.113** |
| `engagement` | 0.675 | 0.525 | **0.825** | **+0.150** |

`compare_arms(rows, "humor-baseline", "humor-live")` → **promote: True**, all
criteria met. Every axis improves; no veto fires.

### The confound — why `humor-off` is worse than baseline

Three of the five curated exemplars open with `lol` / `lol 😂`. Front-loaded into
the few-shot window with no directive, the model generalised *"open every message
with a laugh token"* and collapsed into a template — **12 of 16 Seth turns in
`humor-off` began with "Ha"**:

> Ha shut up... I'm just cautious with my tech
> Ha fingerprints on the timer are a nightmare
> Ha just leave me and my clean microwave alone

Judge: *"a massive AI tell of a system trying to simulate a casual laugh but
doing it mechanically"*, *"Felt like a bot programmed with a nervous laugh"*.

The directive's *"do not open every message with a quip"* is exactly what breaks
this, which is why LIVE recovers. The lesson generalises: **the curated exemplars
and the directive are one shipping unit.** Front-loading teasing examples without
the do-not-force instruction is a regression against production, not an
improvement. Do not revert one without the other.

For contrast, `humor-live` on the same probes:

> turns out my LinkedIn is lying about the architect part
> damn. I'm sorry... honestly forget about me, how are you holding up?

The second is `humor_wrong_moment` — correct restraint, scored `humor=0.70`,
`humor_forced=0.10`.

### Incidental finding — persona-prompt echo

Two `humor-off` turns emitted verbatim system-prompt content as messages:
`"to be honest -> tbh"` (literally in the prompt's "Typing quirks" line) and
`"Empathy first"`. The arena calls MLX directly and bypasses the outbound
pipeline, so this is raw model output and is **not** proof the shape reaches real
contacts — but G10 covers deliberation/critique leaks and has no prompt-echo
rule. Filed as a follow-up chip, scoped to verify-before-fixing.

### Honest limits of this measurement

- **n = 4 conversations per arm.** Small. Per-scenario variance is large — the
  baseline arm ranged from `humor=0.20, forced=0.80` (`self_own`) to
  `humor=0.80, forced=0.10` (`straight_line`).
- One run per arm, no repetition; no confidence intervals.
- A single judge model, and the rubric wording is itself a design choice.
- The comparison bundles two changes (exemplars + directive) against baseline.
  That is the right unit for a *ship* decision, but it does not isolate the
  directive's own contribution. `humor-off` shows the exemplars alone are
  negative, so the directive is doing the load-bearing work.

Treat +0.113 on `voice_consistency` as directional, not precise. The result is
strong enough to ship under a bias-to-shipping bar; it is not strong enough to
quote as a headline number.

### Reproducing

```bash
# baseline arm needs a frozen copy of the pre-change persona:
python3 - <<'PY'
import json, pathlib
bak = pathlib.Path('~/.human/personas/seth.json.bak-humor-20260722').expanduser()
out = pathlib.Path('~/.human/personas/seth-baseline.json').expanduser()
d = json.loads(bak.read_text()); d['name'] = 'seth-baseline'
out.write_text(json.dumps(d, ensure_ascii=False, indent=1) + '\n')
PY
python3 scripts/conversation_arena.py --humor-only --conversations 4 --turns 8 \
    --no-dpo --persona seth-baseline --human-bin build/human --tag humor-baseline
```

(The temporary `seth-baseline.json` was removed after the run so no stray persona
is loadable; recreate it with the snippet above.)

## Decision

**PROMOTED to `live`, 2026-07-22.**

The measurement cleared every criterion against the true production baseline, and
the operator's bar was set to bias-to-shipping. Deployed:

- `HU_HUMOR_DIRECTIVE=live` added to `ai.human.service-loop.plist`
  (backup: `ai.human.service-loop.plist.bak-humor-<ts>`; the installer preserves
  operator gates, and all 16 pre-existing gates survived — 17 total).
- New binary installed via `scripts/install-human-daemon.sh` (atomic `mv`, never
  `cp` — see `.claude/rules/never-cp-over-running-binary.md`).
- Verified: `HU_HUMOR_DIRECTIVE=live` present in the running daemon's environment
  (`ps eww` **and** `launchctl print`); the deployed binary renders the directive
  under the live gate and omits it when unset; `doctor imessage` 5 ok / 0 errors;
  daemon stable.

### Why promotion was also the *safe* action here

Between the exemplar merge and this deploy, production was running the **`humor-off`
configuration** — new front-loaded exemplars against a deployed binary with no
directive code, i.e. the measured-worst arm. That was a live regression introduced
by this work. Promoting resolved it; the alternative would have been reverting the
exemplar order. Reverting the exemplars alone would also have been correct — doing
neither would not.

### What to watch

- `humor` and `humor_forced` on the scheduled arena runs (6×/day). If
  `humor_forced` trends above ~0.35 on non-probe scenarios, the directive is
  generalising into forced humor and should go back to `shadow`.
- Any recurrence of a single-opener template ("Ha …", "lol …") across consecutive
  turns — that is the `humor-off` signature and means the exemplars are dominating
  the directive again.

### Rollback

```bash
/usr/libexec/PlistBuddy -c "Set :EnvironmentVariables:HU_HUMOR_DIRECTIVE off" \
    ~/Library/LaunchAgents/ai.human.service-loop.plist
launchctl kickstart -k gui/$UID/ai.human.service-loop
```

Note that rolling the directive back to `off` **without** also restoring
`~/.human/personas/seth.json.bak-humor-20260722` reproduces the `humor-off` arm —
the worst measured configuration. Roll back both, or neither.
