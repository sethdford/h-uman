# Session Handoff Pack — 2026-05-27

Bootstrap prompts for the four sub-projects from the 2026-05-26 + 2026-05-27
planning sessions. Each prompt is self-contained: paste into a fresh Claude
Code session and the session has everything it needs to start without
re-reading 100+ turns of brainstorming history.

UPDATE 2026-05-27: voice-via-Cartesia brainstorm was completed in the
same session as the original planning (after the user pushed past the
recommended stop-point); Prompt 4 is now an execute prompt rather than
a brainstorm prompt. Cross-channel synthesis (Prompt 3) remains a
brainstorm prompt — that one is still gated on reflection-loop landing.

## Why a handoff pack

The 2026-05-26 planning session shipped two complete specs (reflection
loop, calibrated uncertainty) but ran out of useful context budget before
executing the implementation or brainstorming the remaining sub-projects.
Rather than push the remaining work into a degraded context, we deferred
to fresh sessions per the discipline in `~/.claude/CLAUDE.md` ("Plan
before you build") and the practical reality that subagent-driven
execution of a 12-task plan needs supervision across days, not minutes.

Each prompt below is intended to be the FIRST message in a new chat
session. They include enough context that you don't need to explain the
project state.

---

## Prompt 1 — Execute reflection-loop (highest priority)

Use this in a fresh session when ready to ship the reflection loop.
Recommended pacing: one wave per day, with manual review of each wave's
commits before dispatching the next wave.

```
Execute the reflection-loop implementation plan at
docs/plans/2026-05-26-reflection-loop/tasks.md via the
superpowers:subagent-driven-development skill.

Current state:
- Spec at docs/plans/2026-05-26-reflection-loop/design.md (approved)
- Plan at docs/plans/2026-05-26-reflection-loop/tasks.md (12 tasks)
- Task 1 (header + schema + stable id) already landed at commit 7ed1d482
  on origin/main with 17 tests passing
- Tasks 2-12 are pending

Execute in four waves to respect cross-task dependencies:

WAVE 1 (parallel, independent foundations):
- Task 2: SQLite storage layer
- Task 3: Config plumbing

WAVE 2 (after Wave 1 lands):
- Task 4: Prompt template + input assembly
- Task 5: Reflection orchestration (tick + run)
- Task 6: Consumer queries

WAVE 3 (integrations, partially parallel):
- Task 7: System-prompt integration
- Task 8: init_proposer integration
- Task 9: Daemon wiring
- Task 11: Quorum predicate + CI gate

WAVE 4 (sequential, closes the sprint):
- Task 10: End-to-end test with mock provider
- Task 12: Operator health + acceptance verification

Constraints:
- Each task dispatched to a fresh subagent in its own git worktree off
  origin/main (per ~/.claude/rules/worktree-merge-before-cleanup.md and
  worktree-cwd-resets-in-bash.md)
- Critic review (the verifier agent + a code-reviewer agent) between
  every wave before moving forward
- DO NOT call TeamDelete or cleanup worktrees until all tasks for a
  wave are merged to main
- After each merge, run `bash scripts/check-test-source-gate-symmetry.sh`
  and `./build/human_tests` (full suite) before declaring the wave done
- Per `~/.claude/rules/agent-task-sizing.md` (N <= 8 mechanical sites or
  split) — Wave 2 + Wave 3 each have 4 tasks, acceptable

Acceptance criteria (from design.md):
AC-1: Reflection run within 24h of daemon startup with reflection.enabled=true
AC-2: Run produces >=3 typed patterns covering >=2 pattern types from >20 turns
AC-3: Re-run on same corpus produces >=80% pattern overlap by stable id
AC-4: Malformed model output -> status='schema_invalid', daemon keeps serving
AC-5: Query for system prompt returns <=5 channel-filtered patterns
AC-6: init_proposer can surface a pattern and retire on contradiction
AC-7: Non-SQLite build compiles and tick returns HU_OK with one-shot disabled-log

Sprint 2 (US-10..US-13: shadow mode + eval) is OUT OF SCOPE here; gets its
own plan when this one ships.

Start with Wave 1.
```

---

## Prompt 2 — Execute calibrated-uncertainty

Use this in a fresh session when ready to ship calibrated uncertainty.
Can run in parallel with reflection-loop execution because the modified
files don't overlap (uncertainty touches src/agent/uncertainty.c +
persona overlay; reflection touches src/reflection/ which is new). The
overlap point is the consumer integration (Task 9 of uncertainty wires
into src/reflection/consumer.c), so SEQUENCE that task after the
reflection-loop's consumer.c lands.

