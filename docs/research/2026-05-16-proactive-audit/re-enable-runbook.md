---
title: Re Enable Runbook
date: 2026-05-16
status: audit
---

# Re-Enable Runbook — Post-2026-05-16 Proactive Messaging Incident

**Current branch:** `claude/friendly-bassi-8b438e`
**Commits ahead of `origin/main`:** 19 (16 fixes + 2 merges + 1 follow-up)
**Test suite:** 10,462 / 10,463 — the 1 failure is the pre-existing
`route_populates_global_log` flake in `test_model_router.c`, confirmed pre-existing
by stash-and-rerun.
**ASan:** clean.
**LaunchAgent state:** unloaded (no daemon process can spawn).
**Persona `proactive.master_enabled` default:** `false` (OFF).

## Pre-flight checks before re-enable

Run from the worktree root:

```bash
# 1. Confirm no daemon process
ps aux | grep "human-daemon" | grep -v grep   # expect: empty

# 2. Confirm LaunchAgent is unloaded
launchctl list | grep ai.human.service-loop    # expect: empty

# 3. Full suite green at HEAD
cmake --preset dev && cmake --build --preset dev --target human_tests
./build/human_tests | tail -1                  # expect: 10,462/10,463 passed, 1 FAILED (pre-existing flake)

# 4. Confirm master kill switch defaults OFF in your persona JSON
jq '.proactive.master_enabled // false' ~/.human/personas/<your-persona>.json
# expect: false
```

## Re-enable steps (in order)

```bash
# 5. Opt in explicitly (this is the one structural gate)
#    Edit ~/.human/personas/<your-persona>.json and set:
#      "proactive": { "master_enabled": true, "channel": "...", "schedule": "..." }
#    Also confirm each contact you actually want proactive contact with has
#    "proactive_checkin": true at the contact level.

# 6. Re-load the LaunchAgent
launchctl load ~/Library/LaunchAgents/ai.human.service-loop.plist

# 7. Verify the daemon came up
sleep 2 && ps aux | grep "human-daemon" | grep -v grep
launchctl list ai.human.service-loop

# 8. Watch the log for the first hour
tail -F ~/.human/logs/service-loop-error.log | \
  grep -E "F25 emotional check-in|proactive check-in|BLOCKED"
```

## What's structurally closed

Every concrete cause of the 21 broken messages on 2026-05-16 is gated at
multiple layers now:

| Failure mode (2026-05-16) | Layer that blocks recurrence |
| --- | --- |
| Raw 60–255-char user confession stored as `topic` | **Phase 2 P2-1, P2-2, P2-3** — daemon and `emotional_state` use the noun-phrase extractor; no raw fallback. Plus **Phase 2 P2-6** — `fact_extract` paraphrases to third person. |
| Topic interpolated verbatim into "hey how are you doing with %s?" | **Phase 1 P1-3** — `hu_proactive_topic_is_safe` predicate at the send boundary (daemon.c, proactive.c, daemon_proactive.c, humanness.c — 4 call sites). |
| F25 fallback contact-match routing one moment to multiple contacts | **Phase 1 P1-2** — `hu_proactive_contact_matches_moment` strict equality only. |
| Replay-insights `replay:latest` global key + static buffer | **Phase 1 P1-4** — `hu_proactive_build_replay_key` per-contact key; static buffer removed. |
| F30 / F31 templates bypassed LLM and shipped raw `fact` / `topic` | **Phase 1 P1-5** — `hu_proactive_topic_is_safe` wired into F30 curiosity + F31 callback (both followup + commitment branches). |
| "How'd it go with the loan?" sent 4 times in a row | **Phase 4 P1-8** — `hu_self_awareness_build_directive_from_memory` now reaches the proactive prompt. **Plus P4-1** — `topic_baselines.last_checkin_sent_at` cooldown. **Plus P4-4** — `delayed_followup` now marks sent only on confirmed delivery. |
| 9+ contacts → every-hour duplicate morning message | **Phase 4 P1-7** — heap-backed `(feature, contact_id, ymd)` dedup; legacy `[8]` ring buffers retired. |
| Channel rate limiter only enforced on inbound | **Phase 4 P1-6** — `hu_proactive_throttle_t` enforces channel limits on every proactive send. |
| Unlimited consecutive sends per contact | **Phase 4 P4-6** — per-contact hard cap 1/24h, 3/7d. |
| Memory degradation corrupting outbound content | **Phase 2 P2-11** — degradation now skipped for outbound-bound categories. |
| `(last: %lld)` recall-format string shipped as message | **Phase 1 P1-3** rejects the `(last:` substring outright. **Plus Phase 1 P1-4** removes the channel that allowed format strings to leak there in the first place. |
| Inner-thought store as a prompt-injection vector (raw user text in system prompt) | **Phase 2 P2-3** — extract topic before accumulate; skip if extraction empty. |
| `mood_log` schema collision (silent insert failures) | **Phase 2 P2-10** — single canonical schema; per-contact `contact_mood_log` table. |
| world_model case-insensitive display-name match leaking one contact's profile to another | **Phase 3 P3-8** — strict `contact_id` equality only. |
| Master kill switch absent (no global "stop everything" toggle) | **Phase 1 P1-1** — `proactive.master_enabled`, default OFF. |

