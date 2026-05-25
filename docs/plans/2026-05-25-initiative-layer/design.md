# Initiative Layer — Design

**Status:** Draft (2026-05-25)
**Reads:** `requirements.md`, `audit-director-compression.md`, `audit-lora-training-judge.md`
**Feeds:** `tasks.md`

## What's already in place (don't rebuild)

This design composes existing primitives. The novel surface is small:

| Already exists | Where | Role |
|---|---|---|
| `hu_proactive_throttle` | `src/agent/proactive_throttle.c` (referenced in `src/daemon.c:1006`) | Per-contact recency cap, daily budget, quiet hours |
| `hu_proactive_budget_t` | `src/daemon.c:493`, `src/daemon.c:497` | `daily_max`, `cool_off_hours`, backoff on unanswered |
| `hu_proactive_context_t` | `src/agent/proactive.c`, `src/daemon_proactive.c` | Per-contact ring of recent moments, dedup state |
| `hu_proactive_check_events` | `src/agent/proactive.c:432`, called at `src/daemon.c:1577` | Inbound-event-driven proactive (different trigger shape) |
| Cron job framework | `src/main.c:1366`, `hu_cron_scheduler_t` | Schedules family check-ins at 10am |
| `hu_prompt_build_system` | `src/agent/prompt.c:109` | The 25+ field context assembler — initiative reuses this |
| F30/F31/F129 compute layers | `src/agent/proactive_ext.c` (per `2026-05-25-proactive-ext-completion.md`) | Curiosity / callback / disclosure detectors — INPUTS to initiative |
| Autoresponder quiet hours | `~/.human/autoresponder.json::schedules` | Already enforces 22:00-07:00 quiet |
| Model router with thinking budgets | `src/agent/model_router.c`, `src/agent/token_budget.c` | Picks reflexive/conversational/analytical/deep tiers per intent |

The initiative layer is the **glue** that orchestrates these primitives on a timed loop.

## Architectural shape

```
                          ┌────────────────────────────────────┐
                          │  CRON SCHEDULER (existing)         │
                          │  every N minutes during awake hrs  │
                          └─────────────────┬──────────────────┘
                                            │ tick
                                            ▼
                          ┌────────────────────────────────────┐
                          │  hu_init_proposer_tick             │
                          │  (new: src/agent/init_proposer.c)  │
                          └─────────────────┬──────────────────┘
                                            │
              ┌─────────────────────────────┴───────────────────┐
              │ 1. Governor pre-check (quiet, budget, recency) │
              │    → SKIP early if any check fails              │
              └─────────────────────────────┬───────────────────┘
                                            │
              ┌─────────────────────────────┴───────────────────┐
              │ 2. Assemble context bundle:                     │
              │    • All 25+ fields from prompt builder         │
              │    • F30: curiosity topics with pending hooks   │
              │    • F31: callback opportunities                │
              │    • F129: disclosures that haven't fired       │
              │    • Last N inbound/outbound from Seth          │
              │    • Today's calendar (if hu_awareness wired)   │
              └─────────────────────────────┬───────────────────┘
                                            │
              ┌─────────────────────────────┴───────────────────┐
              │ 3. LLM call (analytical tier):                  │
              │    "Given everything you know about Seth right  │
              │     now, is there something he'd appreciate     │
              │     hearing from you? Respond ONLY with a JSON  │
              │     object: {action: 'skip'|'propose',          │
              │              confidence: 0-1, message: '...',    │
              │              source_field: '...', reason: '...' }│
              └─────────────────────────────┬───────────────────┘
                                            │
              ┌─────────────────────────────┴───────────────────┐
              │ 4. Decision gate:                               │
              │    • action='skip' → log + done                 │
              │    • confidence < threshold → log + done        │
              │    • else → handoff to existing proactive       │
              │      throttle for final budget/recency check    │
              └─────────────────────────────┬───────────────────┘
                                            │
              ┌─────────────────────────────┴───────────────────┐
              │ 5. Send via existing iMessage channel path      │
              │    (same AX bridge, response_guard, etc.)       │
              └─────────────────────────────────────────────────┘
```

