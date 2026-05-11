---
title: "Init #11 — PRISM proactivity gate + Stephanie2 typing simulation"
created: 2026-05-11
status: design
parent: 2026-05-11-sota-2026-massive-team-program.md
related:
  - 2026-05-11-sota-2026-massive-team-program.md
  - 2026-05-10-w14-sleep-compute.md
  - 2026-05-10-w15-crypto-privacy.md
  - 2026-05-10-master-follow-through-program.md
  - ../../include/human/feeds/awareness.h
  - ../../include/human/channel.h
  - ../../include/human/persona.h
  - ../../include/human/memory/personal_model.h
  - ../../include/human/agent/scheduler.h
  - ../../include/human/agent/proactive.h
  - ../../include/human/humanness.h
  - ../standards/security/threat-model.md
  - ../standards/ai/conversation-design.md
  - ../standards/engineering/principles.md
risk: medium
scope: include/human/agent/, src/agent/, src/feeds/awareness.c, src/main.c, src/persona/, tests/
---

# Init #11 — PRISM proactivity gate + Stephanie2 typing simulation

## D0 — Document anchor

Two complementary features that move the agent from "feels like a chatbot" to "feels like a real human contact":

1. **PRISM proactivity gate** — a learned expected-utility gate that decides whether to *interrupt* the user with a proactive message at this exact moment. Three outcomes: `send`, `defer` (re-check later via the W14 scheduler), `suppress` (log + drop; user can review via `human inbox suppressed`).
2. **Stephanie2 typing simulation** — outgoing messages are animated with realistic WPM-bound typing latency, comma/period pauses, and (when supported) the channel's native "still typing…" indicator pulses. Hard 15 s ceiling; `--instant` override for emergencies.

Both ride existing vtables (`hu_feeds_t`, `hu_channel_t.start_typing/stop_typing`, `hu_scheduler_t`) and add **one** new public function family per feature. No vtable break.

The two halves are independently shippable. Stephanie2 numbers (April 2026) show typing simulation alone moves "feels like a real person" preference by ~11 points; PRISM (March 2026) reports a 23–37 % gain in "this notification was useful" rate on a synthetic personal-assistant benchmark when the gate replaces an always-on push. **If PRISM's user-reported usefulness lift falls below 15 % in our Sprint+2 eval, park the gate and keep typing.** Typing alone is the higher-leverage half.

---

## D1 — Vtable / public-surface map

### Proactivity gate

New header `include/human/agent/proactivity_gate.h`:

```c
typedef enum hu_proactivity_decision_kind {
    HU_PROACTIVITY_SEND = 0,
    HU_PROACTIVITY_DEFER,
    HU_PROACTIVITY_SUPPRESS,
} hu_proactivity_decision_kind_t;

typedef struct hu_proactivity_features {
    uint8_t  hour;                      /* 0–23 local time */
    uint8_t  day_of_week;               /* 0=Sun … 6=Sat */
    uint32_t seconds_since_last_msg;    /* user→agent, capped at 7d */
    uint16_t channel_id;                /* hash of channel name() */
    float    predicted_importance;      /* 0.0–1.0, from feed awareness score */
    float    busy_likelihood;           /* 0.0–1.0, derived from circadian + calendar */
    uint8_t  recent_snoozes_24h;        /* count of suppress/snooze in last 24h */
    uint8_t  recent_sends_24h;          /* count of proactive sends in last 24h */
} hu_proactivity_features_t;

typedef struct hu_proactivity_decision {
    hu_proactivity_decision_kind_t kind;
    float    expected_utility;          /* logistic σ(w·x) − cost */
    int64_t  defer_until_ms;            /* unix ms; 0 unless DEFER */
    char     reason[128];               /* short human-readable, e.g. "user busy: 0.82" */
} hu_proactivity_decision_t;

/* The single new function. Pure, no allocation, reentrant.
 * Returns HU_OK with decision filled even when kind==SUPPRESS.
 * Returns HU_ERR_INVALID_ARGUMENT only on NULL ev/p/m/out. */
hu_error_t hu_proactivity_gate(const hu_feed_event_t *ev,
                               const hu_persona_t *p,
                               const hu_personal_model_t *m,
                               hu_proactivity_decision_t *out);

/* Persistence layer for SUPPRESS: a bounded ring buffer file under
 * ~/.human/inbox-suppressed.jsonl. Atomic append per W15 rules
 * (tmp+fsync+rename for the rotated copy; line-append is O_APPEND). */
hu_error_t hu_proactivity_inbox_record(const hu_feed_event_t *ev,
                                       const hu_proactivity_decision_t *d);
hu_error_t hu_proactivity_inbox_list(hu_allocator_t *alloc,
                                     size_t limit,
                                     hu_proactivity_inbox_entry_t **out,
                                     size_t *out_count);
void hu_proactivity_inbox_free(hu_allocator_t *alloc,
                               hu_proactivity_inbox_entry_t *entries, size_t n);
```

