# Seth-Aliveness — Requirements

> The three "aliveness" subsystems — proactive initiative, memory salience, and
> iMessage room-reading — are each ~80% built and tested. What's missing is the
> last wiring that makes them fire with the right context at the right moment.
> This spec closes only the gaps that a verify-first audit (2026-05-29) confirmed
> are GENUINELY missing or partial. Claims that earlier exploration flagged as
> "stubbed/missing" but turned out already-wired (mark_read call sites, inbound
> reply threading, follow-up quiet-hours via chronotype snapping) are explicitly
> out of scope — see Non-goals.

## Background (verified against code, 2026-05-29, HEAD f35e12bc)

- **Initiative**: `hu_init_proposer` decides whether to send an unprompted message.
  Its context fields `HU_INIT_FIELD_{PERSONA,MEMORY,PERSONAL_MODEL,AWARENESS,STM}`
  are hard-zeroed (`src/agent/init_proposer.c:189-193`, "T2 stub" comment). The
  agent struct already holds `personal_model` (`include/human/agent.h:213`) and
  `memory` — they're just not plumbed to the proposer. The proposer scores a
  decision against essentially empty context today.
- **Memory leak**: the live iMessage reply path (`src/agent/autoresponder.c:493`)
  calls `hu_personal_model_build_prompt`, whose emotional/anticipatory walk
  (`src/memory/personal_model.c:800-870`) emits EVERY distinct contact's private
  context (capped at 8) into the system prompt — even when replying to one
  person. The contact-scoped loader `hu_personal_model_load_for_contact`
  (`personal_model.c:3044`) exists and IS used at line 500, but the broadcast at
  line 493 leaks contact Y's "her mother is sick" into a reply to contact X. The
  in-code comment (`personal_model.c:793-796`) documents this as intentional;
  this spec narrows it to the in-scope contact.
- **Temporal decay**: `temporal_decay_factor` defaults to `0.0` (disabled) at
  `src/agent/memory_loader.c:167`. The decay machinery exists
  (`src/memory/retrieval/engine.c:62-65`, `hu_temporal_decay_score`) but never
  runs, so a 2-year-old memory ranks identically to yesterday's on the recency
  axis.
- **Scheduled-send threading**: inbound reactive replies thread correctly
  (`hu_daemon_dispatch_imessage_reply`, `daemon.c:975-1023`). But scheduled/async
  sends (`hu_conversation_flush_scheduled_for`, `daemon.c:1476-1525`) pass
  `NULL, 0` for parent guid (`daemon.c:1523`) — proactive and follow-up messages
  land as free-floating new messages, not thread replies.
- **Banter speed**: `hu_followup_compute_send_time` (`src/follow_up.c:39-89`)
  implements warmth-based base delay, seeded jitter, chronotype hour-snapping
  (which already covers 3am avoidance), and a 24h staleness cutoff — but NO
  register-aware fast path: a one-word banter reply is delayed on the same
  minutes-to-hours schedule as a substantive message.

## User stories

- As Seth, I want a proactive message to be **grounded in who I am and what we
  last discussed**, so an unprompted ping reads like me, not a random nudge.
- As Seth, I want a reply to one person to reflect **our specific thread**, not be
  colored by every other relationship's private context at once.
- As Seth, I want **stale memories to fade** so recent context wins when it should.
- As Seth, I want proactive/follow-up iMessages to **land in-thread**, like a human
  replying to a specific message.
- As Seth, I want **banter answered at banter speed** — a quick quip shouldn't sit
  on the same delay schedule as a thoughtful reply.

## Acceptance criteria