## Design decisions you should weigh in on

The spec marks these as DECISIONS to be made before implementation. Each has a default and a tradeoff. **Your taste matters more than mine here** — these shape the felt-experience of the product.

### DECISION-1: Cadence

How often does `hu_init_proposer_tick` fire during awake hours?

| Cadence | Pros | Cons |
|---|---|---|
| **Every 15 min** | Catches short windows of opportunity | High SKIP rate burns cycles; risk of spam |
| **Every 30 min (recommended default)** | Reasonable resolution, ~32 ticks/day | Could miss time-sensitive opportunities |
| **Every 60 min** | Cheap; matches "human friend would think about you a few times a day" | Low resolution; might feel sporadic |
| **Tied to circadian model** | Most "human" — h-uman thinks of you more in the morning/evening | Requires the circadian model to be reliable; more code |
| **Event-triggered (not periodic)** | No wasted ticks; fires when a F30/F31/F129 signal lands | Coupling to extractor latency; harder to reason about |

**Default in design:** Every 30 min during awake hours. Open to your input.

### DECISION-2: Confidence threshold and gate type

When the LLM proposes a message with confidence `c`, what do we do?

| Gate | Behavior | Best for |
|---|---|---|
| **Hard threshold (c >= 0.85)** | Below threshold = silent. | Aggressive spam-aversion. Fewer messages but each one is "deserved." |
| **Probabilistic gate (sample with p=c)** | Higher confidence = higher fire rate, but no cliff | Smoother experience; some lower-confidence proposals get through |
| **Tier-based** | Different thresholds for different proposal categories (curiosity 0.7, callback 0.85, disclosure 0.9) | Fine-grained control; more wiring |

**Default in design:** Hard threshold at 0.85. Tune down later only after Seth confirms the proposals are wanted. Per AC-7's reversibility, this can be tweaked without restart.

### DECISION-3: Model tier for the proposer LLM call

The "should I propose?" call is more expensive than a reactive reply because it needs richer reasoning over more context.

| Tier | Model | Cost per tick | Reasoning quality |
|---|---|---|---|
| Reflexive | `gemini-3.1-flash-lite-preview` | ~$0.0001 | Too shallow for this decision |
| Conversational | `gemini-3.5-flash` | ~$0.0003 | Probably enough |
| Analytical (recommended) | `gemini-3.1-pro-preview` | ~$0.003 | The decision deserves Pro-quality reasoning |
| Deep | `gemini-3.1-pro-preview` w/ thinking=4096 | ~$0.01 | Overkill except for high-stakes proposals |

**Default in design:** Analytical for the "propose-or-skip" decision. If propose, the actual message draft can use Conversational. ~$0.003 per non-SKIP tick × ~3 non-SKIP per day = ~$0.01/day.

### DECISION-4: How to populate awake-hours and circadian state

`autoresponder.json::schedules` has Seth's quiet hours (22:00-07:00). The initiative layer's awake window should respect this by default, but might want refinement:

| Approach | Behavior |
|---|---|
| **Just use autoresponder.json** | Awake = NOT in any autoresponder schedule. Simple, single source of truth. |
| **Add per-day overrides** | Sunday morning is different from Tuesday morning |
| **Tie to actual activity signal** | Use Mac wake/sleep state or recent keyboard activity |

**Default in design:** Just use autoresponder.json. Per-day overrides are easy to add later.

## Data structures