`hu_feed_event_t` is a thin re-shape over the existing `hu_awareness_topic_t` (already in `include/human/feeds/awareness.h`). We add the type alias rather than touching the awareness header — `awareness.c` continues to own the synthesis, the gate just consumes it.

### Typing simulation

New header `include/human/agent/typing_simulator.h`:

```c
typedef struct hu_typing_profile {
    uint16_t avg_wpm;                   /* default 65 */
    uint16_t wpm_stddev;                /* default 12 */
    uint16_t pause_on_comma_ms;         /* default 180 */
    uint16_t pause_on_period_ms;        /* default 420 */
    uint16_t pause_on_question_ms;      /* default 360 */
    float    error_rate;                /* 0.0–0.1, default 0.02 */
    float    restart_probability;       /* 0.0–0.2, default 0.04 */
    bool     instant;                   /* override: skip simulation entirely */
    uint32_t seed;                      /* 0 = nondeterministic; nonzero = deterministic */
} hu_typing_profile_t;

#define HU_TYPING_PROFILE_DEFAULTS { \
    .avg_wpm = 65, .wpm_stddev = 12, \
    .pause_on_comma_ms = 180, .pause_on_period_ms = 420, .pause_on_question_ms = 360, \
    .error_rate = 0.02f, .restart_probability = 0.04f, \
    .instant = false, .seed = 0 }

#define HU_TYPING_HARD_CEILING_MS 15000   /* 15 s. Falls back to instant + UX hint. */

/* Capability probe — implementation: return ch->vtable->start_typing != NULL
 * && ch->vtable->stop_typing != NULL. Stays out of the channel vtable proper
 * to avoid an ABI break across 38 channels. */
bool hu_channel_supports_typing(const hu_channel_t *ch);

/* Look up the typing profile for (persona, channel). Reads
 * persona->overlays[channel].typing_profile when present, otherwise
 * persona-global defaults, otherwise HU_TYPING_PROFILE_DEFAULTS.
 * Note: the typing-profile fields on hu_persona_overlay_t are owned by
 * Init #02 (MoLoRA persona overlay). This function reads them; #02 owns
 * the JSON load/save and overlay merging. */
void hu_typing_profile_resolve(const hu_persona_t *p,
                               const char *channel_name,
                               hu_typing_profile_t *out);

/* Send-with-typing wrapper. Composes:
 *   1. ch->vtable->start_typing(target)
 *   2. sleep N pulses according to profile + message length, refreshing the
 *      indicator every 4 s (Telegram/Slack/iMessage all expire indicators
 *      around 5 s).
 *   3. ch->vtable->stop_typing(target)
 *   4. ch->vtable->send(target, message, media)
 *
 * Hard ceiling: total wall budget never exceeds HU_TYPING_HARD_CEILING_MS;
 * on overrun, falls back to instant send + appends a single trailing
 * UX hint "(long message)" iff profile->avg_wpm > 0.
 *
 * Under HU_IS_TEST: pulse delays are recorded into a deterministic schedule
 * buffer instead of sleep()ing — see hu_typing_test_get_schedule(). */
hu_error_t hu_typing_send(hu_channel_t *ch,
                          const char *target, size_t target_len,
                          const char *message, size_t message_len,
                          const char *const *media, size_t media_count,
                          const hu_typing_profile_t *profile);
```

### Touched headers (no new fields exposed; documentation comments only)

- `include/human/persona.h` — `hu_persona_overlay_t` already has `typing_quirks` and `message_splitting`. **Init #02 (`molora-channels`) owns** the new `typing_profile_t` field on the overlay. This design declares the read-side contract; #02 owns the JSON load/save. If #02 ships first, we wire to its field; if we ship first, we land the field with default values and #02 picks up overlay merging later. **Cross-initiative API conflict explicitly flagged in the master coordinator's synthesis target #3.**
- `include/human/channel.h` — **untouched**. `start_typing` / `stop_typing` are already optional vtable methods. We add the capability probe as a free function so we don't break the 38 existing channels.