```
Execute the calibrated-uncertainty implementation plan at
docs/plans/2026-05-26-calibrated-uncertainty/tasks.md via the
superpowers:subagent-driven-development skill.

Current state:
- Spec at docs/plans/2026-05-26-calibrated-uncertainty/design.md (approved)
- Plan at docs/plans/2026-05-26-calibrated-uncertainty/tasks.md (10 tasks)
- src/agent/uncertainty.c ALREADY EXISTS with the base hu_uncertainty_*
  module; this plan extends it (does not replace)

CRITICAL ORDERING:
- Task 1 MUST be the first subagent dispatch. It pins pre-change
  production behavior bit-for-bit (AC-4 regression test). If any other
  task lands before Task 1, the regression contract is broken.
- After Task 1, Tasks 2-6 can fan out in two waves
- Tasks 7-9 wire into call sites and need src/reflection/consumer.c to
  exist (from reflection-loop sprint). If reflection-loop's Task 7 has
  not landed yet, defer this plan's Task 9 until it has.

Execute in waves:

WAVE 0 (sequential, MUST be first):
- Task 1: Lock existing behavior with regression test

WAVE 1 (parallel after Task 1):
- Task 2: Soft-blended score function
- Task 3: Verbalized confidence prompt addendum + parser
- Task 4: Persona overlay hedge phrase bank

WAVE 2 (after Wave 1):
- Task 5: Temporal decay detection + hedge variant
- Task 6: ECE-ready logging schema + log API

WAVE 3 (call site integrations — sequential because they share files):
- Task 7: Wire into agent_turn.c:5921 (existing call site)
- Task 8: Wire into init_proposer.c
- Task 9: Wire into reflection/consumer.c (BLOCKED on reflection-loop
  Task 7 landing — verify with: ls src/reflection/consumer.c)

WAVE 4 (closes the sprint):
- Task 10: Acceptance verification

Constraints same as Prompt 1 (worktrees off main, critic review between
waves, no TeamDelete until merged, gate symmetry check, full test suite).

Acceptance criteria from design.md:
AC-1: All four confidence levels return non-NULL hedge phrase
AC-2: Persona overlay overrides defaults when hedge_phrases field present
AC-3: With fact_count>=3, heuristic regex signals contribute 0% to score
AC-4: With fact_count==0, score equals pre-change value bit-for-bit (REGRESSION)
AC-5: VERY_LOW reflection patterns dropped by init_proposer
AC-6: Slice annotates MEDIUM patterns with " (likely)", LOW with " (uncertain)"
AC-7: 21+ new tests pass, 0 ASan errors, gate-symmetry check passes

Sprint 2 (ECE computation + per-channel calibration) is out of scope here.

Start with Wave 0 (Task 1, sequentially, MUST land before anything else).
```

---

## Prompt 3 — Brainstorm cross-channel synthesis (#2)

Use this AFTER reflection-loop sprint lands (this sub-project depends on
hu_reflection_pattern_t existing in production code). Brainstorm only —
the session should produce a spec, not implementation.