```c
// include/human/agent/init_proposer.h

typedef struct hu_init_proposer_config {
    bool enabled;                          // master kill switch (config.json::initiative.enabled)
    uint32_t tick_interval_sec;            // DECISION-1; default 1800
    double confidence_threshold;           // DECISION-2; default 0.85
    const char *propose_model;             // DECISION-3; default gemini-3.1-pro-preview
    const char *propose_thinking_budget;   // budget value for propose call
    uint32_t max_proposals_per_day;        // hard cap; default 6
} hu_init_proposer_config_t;

typedef enum {
    HU_INIT_PROPOSAL_SKIP = 0,
    HU_INIT_PROPOSAL_PROPOSE = 1,
    HU_INIT_PROPOSAL_ERROR = 2,
} hu_init_proposal_action_t;

typedef struct hu_init_proposal {
    hu_init_proposal_action_t action;
    double confidence;                     // 0.0 - 1.0
    char message[1024];                    // the proposed text to send
    char source_field[64];                 // which input drove the proposal (e.g. "f30_curiosity:bonsai")
    char reason[512];                      // LLM-generated rationale (logged, not sent)
} hu_init_proposal_t;

// Public API
hu_error_t hu_init_proposer_create(hu_allocator_t *alloc,
                                   hu_agent_t *agent,
                                   const hu_init_proposer_config_t *cfg,
                                   hu_init_proposer_t **out);
hu_error_t hu_init_proposer_tick(hu_init_proposer_t *proposer,
                                 int64_t budget_ms,
                                 hu_init_proposal_t *out_proposal);
void hu_init_proposer_deinit(hu_init_proposer_t *proposer);
```

## Tick-budget allocation (per AC-4)

A SKIP tick should be cheap. Time/cost breakdown:

| Phase | Time | Cost |
|---|---|---|
| Governor pre-checks (quiet, budget, recency) | <10ms | $0 |
| Context bundle assembly (reuse prompt builder) | <50ms | $0 (no LLM) |
| LLM propose-or-skip call (analytical tier) | ~500-2000ms | ~$0.003 |
| Decision gate | <10ms | $0 |
| Send via channel (if proposed) | ~3000ms (AX bridge) | ~$0 marginal |

Target SKIP tick: <100ms, $0 (skip happens entirely in governor pre-checks for most cases).

**Cost optimization:** the proposer can use a CHEAP reflexive-tier "is anything worth checking?" pre-call before invoking the analytical tier. If pre-call returns "nothing changed," skip the expensive call. This adds latency but cuts daily cost by 5-10x.

## Telemetry / observability (per AC-6)

Every tick logs:
```
[init_proposer] tick id=42 phase=governor_precheck quiet=false budget_remaining=4 recency_ok=true
[init_proposer] tick id=42 phase=context_assembly fields=28 f30=1 f31=2 f129=0 cal_events=1 last_inbound_min=147
[init_proposer] tick id=42 phase=propose model=gemini-3.1-pro-preview tokens_in=4231 tokens_out=187 ms=1450
[init_proposer] tick id=42 result=PROPOSE confidence=0.91 source=f31_callback reason="Seth mentioned the bonsai talk 6 days ago, hasn't followed up"
[init_proposer] tick id=42 phase=send channel=imessage contact=+18018285260 status=ok
```

Or for a skip:
```
[init_proposer] tick id=43 result=SKIP confidence=0.42 reason="No new signals since last tick"
```

Or for a governor-blocked tick:
```
[init_proposer] tick id=44 result=GATED reason="daily_budget_exhausted (6/6 used today)"
```

This matches the discipline in `~/.claude/rules/silent-config-gated-subsystems.md`.

## What CAN'T be designed yet

These wait for empirical data from Seth's actual usage:

- The right tick cadence (DECISION-1) — depends on how often signals actually arrive
- The right confidence threshold (DECISION-2) — depends on what fraction of proposals Seth ACTUALLY appreciates
- Whether to track "proposal acceptance rate" as feedback signal — slice 2 work after observing real outcomes for 1-2 weeks
- Multi-channel rollout — slice 3+

## What we are NOT changing

- Cron-based check-ins to Mindy/Betty/Annie (continue as-is)
- The existing `hu_proactive_check_events` inbound-driven path (it's a different trigger shape; initiative is periodic, that one is reactive)
- The system prompt builder (just call it from a new call-site)
- The iMessage channel transport
- Any other channel until v2