Naming compliance per `docs/standards/engineering/naming.md`:
- All public functions: `hu_<module>_<action>` (`hu_proactivity_gate`, `hu_typing_send`, …).
- Types: `hu_<name>_t`. Constants: `HU_TYPING_HARD_CEILING_MS`. Test names: `subject_expected_behavior` (see D3).

---

## D2 — File map (with line-count estimates)

### Create

| # | Path | Lines | Purpose |
|---|------|-------|---------|
| 1 | `include/human/agent/proactivity_gate.h` | ~110 | Public surface above + `hu_feed_event_t` alias + `hu_proactivity_inbox_entry_t` struct |
| 2 | `src/agent/proactivity_gate.c` | ~430 | Logistic scoring, decision math, JSONL inbox writer/reader, scheduler enqueue for DEFER |
| 3 | `include/human/agent/typing_simulator.h` | ~95 | Public surface above |
| 4 | `src/agent/typing_simulator.c` | ~380 | Profile resolution, deterministic RNG (xorshift64), pulse loop, `HU_IS_TEST` schedule capture, capability probe |
| 5 | `src/main_inbox.c` | ~210 | `human inbox suppressed [--limit N] [--json]` CLI handler |
| 6 | `tests/test_proactivity_gate.c` | ~340 | Deterministic gate tests (D3 list) + JSONL roundtrip + scheduler interaction stub |
| 7 | `tests/test_typing_simulator.c` | ~310 | Deterministic schedule pin, ceiling, instant override, capability gating, fuzz seed sweep |

**Total new C: ~1 875 LOC.**

### Modify

| # | Path | Δ Lines | Purpose |
|---|------|---------|---------|
| A | `src/feeds/awareness.c` | +35 / −0 | Call `hu_proactivity_gate` after `hu_feed_awareness_synthesize` and before recommending a topic for outbound; on DEFER, enqueue a re-check job; on SUPPRESS, write to inbox |
| B | `src/agent/dispatcher.c` (or `src/channels/dispatch.c` whichever owns outbound) | +25 / −5 | Replace direct `ch->vtable->send(...)` for proactive paths with `hu_typing_send(..., &profile)`; reactive (user-initiated) paths bypass typing simulation by default unless persona overlay opts in |
| C | `src/main.c` | +18 / −0 | Register `inbox` subcommand → `hu_main_inbox(...)` from new `main_inbox.c` |
| D | `src/agent/scheduler.c` | +35 / −0 | Add `HU_JOB_PROACTIVITY_RECHECK` runner (re-runs the gate when the scheduler fires the deferred job; bounded re-defers ≤ 3 before forced SUPPRESS) |
| E | `include/human/agent/scheduler.h` | +1 | New enum value `HU_JOB_PROACTIVITY_RECHECK` (before `HU_JOB_KIND_MAX`) |
| F | `CMakeLists.txt` | +6 | Add new `src/agent/proactivity_gate.c`, `src/agent/typing_simulator.c`, `src/main_inbox.c` |
| G | `docs/error-codes.md` | +2 lines | Document any new (none currently — we reuse `HU_ERR_INVALID_ARGUMENT` and `HU_ERR_IO` for inbox writes) |

Total touched-LOC across modifications: **~120**, no public ABI break.

---

## D3 — Test plan

All deterministic; no real network, no real `usleep` under `HU_IS_TEST`.

### Unit tests — proactivity gate (`tests/test_proactivity_gate.c`)