```
Brainstorm sub-project #2 (cross-channel synthesis) using the
superpowers:brainstorming skill.

Mission context (from CLAUDE.md M2 — Personal Model):
The agent has 31 messaging channels but treats each as isolated. When
something happens on iMessage, the Telegram agent should know. A human
friend doesn't work that way — they remember what you told them
yesterday regardless of which medium you used.

The architecture supports this trivially (hu_personal_model_t is global)
but the SYNTHESIS layer doesn't exist: when should one channel bring up
something that originated in another? When should it stay quiet?
That's the "more better than human" capability — h-uman knows what you
said on iMessage when you're texting on Telegram.

Pre-requisite state (verify before starting):
- ls docs/plans/2026-05-26-reflection-loop/results/  (acceptance results exist)
- grep -rn "hu_reflection_pattern_t" src/reflection/storage.c  (struct shipped)
- ls src/reflection/consumer.c  (slice consumer landed)

If any of these are missing, this brainstorm is blocked — reflection-loop
needs to land first.

Starting context to read:
- docs/plans/2026-05-26-reflection-loop/design.md (sibling spec — defines
  the pattern struct cross-channel will consume)
- ~/.claude/CLAUDE.md (M2 mission, persona overlay architecture)
- src/persona/overlay.c (Tier-1 channel overlays — formality/length differ)

Key design questions the brainstorm should resolve:
1. What event triggers cross-channel synthesis? (new fact extraction on
   channel A → check whether channel B should reference it? Periodic
   sweep? Per-turn enrichment?)
2. What's the "should I bring this up?" judge? (heuristic? small LLM
   call per turn? reuse init_proposer's governor?)
3. Privacy/UX surface: which channels CAN share context vs which are
   walled off (Slack work vs iMessage family)?
4. How does the persona overlay drive cross-channel speech? ("As you
   mentioned earlier on iMessage..." is one shape; "Hey, I noticed..."
   is another)
5. How do we detect when cross-channel context is unwelcome? (reaction-
   based feedback loop like reflection's retire-on-contradiction)

Out of scope:
- Voice integration (#4 — separate sub-project)
- Calibrated uncertainty wiring (already specced)
- New cross-channel storage tables (use existing hu_personal_model_t)

Produce a spec at docs/plans/<date>-cross-channel-synthesis/{design.md,
tasks.md} following the pattern of the two existing 2026-05-26 specs.
Apply brainstorming flow rigorously (one clarifying question at a time,
present design sections with approval, self-review before commit).

The terminal step per the brainstorming skill is to invoke
superpowers:writing-plans to generate the tasks.md.
```

---

## Prompt 4 — Execute voice via Cartesia

The brainstorm landed during the 2026-05-27 session — spec and tasks
already exist at `docs/plans/2026-05-27-voice-cartesia/`. This is now an
execute prompt, not a brainstorm prompt.

Independent of reflection-loop and calibrated-uncertainty execution
(touches different files). Has real prerequisites that must be met
before sprint kickoff.

