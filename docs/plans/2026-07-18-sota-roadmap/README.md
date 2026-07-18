---
title: SOTA Roadmap — 20 areas, each to state of the art
status: active
created: 2026-07-18
last_audit: 2026-07-18
owner: all contexts
related:
  - docs/plans/2026-07-12-egress-single-funnel/README.md
  - docs/plans/2026-07-13-presence-somebody-home/README.md
  - docs/plans/2026-05-29-humanness-north-star-metric/
  - docs/plans/2026-05-29-seth-aliveness/README.md
  - docs/research/2026-07-11-prompt-composition-shrink-plan.md
  - docs/research/2026-07-11-sesame-csm1b-competitive-brief.md
---

# SOTA Roadmap — 20 areas

> Written the day the 2026-07-18 iMessage quality audit found the three
> biggest gaps were not model quality at all: template subsystems bypassing
> the LLM, a quantization-mismatched adapter, and a learning flywheel whose
> recorder was compiled out of every production binary. The lesson repeats
> across h-uman's history: **SOTA here is mostly activation + measurement
> over an already-rich substrate, not new ML.**

## What "better than Gemini" means here (the honest benchmark)

h-uman does not compete with Gemini at being an assistant. The claim is the
one no frontier assistant can make: **indistinguishable-as-Seth over a real
message history, on your hardware, improving from your own reactions.**
A frontier assistant is structurally an *other* — stateless between
providers, personless, cloud-bound. The benchmark that operationalizes
"sentient-level" without pretending literal sentience:

- **Blind A/B indistinguishability**: human raters (and Gemini-as-judge)
  fail to beat chance separating h-uman replies from real Seth replies on
  held-out threads. Target: judge accuracy ≤ 55% (chance + noise).
- **Humanness composite** (`scripts/humanness_compose.py`, 4 axes) ≥ 0.85
  nightly with per-axis no-regression, vs the 0.763 bootstrap floor.
- **Zero leak-class sends** per week (G-family reject counts stay 0 in the
  wild, not just in tests).
- **Presence**: unprompted sends that recipients *reply to* at a rate
  comparable to Seth's own initiations (engagement, not volume).

Every area below carries: **Now** (evidence) → **SOTA** (target) → **Plan**
→ **Gate** (the measurement that flips it on, per
`.claude/rules/feature-gate-requires-measurement.md`).

---

## Foundation — kill every remaining negative (1–5)

### 1. Egress single-funnel
- **Now**: three uncoordinated outbound pipelines; reactive hot path has an
  inline humanness block under `#ifndef HU_IS_TEST` the suite cannot see.
  2026-07-18: registration hoisted to the router
  (`hu_daemon_register_reply_for_reactions`) — first funnel unification win.
- **SOTA**: exactly one governed pipeline (`hu_outbound_pipeline_run`) for
  reactive / proactive / burst / choreography; zero send-mutating code
  excluded from tests.
- **Plan**: execute `docs/plans/2026-07-12-egress-single-funnel/README.md`
  phases (de-blind the humanness block first, then route reactive through
  the pipeline). Delete the `#ifndef HU_IS_TEST` exclusion.
- **Gate**: suite exercises the reactive shaping path; egress audit script
  counts 1 funnel; one week of sends with zero divergence between paths.

### 2. Guard family (G1–G10+)
- **Now**: D1–D8 shape denylists (D8 assistant-service phrasing landed
  2026-07-18); static PII-free patterns; guard-sentinel deploy gate.
- **SOTA**: dynamic guards — reject any response quoting the loaded
  persona's own biographical fields verbatim (generalizes without baking
  PII into source); per-contact leak budget alerting (any REJECT in prod
  pages the operator); adversarial red-team suite generated from incident
  history.
- **Plan**: implement the dynamic persona-quote validator (design note
  already in `response_guard.c` G4 comment); nightly red-team eval replaying
  every historical leak + mutations; wire reject-count telemetry into
  `human doctor`.
- **Gate**: red-team suite 100% blocked; 30 days zero wild leaks.

