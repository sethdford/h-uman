---
title: Learned style profile — runtime (HU_LEARNED_STYLE)
created: 2026-10-01
status: operator-facing
---

# Learned style profile — runtime

The persona JSON carries hand-written length rules such as "Default 5-15
words", "MAX 15 words" and "Be brief". Nobody re-measures them. A nightly
learner (Part A, covered in its own guide) measures how the owner actually
replies, per contact and per inbound shape, from his own sent iMessages. It
writes only numbers to:

    <persona dir>/<persona>.learned-style.json      schema learned-style/v1

This page covers the C side: how the daemon reads that file and what the
`HU_LEARNED_STYLE` gate changes. The persona JSON is never modified.

## What the runtime does

| Piece | Where |
|---|---|
| Inbound shape rule (`question` / `story` / `casual`), identical to the learner's | `hu_learned_style_shape`, `src/persona/learned_style.c` |
| Loader: mtime-keyed cache, re-stat at most every 60 s; missing, malformed or wrong-schema file = absent, one WARN per process | same file |
| `hu_learned_style_lookup(contact, shape)`: contact `shape:<x>` bucket, then contact `overall`, then `global` | same file; exposed for the length cap (follow-up after #580) |
| One second-person line, with rate clauses only when decisive (≤ 0.2 or ≥ 0.8) | `hu_learned_style_render_line` |
| Fixed-length rule classifier (word/char counts, `MAX N`, `one line`, `brief`/`short`, but not inside a "match the energy" rule) | `hu_learned_style_is_length_rule` |
| Per-turn wiring: eligibility, head rebuild, SHADOW log | `hu_agent_learned_style_apply`, `src/agent/turn/learned_style_turn.c` |

Example line:

    How you text Alex: usually about 25 characters, up to about 90; lowercase
    start most of the time; rarely end with punctuation.

The qualifier "when they ask something" or "when they tell you something big"
appears only when the contact's own shape bucket answered the lookup.

## Where it acts

The call sits in `hu_agent_turn_stream_v2` (`src/agent/agent_stream.c`) right
after the persona head is built and before anything is appended to it. It is
also in `hu_reply_prompt_render`, so offline rendering matches what is served.

| Head in use | When | LIVE behaviour |
|---|---|---|
| Lean (`hu_agent_build_lean_persona_head`) | `llm_decides` channels, which includes production iMessage | rebuilt without length-imposing `style_rules`, `communication_rules`, overlay `avg_length` and `style_notes`; line after the channel style line |
| Compact immersive | `HU_PERSONA_HEAD=live`, non-`llm_decides` | same suppression for overlay `avg_length`, `style_notes` and the first four `communication_rules` (the compact head never renders `style_rules`); line after the channel block |
| Full | neither of the above | no change; the log says `head=full applied=0` |

In LIVE, the contact profile also loses its length sentences. Only the
`Dynamic:` line is edited (sentence by sentence) and only `Pattern:` lines
are dropped. This happens in the immersive branch of `hu_prompt_build_system`
(`learned_style_live`).

Eligible turns: a reactive turn whose `memory_session_id` resolves to a
persona contact. Group chat ids and strangers are not persona contacts, so
nothing happens for them. The first name comes from the contact's `name`
(first token); with no name the line says "them".

## The gate

`HU_LEARNED_STYLE=off|shadow|live`, default **off**. It is parsed by
`hu_gate_mode_from_env`.

- **off**: returns before any lookup or log. The prompt is byte-identical (pinned by
  `gate_off_and_shadow_leave_prompt_byte_identical`).
- **shadow**: does the lookup, renders the line, builds the would-be head only
  to count suppressions, then discards it. The prompt is unchanged. It logs one
  aggregate line per eligible turn, with no handle, no name and no text:

      [learned_style shadow] found=1 level=bucket shape=question n=.. p50=.. p90=.. suppressed_rules=.. line_bytes=.. head=lean applied=0

- **live**: applies the change. It logs the same line with `[learned_style live]`
  and `applied=1`.

## Promoting shadow → live

1. Run SHADOW for at least 7 days. In the service log, confirm `found=1` on most
   eligible turns, and that `level=global` stays a minority. If it doesn't, the
   learner is omitting too many contacts.
2. Run the drift self-check, `scripts/learned_style_drift.py` (Part A). Record
   the per-contact KS and the median ratio of h-uman's length to Seth's.
3. Run the blind A/B with `HU_LEARNED_STYLE=live` on the candidate arm
   (`scripts/blind_ab_gate.py`). Promote only if detection does not rise
   against the current arm, AND a re-run of the drift check after ≥ 3 LIVE
   days shows the KS for contacts with n ≥ 20 moving toward the learned
   distribution (lower), with none above 0.35.

Rollback: set `HU_LEARNED_STYLE=off` (or remove it) in the service-loop plist
environment and reinstall with `scripts/install-human-daemon.sh`. Nothing is
persisted by the runtime, so there is nothing to undo.

## Not in this change

- **Length cap.** The cap (#580) does not use the lookup yet. Wiring it is a
  follow-up after both PRs merge.
- **Time and pace buckets.** `time:*` and `pace:rapid` are stored by the learner
  but not used by the runtime in v1.
- **Non-streaming fallback.** `hu_agent_turn` is not wired:
  `agent_turn.c` is pinned at its line ceiling, and the daemon's reactive path
  is the streaming one.
