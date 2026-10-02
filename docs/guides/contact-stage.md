---
title: Contact stage — per-contact relationship stage from interaction data
created: 2026-10-02
status: operator-facing
---

# Per-contact relationship stage (DEF-16)

`agent->relationship` (NEW / FAMILIAR / TRUSTED / DEEP) feeds the length
calibration, the brief-mode cap, the model-route relationship weight, the
`### Relationship Context` prompt line and inner-world disclosure. It used
to be **one agent-wide state**: loaded from whichever contact spoke first
after a restart and raised by every turn with every contact
(`hu_relationship_update`, 20/80/200 turns → FAMILIAR/TRUSTED/DEEP). A
contact's stage measured the daemon's uptime, not the relationship.

Now `hu_contact_stage_refresh` (`src/agent/turn/contact_stage_turn.c`)
derives the stage of the turn's contact alone, from the session store
(`messages`, counts only) and the persona's declared Dunbar layer as a
prior. Formula and rationale: `include/human/persona/contact_stage.h`.
Volume and active days saturate at the owner's **median** contact, so the
scale adapts to how much this person texts. Turn counting can no longer
raise a derived stage.

It is called where a turn learns its contact: the daemon's per-batch context
load (before length calibration reads the stage) and the turn entry (every
path). Ungated: this is a correctness fix.

## Persistence and migration

The per-contact stage lives in the existing `frontier_state.rel_stage`
column. Existing rows are overwritten with the derived stage on the
contact's next turn; new contacts get a row from the end-of-turn frontier
save. Rows are never inserted early (a new row would make the frontier
loader treat the contact as restored, with column defaults).

## Before / after (prod, counts only, 2026-10-02)

| | NEW | FAMILIAR | TRUSTED | DEEP |
|---|---:|---:|---:|---:|
| before: `frontier_state.rel_stage` (14 rows, snapshots of the shared counter) | 7 | 4 | 3 | 0 |
| after: derived for the 14 contacts with history (8 have a persona prior) | 2 | 3 | 8 | 1 |

Norms: median 186.5 messages, 20 active days. In prod the daemon logs the
same comparison once per process:

```
[contact_stage] distribution contacts=N before=a/b/c/d after=a/b/c/d median_msgs=.. median_days=..
[contact_stage] stage=2 prev=0 q=0.71 prior=3 msgs=947 days=54
```

(`prior` is the stage the declared layer maps to, -1 when none.)

## Rollback

Revert the commit. There is no gate; the persisted rows are rewritten by
whichever code runs next.