### Phase A — Initiative with real context
- [ ] **AC-A1**: `hu_init_proposer` populates the `PERSONAL_MODEL` and `MEMORY`
  context slots (today zeroed at `init_proposer.c:189-193`) from `agent->personal_model`
  and `agent->memory` before scoring a proposal, and populates the `AWARENESS`
  slot with the current local time-of-day. The PERSONAL_MODEL slot MUST be rendered
  via the contact-scoped builder (AC-B1's `_for_contact`) for the proposer's chosen
  target contact, so the populated context is the SPECIFIC person's, not a broadcast.
  Test: a proposer run on an agent with a populated personal_model yields nonzero
  `bytes[HU_INIT_FIELD_PERSONAL_MODEL]` and nonzero `bytes[HU_INIT_FIELD_AWARENESS]`,
  AND the rendered PERSONAL_MODEL slot for target-contact X contains X's
  contact-specific tokens while excluding a different contact Y's private context
  (the SOTA "proactive must reference specific shared context" requirement, CHI 2025
  3714002 — grounding must reach generation, not merely the confidence gate); an
  agent with no personal_model/memory yields byte-identical output to today
  (safe-default preserved).

### Phase B — Memory that reads the thread
- [ ] **AC-B1**: When a contact is in scope, `hu_personal_model_build_prompt`'s
  emotional/anticipatory walk emits ONLY the in-scope contact's context, not all
  distinct contacts'. Test: build the prompt with contact X in scope when the model
  holds facts for X and Y — X's emotional context appears, Y's does not. With NO
  contact in scope, behavior is byte-identical to today (broadcast preserved for
  non-contact-scoped callers).
- [ ] **AC-B2**: `temporal_decay_factor` defaults to a nonzero value so recency
  weights retrieval ordering, re-evaluated at recall time. Test: of two memories
  with equal base salience, the more recent ranks higher after decay; setting the
  factor back to `0.0` reproduces today's ordering exactly.

### Phase C — iMessage at human timing
- [ ] **AC-C1**: Scheduled/async iMessage sends carry `parent_guid` so they thread.
  Test: a scheduled reply whose stored context names a parent message produces a
  send/reply call carrying that parent guid (not `NULL`); a scheduled send with no
  known parent still sends flat (no regression).
- [ ] **AC-C2**: `hu_followup_compute_send_time` (or a register-aware wrapper)
  returns a fast send time for short/casual banter messages while keeping the
  warmth/chronotype schedule for substantive ones. Test: a one-word casual message
  schedules a markedly shorter delay than a multi-sentence substantive message to
  the same contact; quiet-hours snapping still applies to both.

## Non-goals

- **Not** adding a "Seth engages → mark read" trigger — `mark_read` already fires
  reactively at `daemon.c:6122/6131/10724`; a user-engagement trigger is ill-defined
  (Seth reads on his own device) and not a genuine gap.
- **Not** re-implementing inbound reply threading — already wired (`daemon.c:975-1023`).
- **Not** adding 3am-avoidance to follow-ups — chronotype hour-snapping
  (`follow_up.c:68-80`) already covers it.
- **Not** training/altering the LoRA adapter or persona JSON (M3/recipe frozen).
- **Not** building new memory/fact/initiative subsystems — wiring existing ones.
- **Not** auto-enabling `initiative.enabled` (operator opt-in stays default-off).
- **Not** incoming read-receipt parsing (the one genuinely-missing iMessage piece;
  deferred to a follow-up spec).
- **Not** changing streaming (settled by `f35e12bc`).
- **Not** proactive intent-type classification or suppression learning — SOTA-worthy
  (Poppy 2026-05-13, arXiv 2509.07438) but a distinct initiative-layer body of work;
  deferred to **Phase D** (`docs/plans/2026-05-29-seth-aliveness/phase-d-intent.md`).
- **Not** memory consolidation (episodic→semantic summarization) — the one real
  architectural gap vs. 2026 SOTA (arXiv 2603.07670), but its own subsystem;
  deferred to **Phase E** (`docs/plans/2026-05-29-seth-aliveness/phase-e-consolidation.md`).
- **Not** message-burst segmentation or dynamic turn-velocity matching — research has
  no shipped measurement standard yet (Technologies 2025 13/12/591); future.

## SOTA grounding (web research, May 2026)

Three grounded research passes confirmed the core 5 ACs are SOTA-aligned and in
several cases ahead of shipped products:
- **B1/B2** match the documented 2026 memory direction: filter-at-reply-time +
  theory-of-mind-per-contact (arXiv 2509.22887), per-category half-life decay
  (mem0 state-of-2026, FOREVER arXiv 2601.03938).
- **A1** addresses the highest-leverage proactive finding: a proactive message reads
  human only when it references SPECIFIC prior context (CHI 2025 3714002) — hence the
  A1 amendment requiring contact-scoped grounding to reach generation.
- **C1/C2** are ahead of curve (most 2026 systems send flat, no turn-velocity
  awareness).
The strongest deferred adds (intent classification + suppression; consolidation) are
captured as Phase D / Phase E stubs rather than expanding this worktree's blast radius.

## Constraints

- C11, `-Wall -Wextra -Wpedantic -Werror`, free every allocation, ASan-clean.
  Full `human_tests` suite (12,993+) passes: 0 failures, 0 ASan, 0 leaks.
- `HU_IS_TEST` guards on all side effects (no real network/spawn/hardware in tests);
  deterministic tests only.
- **Safe defaults**: every new behavior is byte-identical to today when its
  context is empty or its flag is off. AC-A1 (empty model), AC-B1 (no contact),
  AC-B2 (factor 0.0) each ship a pinned "no-change" test.
- Local model output still passes the outbound guard chain; cloud fallback intact.
- Security: deny-by-default unchanged; never log secrets; the AC-B1 fix is itself a
  privacy improvement (stops cross-contact context leak).
- Use `SQLITE_STATIC` never `SQLITE_TRANSIENT`. Per-language anti-patterns per CLAUDE.md.
- Per `tests-that-pin-bugs.md`: each change ships a positive contract test, not a
  test that locks the old (leaky/zeroed/decay-off) behavior.
- Test/source gate symmetry honored for any flag-gated source.

## Phasing & sequencing

| Phase | ACs | Why this order |
|---|---|---|
| **A** | AC-A1 | Initiative grounding — independent, own file (`init_proposer.c`). |
| **B** | AC-B1, AC-B2 | Memory quality — B1 (leak) is the single highest-value fix; B2 is independent tuning. |
| **C** | AC-C1, AC-C2 | iMessage timing/threading — both touch the daemon/follow_up send path. |

Phases are independent (distinct files) and run in isolated worktrees, merged when
the peer session is quiet. Within a phase the tasks are sequential.