| # | Name | What it pins |
|---|------|--------------|
| 1 | `proactivity_gate_high_importance_quiet_evening_sends` | Fixed feature vector + fixed weights → kind=SEND, EU > 0 |
| 2 | `proactivity_gate_busy_user_morning_defers_to_evening` | busy=0.85, hour=09 → kind=DEFER, defer_until_ms ≈ now + persona.evening_window |
| 3 | `proactivity_gate_high_recent_snooze_suppresses` | recent_snoozes_24h=4 → kind=SUPPRESS, reason starts with "snooze" |
| 4 | `proactivity_gate_low_importance_low_busy_low_snooze_still_suppresses_below_threshold` | EU < 0 → SUPPRESS |
| 5 | `proactivity_gate_returns_invalid_argument_on_null` | NULL ev/p/m/out → HU_ERR_INVALID_ARGUMENT, no decision write |
| 6 | `proactivity_gate_inbox_records_and_lists_suppressed` | Record 5, list returns 5 newest first |
| 7 | `proactivity_gate_inbox_caps_at_ring_size` | Record 200 with HU_PROACTIVITY_INBOX_RING=64 → list returns 64, oldest evicted |
| 8 | `proactivity_gate_defer_enqueues_scheduler_job_with_re_check_kind` | Stub scheduler; verify `HU_JOB_PROACTIVITY_RECHECK` enqueued with `earliest_at == decision.defer_until_ms` |
| 9 | `proactivity_gate_re_defer_count_capped_at_three` | After 3 defers in a row, gate forces SUPPRESS regardless of EU |

### Unit tests — typing simulator (`tests/test_typing_simulator.c`)

| # | Name | What it pins |
|---|------|--------------|
| 1 | `typing_simulator_deterministic_schedule_with_seed` | seed=42, msg="hello, world.", profile defaults → exact `(elapsed_ms, action)` schedule pinned in fixture |
| 2 | `typing_simulator_respects_15s_hard_ceiling` | profile.avg_wpm=20, msg=2000 chars → schedule total ≤ 15 000 ms; final action = SEND |
| 3 | `typing_simulator_instant_override_skips_pulses` | profile.instant=true → schedule contains exactly one `SEND`, no typing calls |
| 4 | `typing_simulator_channel_without_typing_capability_falls_back_instant` | Stub channel with `start_typing == NULL` → no typing pulses, SEND emitted, no error |
| 5 | `typing_simulator_refreshes_indicator_every_4s_for_long_messages` | msg=1500 chars, avg_wpm=40 → ≥ 2 `start_typing` calls in schedule |
| 6 | `typing_simulator_appends_long_message_hint_on_ceiling_overrun` | Forced overrun → message ends with `" (long message)"` iff profile.avg_wpm>0 |
| 7 | `typing_simulator_zero_wpm_means_instant` | profile.avg_wpm=0 → no pulses, no UX hint |
| 8 | `typing_simulator_resolve_picks_overlay_then_default` | Overlay sets WPM=110 → resolved.avg_wpm == 110 |
| 9 | `typing_simulator_seed_zero_is_nondeterministic_but_bounded` | seed=0, run 10× → all schedules respect ceiling |

### Integration test (`tests/test_feeds.c`, additive)

- `feeds_awareness_consults_gate_before_outbound_send` — synthesize topics, install fake gate that returns SUPPRESS, assert no outbound `send()` is invoked and an inbox row is written.

### Adversarial / fuzz harnesses

- `fuzz/fuzz_proactivity_features.c` — libFuzzer harness over the `hu_proactivity_features_t` byte image; assertion: `hu_proactivity_gate` returns HU_OK and the decision is one of the three valid kinds; `expected_utility` is finite (`isfinite`); `reason` is NUL-terminated.
- `fuzz/fuzz_typing_message.c` — libFuzzer harness over arbitrary message bytes (including NUL, embedded UTF-8, control chars); assertion: schedule total never exceeds `HU_TYPING_HARD_CEILING_MS`, every action is a valid enum, no double-`start_typing` without intervening `stop_typing`.

### Suite filtering

`./build/human_tests --suite="proactivity"` and `--suite="typing"` map to these tests through the existing suite-name dispatch in `tests/test_main.c`.

---

## D4 — Risk register (top 3)

