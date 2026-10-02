---
title: Contact stage — HU_REL_STAGE_DERIVED, per-contact relationship stage
created: 2026-10-02
status: operator-facing
---

# Per-contact relationship stage (`HU_REL_STAGE_DERIVED`, DEF-16)

`agent->relationship` (NEW / FAMILIAR / TRUSTED / DEEP) feeds the length
calibration, the brief-mode cap, the model-route relationship weight, the
`### Relationship Context` prompt line and inner-world disclosure. It is
**one agent-wide state**: loaded from whichever contact spoke first after a
restart and raised by every turn with every contact
(`hu_relationship_update`, 20/80/200 turns → FAMILIAR/TRUSTED/DEEP). A
contact's stage measures the daemon's uptime, not the relationship.

`hu_contact_stage_refresh` (`src/agent/turn/contact_stage_turn.c`) derives
the stage of the turn's contact alone. Formula and rationale:
`include/human/persona/contact_stage.h`.

| Signal | Source | Never |
|---|---|---|
| volume, active days | the contact's own messages in the session store (`messages`, role `user`) | the `assistant` rows: those are the twin's replies |
| reciprocity | Seth's own reply turns per contact from the learned-style profile (`<persona>.learned-style.json`, `contacts.<handle>.overall.n`, chat.db with the daemon's sends attributed away) | the twin's replies; a contact the profile does not cover gets a neutral value |
| prior | the persona's declared Dunbar layer (or `relationship_stage`) | |

Volume and active days saturate at the owner's **median** contact, so the
scale adapts to how much this person texts. The norms and the profile's reply
counts are cached for an hour; a turn costs one indexed per-contact count.

It is called where a turn learns its contact: the daemon's per-batch context
load (before length calibration reads the stage) and the turn entry.

## Gate

| Mode | Effect |
|---|---|
| `off` (default) | Nothing: the old agent-wide stage, no query, no write. |
| `shadow` | Derives and logs `[contact_stage shadow] prev=… stage=… failed=… q=… prior=… inbound=… days=… seth_replies=…`. Changes nothing and writes nothing. |
| `live` | `agent->relationship.stage` = the derived stage; turn counting can no longer raise it. The derived row goes to its own table, `contact_rel_stage`. `session_count` and `total_turns` keep their old meaning and are not touched, so `frontier_state.rel_*` holds exactly what it held before. A failed derivation (no memory backend, query error) resets the stage to the persona prior alone (NEW without one), never the previous contact's. |

Each norms recompute (hourly, shadow or live) also logs the distribution:

```
[contact_stage] distribution contacts=N before=a/b/c/d after=a/b/c/d median_inbound=.. median_days=.. reply_ratio=.. profile_contacts=..
```

`before` is `frontier_state.rel_stage` (snapshots of the shared counter),
`after` the derived stages. Counts only; no contact ids.

## Before / after (prod, counts only, 2026-10-02)

| | NEW | FAMILIAR | TRUSTED | DEEP |
|---|---:|---:|---:|---:|
| before: `frontier_state.rel_stage` (14 rows) | 7 | 4 | 3 | 0 |
| after: derived for the 14 contacts with history (8 with a persona prior) | 2 | 3 | 8 | 1 |

Norms: median 127.5 inbound messages, 20 active days. The learned-style
profile does not exist on this machine yet, so every contact's reciprocity is
neutral until the nightly learner writes it.

## Promotion: SHADOW → LIVE

1. Run `shadow` for a week. Read the `distribution` lines and a sample of
   `prev → stage` pairs; Seth confirms the derived stage of each contact he
   has a view on (counts only in the log; he maps them himself).
2. The real-turn replay (`docs/guides/replay-harness.md`) with
   `--arm prod: --arm stage:HU_REL_STAGE_DERIVED=live` must show the length
   KS against Seth not worse.
3. Flip to `live`.

## Rollback

Set `HU_REL_STAGE_DERIVED=off` (or unset it). Nothing else is needed: the old
`frontier_state` columns were never rewritten, and `contact_rel_stage` is
only read by this code.