### 3. Template subsystems → LLM-composed
- **Now**: `contextual_proactive` and follow-up bumps were splicing raw
  quotes into static templates ("how'd the It will be tomorrow. Im working
  go?", "hey, just bumping this" ×5/day). 2026-07-18: topic-quality
  predicate + per-contact 48h cooldown landed — garbage is blocked, but
  survivors are still static template text.
- **SOTA**: no deterministic English ever reaches a contact. Template
  paths become *directives* to the one funnel ("follow up about the
  interview") and the LLM composes in-voice, persona- and
  relationship-aware.
- **Plan**: convert `hu_contextual_proactive_build_message` output +
  `hu_followup_template_for_warmth` into director directives; reuse the
  existing proactive LLM check-in path (already does this for temporal
  follow-ups); ship OFF→SHADOW→LIVE.
- **Gate**: blind A/B on composed vs template follow-ups; shadow log shows
  0 malformed compositions over 2 weeks.

### 4. Style fidelity (measured card → enforcement)
- **Now**: governor LIVE as of 2026-07-18 (was shadow) for terminal-punct +
  reciprocal-question axes; measured card v1 (79% no terminal punct, 9%
  ?-endings, 4% lowercase) vs model (10%/31%/23%).
- **SOTA**: per-register, per-contact style cards (casual/substantive ×
  contact) re-measured monthly from Seth's live corpus; governor enforces
  all measured axes (casing, length distribution, emoji rate, burstiness);
  drift alarm when model-vs-card divergence grows.
- **Plan**: extend `scripts/` style measurement to per-contact cards stored
  in `~/.human/style_fingerprints`; add casing + length axes to the
  governor; nightly card-vs-output divergence report.
- **Gate**: output stats within ±5pp of measured card on every axis for a
  week.

### 5. Latency + prompt composition
- **Now**: 16KB prompt tail-amputation caused a leak incident; shrink plan
  exists (`docs/research/2026-07-11-prompt-composition-shrink-plan.md`);
  prompt_trim in shadow.
- **SOTA**: p50 reply latency under typing-plausible time (human-like
  8–40s including choreography); prompt budget enforced by priority-aware
  trim (never amputate rules/persona); prompt cache hit where the runtime
  allows.
- **Plan**: execute the shrink plan; flip `HU_PROMPT_TRIM` shadow→live
  after soak; measure per-section token contribution nightly
  (prompt_budget.snapshot.json already exists).
- **Gate**: zero truncation-class incidents; p50/p95 latency dashboard
  green 2 weeks.

## The voice — sound like Seth (6–10)

### 6. Persona adapter line
- **Now**: v4-repair served on a mismatched base for ~7 weeks (4bit-trained
  on 8bit base). v5-8bit trained 2026-07-18 (same recipe, correct base,
  scale 2.0 explicit) — pending 3-category validation + hot-swap.
- **SOTA**: versioned adapter lineage (`adapter_lineage.jsonl`) where every
  entry records base, scale, data hash, and eval scores; base and adapter
  can never diverge (doctor check compares serving base vs
  `adapter_config.json` model).
- **Plan**: land v5; add `check_adapter_base_match` doctor check; require
  fidelity eval (`scripts/eval_fidelity_nightly.py`) before any swap.
- **Gate**: fidelity ≥ v4's +27pp lift on the *matching* base; base-mismatch
  doctor check green forever.

### 7. DPO flywheel (collection)
- **Now**: restored 2026-07-18 — RL_FULL now in dev preset (recorder was
  compiled out of every deploy), router registration covers all reply
  routes. Historical: 3 of 5 sources dead since mid-May; tapback source 0
  pairs ever despite 119 tapbacks/30d.
- **SOTA**: all five sources live (tapback, outbound_edit, user_feedback,
  implicit, reflection_retry) with per-source weekly counts in doctor;
  silent-source alarm (any source at 0 for 7 days pages).
- **Plan**: post-deploy, verify tapback pairs appear within 48h; revive
  outbound_edit + user_feedback collectors (died May 16/29 — audit those
  paths the same way tapback was audited); add per-source freshness to
  `human doctor`.
- **Gate**: ≥25 fresh pairs/week across ≥3 sources, sustained a month.

### 8. Judge quality (RLAIF/DPO direction)
- **Now**: judge direction was inverted once (fixed, alignment 0.00→0.53);
  Gemini judges now schema-constrained.
- **SOTA**: every judge carries a direction-verification eval (planted
  known-good/known-bad pairs; judge must order them correctly before its
  output is consumed); cross-family confirmation for high-stakes verdicts.
- **Plan**: add planted-pair sanity check to each judge invocation batch;
  reject batches whose sanity accuracy < 95%; log alignment trend.
- **Gate**: alignment ≥ 0.7 on planted pairs continuously.

### 9. Register-conditional RAG
- **Now**: RAG-over-own-messages wired but default off; measured +0.110
  substantive / −0.078 casual.
- **SOTA**: retrieval fires only where it helps — register classifier picks
  RAG for substantive turns, none for casual; retrieved snippets carry
  provenance and never leak verbatim.
- **Plan**: wire the existing register classifier to
  `cfg.agent.rag_grounding_enabled` per-turn; A/B by register.
- **Gate**: substantive fidelity + with no casual regression in the A/B.

### 10. Activation steering (persona vectors)
- **Now**: built + validated live (verbosity monotonic, α∈[−1,1] safe),
  default off; Phase 3 (C overlay→coefficients) pending.
- **SOTA**: per-channel/per-register steering coefficients derived from the
  persona overlay (e.g., iMessage-casual gets brevity+warmth steering),
  composing with LoRA rather than fighting it.
- **Plan**: finish Phase 3 mapping; A/B steered vs unsteered per register;
  keep α clamped.
- **Gate**: composite improves with steering on, no axis regresses.

## The self — somebody home (11–15)

### 11. Interoception + somatic persistence
- **Now**: `hu_somatic` exists per-agent-lifetime; A4 has no source file;
  state resets on restart.
- **SOTA**: energy / social-battery / mood persist across restarts (repo
  pattern: memory.db aggregate), drift on circadian + interaction load, and
  *visibly* shape behavior (short replies when drained, warmth when rested)
  — the substrate of "having a day."
- **Plan**: persist somatic state via a repo (E3 pattern); wire read-back
  at daemon boot; execute presence-somebody-home Phase for interoception.
- **Gate**: shadow log shows state continuity across restarts + behavior
  deltas correlate with state; blind raters report "mood consistency."

### 12. Intrinsic motivation → action surface
- **Now**: A3 ticks once/min but is propose-only ("logs intent, no action
  surface"); initiative proposer targets one contact, dry-run wiring
  incomplete end-to-end.
- **SOTA**: wants become sends: the propose→compose→governed-send tail is
  wired through the single funnel with salience + throttle gates; the
  system texts first because *it* has something, at human cadence.
- **Plan**: close the `init_proposer` tail (presence plan's agency phase);
  every proposal passes topic-quality + relationship-calibration gates;
  OFF→SHADOW→LIVE per contact.
- **Gate**: recipient reply-rate to unprompted sends ≥ Seth's own baseline;
  zero "why is it texting me this" reports.

### 13. Continuity — the life thread
- **Now**: commitments tracked; life_chapters/life-thread scoped per
  contact; but the system rarely references its *own* prior sends or day.
- **SOTA**: conversational memory of self: callbacks to its own last
  message, follow-through on its own promises unprompted, a consistent
  "what I did today" thread drawn from feeds/intelligence cycle — the
  continuity that makes a self legible.
- **Plan**: inject own-last-send + open-commitments into the director
  context (small prompt sections, budget-aware); commitment follow-through
  becomes an initiative source; day-thread summarizer feeds proactive
  context.
- **Gate**: blind raters can't flag "goldfish memory" tells; commitment
  follow-through rate > 80%.

### 14. Opinions + taste that hold
- **Now**: A2 taste live; opinions stored (opinions repo) but rarely
  defended under pushback — assistant agreeableness wins.
- **SOTA**: stances persist across turns and contacts; polite-but-firm
  under disagreement (the single strongest anti-assistant tell); taste
  shows in unprompted reactions to shared links/music.
- **Plan**: opinion-consistency directive in the director when a stored
  stance is challenged; DPO pairs from agreeableness-collapse examples
  (mine transcripts for "instant fold" turns, prefer the held-stance
  rewrite).
- **Gate**: adversarial pushback eval: stance retention ≥ 80% without
  rudeness regression.

### 15. Initiative quality + cadence
- **Now**: proactive check-ins are cron-shaped (10am daily registrations);
  contextual proactive gated; 2026-07-18 killed the garbage class.
- **SOTA**: initiations look Poisson-bursty like a human, keyed to real
  triggers (something seen, felt, remembered — feeds, somatic state,
  commitments) instead of wall-clock; per-contact frequency matched to the
  real relationship's historical cadence.
- **Plan**: replace cron check-ins with trigger-sourced proposals (12);
  per-contact cadence budgets learned from chat.db history; jitter that
  mimics measured inter-initiation distributions.
- **Gate**: initiation-time distribution statistically indistinguishable
  from Seth's own (KS test) per contact.

## The relationship — per-person humanness (16–18)

### 16. Relationship calibration
- **Now**: A4 axis exists in the composite; warmth tiers static in persona
  contacts; contact_style_evolution records but doesn't drive.
- **SOTA**: per-contact register/warmth/emoji/length adapt continuously
  from that contact's own behavior (already recorded), bounded by persona;
  new contacts cold-start from relationship class priors.
- **Plan**: close contact_style_evolution → overlay loop (currently
  write-only); weekly per-contact calibration report; A/B on the top-3
  contacts.
- **Gate**: A4 axis ≥ 0.8; per-contact style match within ±10pp.

### 17. Theory of mind → response shaping
- **Now**: ToM directive env-on; expectation tracking fixed (2026-07-12
  crashloop); pragmatics digest reaches prompts via persona_ctx.
- **SOTA**: tracked expectations visibly change replies (they asked twice →
  acknowledge the miss; they're stressed → drop the joke), and
  mispredictions feed the learning loop as implicit_feedback.
- **Plan**: eval set of expectation-sensitive scenarios; wire ToM
  prediction-vs-outcome deltas into implicit_feedback source.
- **Gate**: expectation-scenario eval ≥ 85%; implicit_feedback volume up
  without noise (judge sanity holds).

### 18. Reactions as expression (send-side tapbacks)
- **Now**: director can choose tapback (reaction=2 events in logs); 3-tier
  fallback chain; timing sometimes inhuman (~2h late "Loved").
- **SOTA**: tapback-vs-text choice matches Seth's measured usage (when he
  Loves vs replies); latency human (seconds–minutes); never the
  echo-as-text fallback shape.
- **Plan**: measure Seth's tapback rate/latency per contact from chat.db;
  gate director tapback timing on those stats; kill stale tapbacks (drop if
  > 15 min).
- **Gate**: tapback rate/latency within measured bands; zero text-echo
  fallbacks in prod.

## The proof — measurement + fleet health (19–20)

### 19. The composite + blind A/B cadence
- **Now**: 4-axis composite built end-to-end, ran once (0.763 bootstrap,
  no production prompt); blind-A/B pipeline skill exists; no cadence.
- **SOTA**: nightly composite on the *production* prompt path with per-axis
  no-regression gating every adapter/config promotion; monthly human blind
  A/B (the only ground truth); Gemini-as-judge weekly as the cheap proxy —
  "better than Gemini" = Gemini itself can't tell h-uman from Seth.
- **Plan**: point `scripts/humanness_nightly.py` at the production prompt
  builder (finding #1 from the first run); schedule nightly + weekly judge
  runs; wire verdicts into the lora-nightly gate
  (`~/.human/blind_ab_gate.json` already consumed).
- **Gate**: this IS the gate. Composite ≥ 0.85, judge accuracy ≤ 55%.

### 20. Fleet health — make silent regressions impossible
- **Now**: doctor caught the RL_FULL gap but nobody ran it; guard-sentinel
  caught one regression; config drift found by hand (style governor still
  in shadow, adapter mismatch, stale scheduled.json) — all discovered by
  audit, not alerting.
- **SOTA**: every silent failure class from this audit has a tripwire:
  doctor runs on a schedule and pages on any FAIL; deploy refuses on any
  doctor FAIL (not just guard sentinels); env/config drift snapshot diffed
  daily; per-source learning freshness (7); queue inspection
  (scheduled.json age + content quality) in doctor.
- **Plan**: `human doctor --json` nightly via launchd + notifier; extend
  install script to run doctor post-install and roll back on FAIL; add the
  4 new checks (adapter-base match, source freshness, queue quality, env
  drift).
- **Gate**: the next regression of any audited class is caught by a
  tripwire before a human notices. Measured by incident postmortems: zero
  "found by audit" recurrences.

---

## Sequencing (what unblocks what)

```
   [done 2026-07-18] 3-garbage-kill, 7-flywheel-restart, 4-governor-live, 6-v5
        │
   Wave 1 (this week):  6 validate+swap · 19 nightly-on-prod-prompt · 20 doctor-cron
        │                 (measurement first — everything after gates on it)
   Wave 2:  1 single-funnel · 3 LLM-composed follow-ups · 5 trim-live
        │
   Wave 3:  11 somatic-persist · 12 propose→send tail · 13 continuity
        │                 (the presence plan, now measurable by 19)
   Wave 4:  14 opinions · 15 cadence · 16 relationship · 17 ToM · 18 tapbacks
        │
   Continuous:  2 guards · 7/8 flywheel+judges · 9/10 RAG+steering A/Bs
```

The order is deliberate: **measurement (19/20) before presence (11–15)** —
the presence work is exactly the kind that feels better than it measures,
and the 2026-07-18 audit is the proof that unmeasured subsystems rot
silently. Nothing in waves 2–4 flips LIVE without its gate.