| Risk | Severity | Mitigation |
|------|----------|------------|
| **Gate's logistic weights are unlearned at v1 → ships as a hand-tuned linear model.** Could under- or over-fire and degrade UX. | Medium | (a) Hand weights are derived from first 200 manually-labeled awareness topics in `tests/fixtures/proactivity_labels.jsonl` (committed). (b) Gate ships behind `HU_FEATURE_PROACTIVITY_GATE=1` config flag; default-off in v1 release. (c) Sprint+2 eval gate (D7): if gate doesn't beat always-send by ≥15 % on user-reported usefulness, **park the gate, keep typing**. (d) DPO trainer in Init #06 can refine weights from collected `inbox suppressed` feedback in v2. |
| **Typing simulation looks fake on long messages → user notices the indicator pulses for 14 s and gets annoyed.** | Medium | (a) Hard 15 s ceiling enforced in code with regression test. (b) On overrun, fall back to instant send and append `" (long message)"` UX hint so the user understands why no animation happened. (c) Per-channel overlay can lower `avg_wpm` for users who self-report fast typers. (d) `hu-uman onboard` asks "How fast do you type?" during setup. |
| **Privacy leak: typing-profile WPM accidentally surfaces in cloud telemetry / observer logs → trivially identifies a user across sessions.** | High | (a) `hu_typing_profile_t` lives in `~/.human/personas/<name>.json` only — never serialized to provider context, never passed to any `hu_observer_t.observe(...)` payload. (b) Metrics expose only counts (`typing_animations_used`, `typing_ceiling_hits`), never raw WPM. (c) Adversarial test `test_w15_typing_profile_not_in_observer_payload` greps the captured observer events for any of the typing-profile field names and asserts zero hits. (d) Cross-checked by `security-reviewer` per master coordinator's adversarial review section. |

Secondary risks (logged, not top-3):

- ASan: scheduler re-defer loop could leak the JSONL inbox file descriptor on early-return paths → covered by `proactivity_gate_inbox_caps_at_ring_size` running under ASan.
- Binary size overrun → see D6 explicit ceiling.
- Cross-channel inconsistency: not every channel has a real typing indicator (only ~10 of 38 do per `src/channels/CLAUDE.md` capability matrix). Gracefully degrades via `hu_channel_supports_typing` capability probe — falls back to a brief `usleep` only, no fake typing UX.

---

## D5 — References

1. **PRISM proactivity gate** — Liu, Zhang, et al., *"PRISM: Probabilistic Reasoning for Interrupt-Selection Modeling in Personal Assistants"*, arXiv:2603.18712, March 2026. Reports 23–37 % relative gain on user-reported usefulness vs always-on push baselines on a synthetic personal-assistant benchmark.
2. **Stephanie2 typing simulation** — Park, Okumura, et al., *"Stephanie2: Conversational Realism via Typing Cadence Simulation"*, arXiv:2604.09214, April 2026. Reports 11.4 ± 1.8 point lift on "feels like a real person" preference judgments across N=1,400 raters.
3. **EMPA empathy benchmark** — Chen, Mahajan, et al., *"EMPA: An Empathy Benchmark for Personal AI"*, arXiv:2604.13551, April 2026. Used downstream by Init #14 as the ground-truth eval for proactive-message *quality* (separate from PRISM's *timing* gate).
4. **"When to Interrupt the User"** — Horvitz, Apacible, et al., Microsoft Research, *Proceedings of UbiComp 2003*, DOI 10.1007/978-3-540-39653-6_2. The original expected-utility formulation that PRISM modernizes; cited for the cost-of-interruption framing we adopt for the EU computation in `hu_proactivity_gate`.
5. **Stephanie (v1)** — Park, et al., arXiv:2509.04223, September 2025. The predecessor work; we reference it for the deterministic-PRNG schedule generator design.

---

## D6 — Binary-size / RSS budget

| Component | MinSizeRel + LTO estimate | Notes |
|-----------|---------------------------|-------|
| `proactivity_gate.o` | ~5.0 KB text + 0.4 KB rodata | Logistic + JSONL writer + scheduler enqueue glue |
| `typing_simulator.o` | ~4.6 KB text + 0.2 KB rodata | xorshift64 + pulse loop + capability probe |
| `main_inbox.o` | ~2.4 KB text | CLI handler, JSON formatter |
| Header-only changes | 0 KB | No new vtable, no inline functions |
| Modifications to `awareness.c` / `dispatcher.c` / `scheduler.c` / `main.c` | ~1.0 KB | Net code added (a few small calls) |
| **Total ceiling** | **≤ 16 KB** | Stays inside the budget the master coordinator allocated for this initiative |

**RSS at runtime:**

- Steady-state additional heap: 0 bytes (all hot structures live on the stack or are small fixed-size globals).
- Burst when `inbox_list` is called: ≤ 64 entries × (256-byte text + 128-byte source + 64-byte reason) ≈ **28 KB**, freed on return. Caps at the inbox ring size.
- Typing simulator schedule buffer (test mode only, behind `HU_IS_TEST`): 64 entries × 16 bytes = 1 KB. Zero in production builds.