```
Execute the voice-via-Cartesia implementation plan at
docs/plans/2026-05-27-voice-cartesia/tasks.md via the
superpowers:subagent-driven-development skill.

Current state:
- Spec at docs/plans/2026-05-27-voice-cartesia/design.md (approved)
- Plan at docs/plans/2026-05-27-voice-cartesia/tasks.md (10 tasks)
- Substantial existing infrastructure: src/tts/cartesia*.c (TTS +
  streaming + emotion_map + voice_clone), src/channels/twilio.c (SMS),
  src/channels/voice_channel.c (3 modes, none Cartesia yet),
  include/human/channels/voice_channel.h, src/voice.c (Whisper STT
  default + Cartesia STT optional), include/human/tts/cartesia.h

PREREQUISITES — VERIFY BEFORE STARTING:
- Twilio account_sid + auth_token available
- Twilio phone number purchased; webhook URL configurable
- Cartesia API key valid for TTS + STT
- Seth's reference audio recorded in Cartesia UI; voice_id UUID copied
- Public-reachable HTTPS endpoint configured (ngrok / Cloudflare Tunnel
  / Tailscale Funnel) — the daemon must be reachable from Twilio's cloud
- Confirmed model_router has a "voice" or "reflexive" tier passing
  thinkingConfig.thinkingBudget=0 (per CLAUDE.md Gemini 3.x gotcha).
  If not present, ADD it before starting Task 6 — biggest sleeper risk.
- Web-search verified: sonic-3-2026-01-12 is currently Cartesia's
  active model_id (or update to whatever is current at sprint start)

If any prerequisite fails, STOP and report. Do not start execution.

Execute in waves:

WAVE 1 (parallel, pure unit modules, no h-uman deps):
- Task 1: μ-law/PCM codec + resample
- Task 2: Silence-based VAD

WAVE 2 (parallel after Wave 1, independent):
- Task 3: Twilio Voice webhook (TwiML response)
- Task 4: WebSocket server foundation (RECON FIRST: check existing
  websocket/wss infra in src/ before deciding extend vs add new)

WAVE 3 (sequential, builds on Waves 1+2):
- Task 5: Per-call session state + TTS streaming integration
- Task 6: End-to-end pump loop (THE critical task — agent integration
  here is where Gemini 3.x thinking-budget gotcha bites)

WAVE 4 (parallel-safe, smaller surface):
- Task 7: Config plumbing for twilio_voice block
- Task 8: Wire CARTESIA mode + register voice_twilio channel + HTTP routes
- Task 9: voice_calls SQLite table + logging

WAVE 5 (closes the sprint):
- Task 10: Acceptance + manual smoke (REQUIRES real phone call against
  Twilio number — cannot be fully automated)

Constraints same as Prompts 1+2 (worktrees off main, critic review
between waves, no TeamDelete until merged, gate symmetry check, full
test suite). Additionally:
- Heap-allocate hu_voice_call_t per ~/.claude/rules/asan-pthread-stack-
  aliasing-darwin.md (cross-thread session state must not be stack-local)
- After Task 8, daemon will respond to /twilio/voice but won't have
  real callers — that's expected; Task 10's manual smoke is the only
  way to validate end-to-end

Acceptance criteria from design.md:
AC-1: Calling configured Twilio number rings -> h-uman answers within 2s
AC-2: Caller speaks -> STT transcription in agent input within 500ms of silence
AC-3: Agent responds -> first TTS byte to caller within 500ms p50 over 10 calls
AC-4: Cloned voice subjectively matches reference (listen test)
AC-5: Caller hangs up -> voice_calls row finalized with end_reason + turn_count
AC-6: With twilio_voice.enabled=false, /twilio/voice returns 404 + one-shot log
AC-7: All unit + integration tests pass with mock providers, 0 ASan, gate symmetry

Sprint 2 (Scope C: voice cloning CLI, per-overlay voice_ids, concurrent
calls, outbound, voicemail) is out of scope here; gets its own plan.

Start with Wave 1.
```

---

## Recommended execution order

| Order | Item | Why |
|---|---|---|
| 1 | Prompt 1 (reflection-loop execution) | Highest leverage — unblocks #2 cross-channel and is foundation for the "knows you" thesis |
| 2 | Prompt 2 (calibrated-uncertainty execution) | Can start in parallel with #1's later waves (file overlap only at consumer.c integration); locks in trust UX |
| 3 | Prompt 3 (cross-channel brainstorm) | Spec only; gated on reflection landing |
| 4 | Prompt 4 (voice execution) | Independent of #1/#2 (different files). Has external prerequisites — check before starting. |

## Notes for the supervising user (you)

1. **One prompt per session.** Resist the temptation to do multiple in
   one chat. Context budget is the bottleneck; fresh sessions are free.

2. **Review between waves, not between tasks.** Each subagent in a wave
   commits its own work; you check the full wave's commits before
   dispatching the next wave.

3. **Worktree hygiene.** Per
   `~/.claude/rules/worktree-merge-before-cleanup.md`, never call
   TeamDelete before merging. The 2026-04-04 incident lost an entire
   3-agent UX redesign because TeamDelete fired pre-merge.

4. **The verifier agent runs the code; the critic reviews it.** Both
   are needed. The verifier proves behavior (`/verify`); the critic
   finds half-fixes and edge cases. Different agents, different roles.

5. **If a wave fails its acceptance criteria, do NOT dispatch the next
   wave.** Fix the failure first. Half-finished waves accumulate
   regressions that compound.

6. **When this handoff pack is fully executed, all five
   "better than human" sub-projects from the 2026-05-26 planning will
   be either shipped or specced.** Update the M2 (Personal Model) and
   M4 (Ship to Users) mission status in CLAUDE.md to reflect that.