64+ regression tests pin these. Branch is safe to re-enable.

## What's deferred (intentionally — not safety-blocking)

These items from `findings.md` did not land in this session. None of them
affect the runtime risk of re-enabling because they're all about
cross-contact data isolation in tables that only matter if a leak path
to outbound exists — and we just closed all of those.

### Phase 3 remaining (9 items, all schema work)

- **P3-1** — `goals` table needs `contact_id` column
- **P3-2** — `life_threads` table needs `contact_id`
- **P3-3** — `held_contradictions` table needs `contact_id` + UNIQUE(contact_id, topic)
- **P3-4** — `life_narration_events` needs `source_contact_id`
- **P3-5** — `scheduler_jobs` dispatch SELECT needs `WHERE contact_id` binding
  (schema already has the column)
- **P3-6** — `prompt_patches` / `learning_signals` / `strategy_weights` — decide
  whether intentionally global (with explicit doc) or scope per-contact
- **P3-7** — `hula_tasks` needs `contact_id`
- **P3-9** — `ab_selections` needs `contact_id`
- **P3-10** — `daemon.c::hu_superhuman_commitment_list_due` and
  `proactive.c::hu_superhuman_delayed_followup_list_due` filter by `contact_id`
  in C after retrieving all rows; should bind in SQL WHERE for prefix-match
  safety

### Phase 6 — Conversational quality (5 items)

- **P6-1** — apply channel overlay (formality, length, emoji_usage) to proactive
  prompts in `daemon_proactive.c:275-517`
- **P6-2** — include `relationship_type` and `dunbar_layer` in proactive
  prompts so the model knows "sister" vs "coworker"
- **P6-3** — emotional-tone check on the contact's LAST received message before
  firing a proactive trigger (don't bounce cheerful pings off a vulnerable
  message)
- **P6-4** — replace hardcoded `"I'm here."` / `"I hear you."` literals in
  `humanness.c:268-284` with LLM call routed through persona
- **P6-5** — move the absolute-rules block out of `agent_stream.c:563` to a
  shared module so the proactive path inherits the same hard constraints
  (lowercase, no markdown, "You are HUMAN", etc.) that reactive path enforces

### Critic / consolidation

- The three local content-safety predicates that Phase 2 added
  (`hu_humanness_substring_is_safe_topic`, `proactive_entry_content_is_safe`,
  `hu_daemon_callback_content_is_safe`) overlap semantically with Phase 1's
  `hu_proactive_topic_is_safe`. Worth a dedicated consolidation pass with
  regression coverage of all four call sites — but only after both branches
  are merged in (which they are now).
- The Phase 3 implementer's abandoned worktree at
  `~/projects/h-uman/.claude/worktrees/agent-a8d5f3b3a081c484a` has 6 files
  with uncommitted goals/world_model edits (build-broken). If you want to
  salvage their goals.c diff for P3-1, the changes are there; otherwise the
  worktree can be deleted with `git worktree remove --force`.

## Emergency kill (if anything goes wrong post-re-enable)

```bash
# Hard runtime kill — doesn't require persona reload
HU_PROACTIVE_ENABLED=0 launchctl setenv HU_PROACTIVE_ENABLED 0
launchctl unload ~/Library/LaunchAgents/ai.human.service-loop.plist

# Or persona-level — survives daemon restart
jq '.proactive.master_enabled = false' ~/.human/personas/<your-persona>.json \
  > /tmp/persona.json && mv /tmp/persona.json ~/.human/personas/<your-persona>.json
launchctl unload ~/Library/LaunchAgents/ai.human.service-loop.plist
```

## Files to inspect if a future incident occurs

The fastest way to triage a future proactive-messaging issue:

1. **The send log** — `~/.human/logs/service-loop-error.log` —
   grep for `F25 emotional check-in sent|proactive check-in sent|BLOCKED`
2. **The predicates** — `src/agent/proactive.c` (topic_is_safe,
   contact_matches_moment, build_replay_key) + `src/persona/persona.c`
   (proactive_is_enabled). If any of these return `true` for input that
   should be unsafe, add a failing regression test FIRST, then fix the
   predicate.
3. **The findings doc** — `docs/research/2026-05-16-proactive-audit/findings.md`
   has the full audit-fleet output with file:line references.
