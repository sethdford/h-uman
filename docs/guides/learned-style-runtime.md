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
| Shape input: the contact's bubbles in the batch joined with `\n`, with daemon-injected notes (`[They sent a photo: …]`, `[Audio transcription: …]`, `[They sent a video]`, attachment placeholders) dropped. Part A classifies the same thing: the inbound bubbles since the owner's last send, joined with `\n` | `hu_learned_style_shape_inbound` |
| Loader: mtime-keyed cache, re-stat at most every 60 s. A missing, malformed or wrong-schema file is treated as absent. It logs one WARN per file version (a rewrite that is still bad logs again) and one per disappearance | same file |
| `hu_learned_style_lookup(contact, shape)`: contact `shape:<x>` bucket, then contact `overall`, then `global`. `hu_learned_style_lookup_for` sets the persona and looks up under one lock | same file; exposed for the length cap (follow-up after #580) |
| One second-person line, with rate clauses only when decisive (≤ 0.2 or ≥ 0.8) | `hu_learned_style_render_line` |
| Fixed-length rule classifier, applied **sentence by sentence**: word/char counts, `MAX N`, `one line`, `brief`/`short` (a sentence that itself says "match" is a mirroring rule and stays) | `hu_learned_style_is_length_rule`, `hu_learned_style_strip_sentences` |
| Per-turn wiring: eligibility, head build, SHADOW count and log | `hu_agent_build_head_learned`, `src/agent/turn/learned_style_turn.c` |

Example line:

    How you text Alex: usually about 25 characters, up to about 90; lowercase
    start most of the time; rarely end with punctuation.

The qualifier "when they ask something" or "when they tell you something big"
appears only when the contact's own shape bucket answered the lookup.

## Where it acts

**The production reactive path is `hu_agent_turn`, not streaming.** The prod
provider (OpenAI-compatible) has streaming off, so `hu_agent_turn_stream_v2`
hands every reply to `hu_agent_turn` at its `!can_stream` check. The daemon's
retry and burst replies also call `hu_agent_turn` directly.
`hu_agent_build_head_learned` builds the head in `agent_turn_run`, where prod
runs. It does the same in `hu_agent_turn_stream_v2` and in the offline
`hu_reply_prompt_render`.

LIVE decides before the head is built, so the head is built once, already
right. The behaviour depends on the head that was **actually** built. The
`lean_prompt` flag does not decide it, because `hu_agent_turn` ignores that
flag:

| Head built | When | LIVE behaviour |
|---|---|---|
| Compact immersive | `HU_PERSONA_HEAD=live` (prod), via `hu_agent_turn` | removes the length sentences of overlay `avg_length`, `style_notes` (first four) and `communication_rules` (first four); the line goes after the channel block |
| Lean | `lean_prompt` on the streaming path, and offline rendering | the same for `style_rules`, `communication_rules`, overlay `avg_length` and `style_notes`; the line goes after the channel style line |
| Full | `HU_PERSONA_HEAD` off (and the compact fail-safe) | no change; the log says `head=full applied=0` |

Removal is per sentence. "Don't perform empathy. Be brief and real." keeps
"Don't perform empathy." An entry with nothing left is omitted.

In LIVE, the contact profile also loses its length sentences. Only `Dynamic:`
and `Pattern:` lines are examined, sentence by sentence. This happens in the
immersive branch of `hu_prompt_build_system` (`learned_style_live`).

Eligible turns: a reactive turn (not `proactive_turn`) whose `memory_session_id`
resolves to a persona contact. Group chat ids and strangers are not persona
contacts, so nothing happens for them. The first name comes from the contact's
`name` (first token); with no name the line says "them".

## The gate

`HU_LEARNED_STYLE=off|shadow|live`, default **off**. It is parsed by
`hu_gate_mode_from_env`.

- **off**: returns before any lookup or log. The prompt is byte-identical (pinned by
  `gate_off_and_shadow_leave_prompt_byte_identical`).
- **shadow**: does the lookup and renders the line. It counts what LIVE would
  remove straight from the persona rules and the contact profile, with no second
  head build and no RAG. The prompt is unchanged. It logs one aggregate line
  per eligible turn, with no handle, no name and no text:

      [learned_style shadow] found=1 level=bucket shape=question n=.. p50=.. p90=.. suppressed_rules=.. line_bytes=.. head=compact applied=0

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