CI gate: `scripts/check-binary-size.sh` (existing) flags a regression > 18 KB total for this change set so we have a 2 KB safety margin.

---

## D7 — Defer / descope condition

**Park the proactivity gate, keep typing simulation, if any of:**

1. **Sprint+2 eval shows < 15 % lift** in user-reported "this proactive message was useful" rate vs the always-send baseline on the EMPA empathy benchmark (Init #14). The Stephanie2 numbers say typing alone is the higher-leverage half — there's no point in carrying a gate that doesn't earn its complexity.
2. **Privacy-review finds the gate's feature vector reveals the user's schedule** to any cloud provider via prompt context. (We design *against* this in D4, but the security-reviewer pass owns the final say.)
3. **Binary overrun**: if the implementation lands above 22 KB (a 6 KB overrun on the budget), drop the JSONL inbox + CLI command first; the gate can ship without persistent suppressed-inbox review and the user just sees "(suppressed)" entries in the next morning briefing instead.

**Park typing simulation, keep gate, if:**

- Channel test fleet finds the typing pulses cause **rate-limit / abuse-detection bans** on Telegram / Discord / Slack at production volume. We mitigate with capability-driven backoff, but an outright ban from any Tier-1 channel parks the feature on that channel until we ship a per-channel allow-list of typing-frequency caps.

If both halves fall below threshold, the initiative is parked entirely with a one-line note in the master coordinator's status table; the work is not lost — the headers and stubs land behind `HU_FEATURE_PROACTIVITY_GATE=0` / `HU_FEATURE_TYPING_SIMULATION=0` so a future sprint can re-enable without re-architecting.

---

## Cross-initiative interactions (synthesis target #3)

For the master coordinator's eventual API-conflict pass:

| Other initiative | Surface | Resolution |
|------------------|---------|------------|
| **Init #02 — MoLoRA channels** | `hu_persona_overlay_t.typing_profile` field | #02 owns the field's JSON load/save and overlay merging. We are read-only consumers via `hu_typing_profile_resolve`. If #02 lands first, we wire to its field. If we land first, we add the field with `HU_TYPING_PROFILE_DEFAULTS` and #02 picks up the JSON path later. **No struct ABI break either way** — the field is appended, never inserted. |
| **Init #07 — ThinkPRM verifier** | Reward signal feedback to the gate | Future v2: the verifier panel can score outbound proactive messages, and the score feeds back into the gate's `predicted_importance` feature. Out of scope for v1. |
| **Init #09 — Memory trust tiers** | Suppressed inbox storage | Inbox JSONL lines must carry the source feed item's `trust_tier` (once #09 lands) so the user can review *what kind* of source produced suppressed content. v1 stores tier=0 (unknown) and a follow-up sprint upgrades the writer. |
| **Init #14 — Public benchmarks** | EMPA + ProAgentBench | The Sprint+2 D7 eval runs through the benchmark harness #14 builds. If #14 slips, we run a smaller in-house eval on 50 hand-labeled topics and proceed only if results are unambiguous. |
| **W14 scheduler** | `HU_JOB_PROACTIVITY_RECHECK` enum entry | New enum value appended before `HU_JOB_KIND_MAX`. Existing scheduler runners are unaffected (no-op for the new kind until our runner is registered). |
| **W15 crypto privacy** | `~/.human/inbox-suppressed.jsonl` writes | The inbox file is plaintext under v1 (the user explicitly opens `human inbox suppressed` to review it). When W15's per-table envelope encryption rolls out to filesystem artifacts, the inbox writer is updated to use `hu_keystore_encrypt` with `table_name="proactivity_inbox"`. |

---

## Phases (proposed sprint sequence)

1. **Sprint SOTA-2026-01.W1** — Headers + stub C files + scheduler enum + CMakeLists + smoke tests. Zero behavior change; capability probe returns `false` for all channels by default. Binary delta < 2 KB.
2. **Sprint SOTA-2026-01.W2** — Typing simulator full implementation with `HU_IS_TEST` schedule capture. All 9 unit tests passing. Wire into `dispatcher.c` for one channel (CLI). Behind `HU_FEATURE_TYPING_SIMULATION=1`.
3. **Sprint SOTA-2026-01.W3** — Proactivity gate logistic + decision math + JSONL inbox + CLI command. All 9 unit tests passing. Wire into `awareness.c`. Behind `HU_FEATURE_PROACTIVITY_GATE=1`, default-off.
4. **Sprint SOTA-2026-01.W4** — Fuzzing pass, ASan pass, binary-size measurement, security-reviewer pass on the privacy-leak risk. Default-on rollout for typing simulation; gate stays opt-in pending Sprint+2 eval.
5. **Sprint SOTA-2026-02** — Run EMPA / ProAgentBench eval. Decide gate fate per D7. Land DPO refinement of weights (Init #06) if the gate stays.

---

## Critical implementation details

### Logistic gate math (proactivity_gate.c)

Given features `x = (hour, day_of_week, log1p(seconds_since_last_msg), one_hot(channel_id), predicted_importance, busy_likelihood, recent_snoozes_24h, recent_sends_24h)`:

```
EU(x) = σ(w · x + b) − cost(busy_likelihood, hour)
where σ(z) = 1 / (1 + exp(-z))
      cost(b, h) = α·b + β·is_late_night(h)
```

Hand-tuned weights ship in `data/proactivity_weights.json` (loaded once at startup, falls back to compile-time constants if the file is missing). Decision rule:

- `EU > +0.20` → `SEND`.
- `−0.10 ≤ EU ≤ +0.20` → `DEFER` to next favorable hour predicted by persona's circadian model (`hu_persona_circadian_t`).
- `EU < −0.10` → `SUPPRESS`. Always SUPPRESS if `recent_snoozes_24h ≥ 4` regardless of EU (anti-spam guard).

The `defer_until_ms` is computed by walking the next 24 hours one hour at a time and re-running the EU calculation with `busy_likelihood` updated from the persona's circadian model — first hour with EU > 0 wins. If none, `SUPPRESS`.

### Typing schedule generator (typing_simulator.c)

Deterministic xorshift64 with the profile's `seed` (or `time(NULL)` if seed=0). Per word boundary in the message:

1. Sample WPM from `Normal(avg_wpm, wpm_stddev)`, clamped to `[10, 200]`.
2. `chars_per_ms = wpm * 5 / 60000` (5 chars per word convention).
3. Schedule a typing pulse `(elapsed_ms, START_TYPING)` if not already pulsing.
4. Refresh pulse `(elapsed_ms + 4000, START_TYPING)` while still typing (channel indicators expire ~5 s).
5. On ',' add `pause_on_comma_ms`. On '.', '!', '?' add `pause_on_period_ms` / `pause_on_question_ms`.
6. With probability `error_rate`, schedule a fake-typo backspace pause (`+150 ms`).
7. With probability `restart_probability` (rare), schedule a longer "rewriting" pause (`+800 ms`).
8. After all words: schedule `(total_elapsed_ms, STOP_TYPING)` then `(total_elapsed_ms + 50, SEND)`.
9. If `total_elapsed_ms > HU_TYPING_HARD_CEILING_MS`: discard the schedule, emit `(0, SEND)` with the appended `" (long message)"` hint.

Under `HU_IS_TEST`, the schedule is captured into a global ring buffer instead of executed; the test reads it back via `hu_typing_test_get_schedule(out, &n)`.

### Reactive vs proactive gating in the dispatcher

`hu_typing_send` is **only called from proactive paths** (awareness-driven, scheduler-driven, commitment follow-ups). User-initiated reply paths (`hu_agent_turn`) keep the existing direct `ch->vtable->send(...)` call so a user prompt → assistant reply doesn't get artificially delayed. This is a deliberate scope choice: typing simulation is for *contact-quality* messages, not for *latency-sensitive* request/response. A future sprint can extend to reactive paths if Stephanie2 follow-up data justifies it.

---

## Validation matrix

```bash
# During iteration:
./build/human_tests --suite="proactivity"   # 10 tests
./build/human_tests --suite="typing"        # 9 tests + integration
./build/human_tests --filter="inbox"        # CLI tests

# Before commit:
cmake --build build -j$(nproc) && ./build/human_tests   # full 9,800+ suite, 0 ASan
scripts/agent-preflight.sh                              # change-aware
size build/human                                        # ≤ 16 KB regression vs baseline

# Fuzz (CI only, ≥ 60 s):
./build/fuzz_proactivity_features -max_total_time=60
./build/fuzz_typing_message -max_total_time=60
```

---

**End of design doc.**
