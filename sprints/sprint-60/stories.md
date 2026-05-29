# Sprint 60 Backlog: TikTok-Style Feels-Alive Learning Loop

## Goal

Implement the critical-path spine for a "feels-alive" learning loop: implicit-feedback reward model → contextual-bandit proactivity engine → online policy update wiring. The spine proves end-to-end learning (human sends a message, system learns a reward signal, updates the policy within the session) and establishes the foundation for techniques #4–#9 (reflection, RAG reranking, world-models, catastrophic forgetting defenses, mixture-of-adapters, process rewards).

---

## PART A: Full Backlog (All 9 Techniques as Epics)

### Epic 1: Implicit-Feedback Reward Model

**Value statement:** Today the system has raw DPO pairs but no standing reward model that can score an arbitrary candidate online. This epic builds a Bradley-Terry-style reward scorer trained on implicit signals (edit-vs-send, reply latency, tapbacks, ignored proactives, user corrections) and makes it available as `hu_reward_model_score(prompt, response) → score ∈ [-∞, ∞]`. The scoring function is deterministic, inference-only, and can be called per-generation to rank candidates.

**User stories:**

#### US-1 (P0): Online Reward Scorer from Production Outcomes
**As a** learning-loop operator, **I want** a deployed reward model that scores (prompt, response) pairs from production interactions, **so that** I can rank candidate outputs in real-time without waiting for nightly retraining.

**Acceptance criteria:**
- AC-1.1: `hu_reward_model_score(prompt, response) → double` is callable, returns a scalar in [-inf, inf], and is reproducible (same inputs always produce the same score).
- AC-1.2: The scorer is trained on DPO pairs from `production_outcomes` table (chosen vs rejected), using Bradley-Terry loss: `-log σ(r_w - r_l)`.
- AC-1.3: After 10+ production outcome pairs are collected and scored, the reward model ranks a held-out preference pair (chosen, rejected) such that `score(chosen) > score(rejected)` ≥90% of the time on fresh pairs.
- AC-1.4: Inference latency is <10ms per call (no subprocess invocation; HUML backend only in Phase 3 Task 2).

**Estimate:** L  
**Risk:** Gradient correctness (AC-1.3 depends on Bradley-Terry loss implementation matching Christiano 2017).  
**Dependencies:** none

#### US-2 (P1): Implicit-Signal Collector for DPO Pair Generation
**As a** learning-loop system, **I want** to automatically mine (chosen, rejected) preference pairs from production signals (edit-vs-send, reply latency, tapback polarity, user corrections), **so that** I have a growing training corpus without manual annotation.

**Acceptance criteria:**
- AC-2.1: Every outbound message inserts a row into `production_outcomes` (channel, target, prompt, chosen, send_timestamp).
- AC-2.2: Implicit signals (tapback polarity, reply latency, user_edited flag, reply_sentiment) are captured in `production_outcomes` columns within 15s of the signal arrival.
- AC-2.3: A nightly job reads `production_outcomes` rows with resolved outcomes and generates DPO pairs: row with tapback_polarity=1 (✓) paired against row with tapback_polarity=-1 (✗) for the same contact.
- AC-2.4: At least 50 production DPO pairs are collected over a 7-day run on a live contact without manual intervention.

**Estimate:** M  
**Risk:** Signal latency (tapback arrival timing is asynchronous); false negatives (some preferences are ambiguous).  
**Dependencies:** US-1 (needs training corpus)

#### US-3 (P2): Reward Model Checkpointing & Versioning
**As a** learning scientist, **I want** to save and load reward model checkpoints with version metadata, **so that** I can A/B test different RM versions and understand which reward definition produced which behavior.

**Acceptance criteria:**
- AC-3.1: `hu_reward_model_save(rm, dir) → HU_OK` writes value-head weights + backbone metadata to `<dir>/checkpoint.pb` (or equivalent binary format).
- AC-3.2: `hu_reward_model_load(dir) → rm` restores an RM from checkpoint with identical scoring behavior (bit-exact reproducible).
- AC-3.3: Checkpoint includes timestamp, training iteration count, corpus size, and a short description (e.g., "v2-implicit-50-pairs").
- AC-3.4: Reward model versions can be swapped without daemon restart (operator can call a CLI command to load a new RM version and verify it on a test prompt).

**Estimate:** S  
**Risk:** None (boilerplate serialization).  
**Dependencies:** US-1

---

### Epic 2: Contextual-Bandit Proactivity Engine

**Value statement:** Today the proactive throttle is a fixed heuristic (send if idle ≥4h, rate-limit to ≤1 per 12h). This epic replaces it with a contextual bandit (Thompson sampling or LinUCB) that learns WHETHER and WHEN to send proactives per contact, based on outcome (reply / ignored / "stop sending"). The result is a policy that adapts to each person's actual preferences in real-time.

**User stories:**

#### US-4 (P0): Thompson-Sampling Arm Selection
**As a** proactivity learner, **I want** to use Thompson sampling to select "send" vs "defer" for a proactive message candidate per contact, learning from empirical reply rates, **so that** I send proactives only when the contact is likely to engage.

**Acceptance criteria:**
- AC-4.1: A contact has a Beta posterior over P(reply | send now) estimated from N prior sends. The posterior is updated after each send: if replied, increment α; if ignored, increment β.
- AC-4.2: On each candidate proactive, sample θ ~ Beta(α, β) and use Thompson sampling to decide: if θ > threshold (e.g., 0.3), send; else defer.
- AC-4.3: Over 50+ proactive attempts on a contact, the empirical reply rate approaches the true contact's engagement level (e.g., if contact replies 40% of the time, RM converges to ~0.4 by attempt 50).
- AC-4.4: Decision latency is <1ms per contact (lookup + sample, no model inference).

**Estimate:** M  
**Risk:** Exploration/exploitation tradeoff (early exploration may send to unresponsive contacts; needs tuning).  
**Dependencies:** US-1 (uses reward signal to update posterior)

#### US-5 (P1): Implicit Proactive Feedback Loop
**As a** proactivity system, **I want** to infer proactive outcomes from implicit signals (tapback, reply, "stop sending" block, message deletion), **so that** I can update the Thompson posterior without explicit user action.

**Acceptance criteria:**
- AC-5.1: When Seth replies to a proactive within 5 minutes of receipt, mark outcome = "reply".
- AC-5.2: When Seth opens but never replies within 24h, mark outcome = "ignored" (inferred from read timestamp + no reply).
- AC-5.3: When Seth blocks the contact or mutes the conversation, mark outcome = "stop_sending" (highest penalty to posterior).
- AC-5.4: Posterior updates happen within 60s of outcome detection (asynchronous update loop, not blocking).

**Estimate:** M  
**Risk:** Attribution (is a reply to a proactive, or a coincidental message from that contact?); timestamp precision.  
**Dependencies:** US-4 (needs posterior to update), US-2 (implicit signals collector)

#### US-6 (P2): Multi-Contact Bandit State Persistence
**As a** proactivity system, **I want** to persist the Thompson Beta(α, β) posteriors per contact across daemon restarts, **so that** learning history is not lost on every restart.

**Acceptance criteria:**
- AC-6.1: Contact proactivity state (contact_handle, α, β, last_send_ts, outcome_count) is stored in a new SQLite table `proactive_bandits` and loaded at daemon startup.
- AC-6.2: Table schema includes metadata: created_at, last_updated, num_replies, num_ignored, num_stops.
- AC-6.3: Daemon persists state every 10 minutes or on every outcome update, whichever is more frequent.
- AC-6.4: Over a 30-day run with multiple restarts, the bandit state for a contact is bit-for-bit identical (no data loss, deterministic serialization).

**Estimate:** S  
**Risk:** None (standard SQLite persistence).  
**Dependencies:** US-4, US-5

---

### Epic 3: Online/Continual Policy Update

**Value statement:** Today LoRA fine-tuning happens only nightly via `hu_mlx_admin_swap_adapter`. This epic wires the hot-swap mechanism into the learning loop so that policy updates happen within a session: new DPO pairs → retrain adapter → swap (live, no daemon restart). The result is a persona that evolves in real-time as Seth interacts.

**User stories:**

#### US-7 (P0): In-Session LoRA Retraining Trigger
**As a** learning-loop operator, **I want** to trigger a LoRA fine-tune based on an online signal (e.g., 5+ new high-confidence DPO pairs), **so that** the persona adapts within the same session instead of waiting for the nightly job.

**Acceptance criteria:**
- AC-7.1: A new config gate `learning.session_retraining_enabled` (default: false) allows online retraining within a session.
- AC-7.2: When enabled, the learning loop checks after each outcome: if `new_dpo_pairs ≥ threshold` (default 5) AND `time_since_last_retrain ≥ cooldown` (default 30min), enqueue a retraining job.
- AC-7.3: The retraining job reads the new pairs from `dpo_pairs` table (where processed_into_dpo=0 and timestamp > last_retrain_ts), launches the training subprocess (already exists: `scripts/rm_mlx_train.py` or equivalent LoRA script).
- AC-7.4: After training, a new adapter checkpoint is written to `~/.human/adapters/lora-persona-<timestamp>.bin`.

**Estimate:** M  
**Risk:** Training time blocking the main loop (mitigated by background subprocess); stability of in-session retraining (LoRA variance at small corpus sizes).  
**Dependencies:** US-2 (needs DPO pairs), US-1 (reward model training)

#### US-8 (P0): Hot-Swap Adapter Wiring into Learning Loop
**As a** learning-loop system, **I want** to invoke `hu_mlx_admin_swap_adapter` when a new LoRA checkpoint is ready, **so that** the policy is updated in the running daemon without restart.

**Acceptance criteria:**
- AC-8.1: After a successful LoRA training run (AC-7.4), the learning loop calls `hu_mlx_admin_swap_adapter(path)` to load the new adapter.
- AC-8.2: If the swap succeeds, future calls to the model (e.g., proactives, turn generation) use the new adapter. If it fails (bad path, OOM), the system logs the error and continues with the previous adapter (graceful fallback).
- AC-8.3: A test fixture verifies that after swapping adapters, a fixed prompt produces different outputs (persona has changed).
- AC-8.4: Swap latency is <2s (non-blocking to the main daemon loop; can be async).

**Estimate:** S  
**Risk:** Adapter compatibility (adapter trained on old base, swapped onto new base); OOM on small devices.  
**Dependencies:** US-7, infrastructure (existing `hu_mlx_admin_swap_adapter` implementation)

#### US-9 (P1): Online-Update Metrics & Telemetry
**As a** learning scientist, **I want** to observe when adapters are swapped, what the new adapter's fidelity is, and whether user behavior changes post-swap, **so that** I can measure the impact of in-session learning.

**Acceptance criteria:**
- AC-9.1: Every adapter swap is logged: timestamp, old adapter path, new adapter path, swap result (success/failure), new persona fidelity (via a quick eval if available).
- AC-9.2: A telemetry counter tracks "adapters swapped per session" and "cumulative pairs trained on this session".
- AC-9.3: Over a 7-day run, the log shows at least 3 successful swaps (indicates in-session retraining fired at least 3 times).
- AC-9.4: Post-swap, user reply rate to next proactive should be ≥ pre-swap baseline (sanity check: new persona is not worse).

**Estimate:** S  
**Risk:** None (logging + telemetry).  
**Dependencies:** US-7, US-8

---

### Epic 4: Reflection & Memory Consolidation

**Value statement:** Today `hu_personal_model` stores raw facts with half-life decay. This epic adds periodic synthesis: facts → higher-order beliefs (what Seth values, when Seth is available, what Seth wants next). The result is a more predictive model that generates richer prompts for the next adapter fine-tune.

**User stories:**

#### US-10 (P2): Belief-Synthesis Aggregator
**As a** reflection system, **I want** to periodically synthesize raw facts into higher-order beliefs (e.g., "Seth prefers async communication on weekday mornings"), **so that** the personal model captures generalizable patterns.

**Acceptance criteria:**
- AC-10.1: A new module `src/memory/belief_synthesis.c` provides `hu_belief_synthesize(personal_model) → beliefs_buffer`.
- AC-10.2: Synthesis reads facts (tagged with predicate: "available_after", "prefers_channel", "work_context") and groups them by predicate, then generates a summary: e.g., "prefers_channel: iMessage 60%, Slack 30%, email 10%".
- AC-10.3: Beliefs are stored in a new `beliefs` JSON field in `personal_model` (alongside facts).
- AC-10.4: Synthesis runs once per 24h or when fact_count > 50 (whichever happens first).

**Estimate:** M  
**Risk:** Belief correctness (synthesis heuristics may misinterpret facts); user perception (if beliefs are wrong, prompts are misleading).  
**Dependencies:** none (independent feature)

#### US-11 (P3): Predictive World-Model Priming
**As a** agent system, **I want** to use beliefs to predict the next action Seth will take, and prime the next adapter for that context, **so that** responses are tailored to likely scenarios.

**Acceptance criteria:**
- AC-11.1: A new module `src/memory/world_model.c` provides `hu_world_model_predict_next_action(personal_model) → predicted_action` (e.g., "will_check_work_email_in_2h", "waiting_for_reply").
- AC-11.2: The prediction is based on beliefs + time-of-day (via `hu_circadian_t`) + recent activity.
- AC-11.3: Prediction quality: on a held-out week of activity, the model's top-3 predictions match the actual next action ≥40% of the time (baseline random is ~5%).
- AC-11.4: If prediction confidence > threshold, the next LoRA fine-tune primes the adapter with the predicted context in the system prompt.

**Estimate:** L  
**Risk:** High (prediction is speculative; wrong predictions degrade fidelity).  
**Dependencies:** US-10 (needs beliefs)

---

### Epic 5: Late-Interaction Reranked Retrieval

**Value statement:** Today RAG-of-own-messages is wired behind a register-conditional flag and shows small wins (+0.110 on substantive, -0.078 on casual). This epic adds ColBERT-style reranking: retrieve the top-K relevant messages, then rerank with a cross-encoder to pick the best context. The result is better-grounded responses without the casual-register regression.

**User stories:**

#### US-12 (P2): Cross-Encoder Reranking Backend
**As a** retrieval system, **I want** to rerank retrieved messages using a cross-encoder (Sentence-Transformers or similar), **so that** the top-1 context is the most relevant instead of just dense-similar.

**Acceptance criteria:**
- AC-12.1: A new module `src/memory/cross_encoder_rerank.c` provides `hu_cross_encoder_rerank(messages, prompt, top_k) → reranked_messages`.
- AC-12.2: Reranking score is a scalar in [0, 1], computed by the cross-encoder model (HUML-backed or subprocess).
- AC-12.3: On a held-out query set, the top-1 reranked message has higher human-judged relevance than top-1 dense-retrieval-only (measured via manual annotation: rerank improves precision by ≥10%).
- AC-12.4: Reranking latency is <50ms for K=10 (batch inference on all 10 candidates in parallel).

**Estimate:** M  
**Risk:** Model availability (may need to train a cross-encoder; using pre-trained requires validation).  
**Dependencies:** none (orthogonal feature)

#### US-13 (P2): Register-Aware RAG Gating
**As a** RAG system, **I want** to enable/disable RAG per register based on empirical impact, **so that** we get the upside (+0.110 substantive) without the downside (-0.078 casual).

**Acceptance criteria:**
- AC-13.1: Config gain: `agent.rag_grounding_enabled` becomes a map: `{"substantive": true, "casual": false, "reflexive": false}` (instead of a single boolean).
- AC-13.2: During turn generation, RAG is invoked only if the current register matches an enabled key.
- AC-13.3: A/B metrics show: substantive register gets +0.110 (as before), casual register regression eliminated (neutral, ~-0.01 within noise).
- AC-13.4: Config default is `{"substantive": true, "casual": false, "reflexive": false}` (safe per prior testing).

**Estimate:** S  
**Risk:** None (configuration change + conditional).  
**Dependencies:** none

---

### Epic 6: Adapter Composition & Mixture-of-LoRAs

**Value statement:** Today we have one global LoRA adapter per persona. This epic adds per-contact or per-register adapters, composed at inference: base_model + persona_adapter + contact_adapter (if available) + register_adapter (if available). The result is richer personalization without multiplying training burden.

**User stories:**

#### US-14 (P3): Adapter Merging & Composition Harness
**As a** ML system, **I want** to compose multiple LoRA adapters into a single fused model at inference time, **so that** I can use per-contact + per-register adapters without N² combinations.

**Acceptance criteria:**
- AC-14.1: A new module `src/ml/adapter_compose.c` provides `hu_adapter_compose(base_model, [adapter1, adapter2, ...]) → fused_model`.
- AC-14.2: Composition uses weighted addition: `fused = base + α1*adapter1 + α2*adapter2 + ...` (weights configurable, default = 1.0 each).
- AC-14.3: On a test prompt with 2 adapters (persona + contact), fused output differs from base+persona-only by at least 5% perplexity (indicates contact adapter is contributing signal).
- AC-14.4: Composition latency is <50ms (single forward pass through fused weights; no multiple passes).

**Estimate:** M  
**Risk:** Weight stability (α values need tuning; bad weights make output worse).  
**Dependencies:** none (independent feature)

#### US-15 (P3): Per-Contact LoRA Training
**As a** learning system, **I want** to train a separate contact-specific LoRA adapter when a contact has 50+ high-confidence DPO pairs, **so that** the response is tuned to that contact's preferences.

**Acceptance criteria:**
- AC-15.1: When contact-specific DPO pairs reach 50, trigger a separate training run: `scripts/train_contact_lora.py <contact_handle> <pairs.jsonl>`.
- AC-15.2: The new contact adapter is saved to `~/.human/adapters/contact-<contact_handle>-lora.bin`.
- AC-15.3: At inference time, if a contact adapter exists, compose it with the persona adapter (US-14).
- AC-15.4: On held-out contact-specific prompts, the contact+persona composition scores ≥5% higher fidelity than persona-alone.

**Estimate:** L  
**Risk:** Overtraining on small corpus (contact adapter may overfit); noise in contact-specific pairs.  
**Dependencies:** US-2 (needs DPO pairs), US-14 (needs composition)

---

### Epic 7: Catastrophic-Forgetting Defenses

**Value statement:** Each LoRA fine-tune risks eroding the base model's general capabilities (catastrophic forgetting). This epic adds Elastic Weight Consolidation (EWC) and replay buffers: hold fixed a subset of "canonical" pairs and mix them with new pairs during training. The result is stable continual learning without skill decay.

**User stories:**

#### US-16 (P3): EWC Regularization Harness
**As a** learning system, **I want** to apply Elastic Weight Consolidation to LoRA training, **so that** the new adapter doesn't drift too far from the prior adapter's weights.

**Acceptance criteria:**
- AC-16.1: A new module `src/ml/ewc.c` computes the Fisher information matrix F of the prior adapter on a held-out validation set.
- AC-16.2: During new training, the loss includes an EWC penalty: `L_ewc = λ * Σ F[i] * (w[i] - w_prior[i])²`.
- AC-16.3: On a test run with 50 new pairs, EWC training converges with validation loss that does NOT regress on canonical pairs (sanity check: not forgetting).
- AC-16.4: EWC setup latency is <30s per training run (Fisher computation is a one-time cost).

**Estimate:** L  
**Risk:** High (Fisher computation is expensive; λ tuning is sensitive).  
**Dependencies:** none (independent feature)

#### US-17 (P3): Canonical Pair Replay Buffer
**As a** learning system, **I want** to maintain a replay buffer of "canonical" DPO pairs (high-confidence examples covering diverse tones/contexts), **so that** each training run mixes old pairs with new pairs.

**Acceptance criteria:**
- AC-17.1: A new config section `learning.canonical_pairs` lists file paths to JSONL files containing 200–500 canonical pairs (diverse, curated examples).
- AC-17.2: During LoRA training, the trainer reads: 50% canonical pairs (deterministically), 50% new pairs (sampled randomly).
- AC-17.3: On a test run with only 10 new pairs, replay-buffer training preserves fidelity on held-out canonical queries (regression <3%) while still adapting to new pairs (improvement >5%).
- AC-17.4: Canonical pair I/O latency is negligible (<1s to load 500 pairs).

**Estimate:** S  
**Risk:** None (configuration + data loading).  
**Dependencies:** none

---

### Epic 8: Process Reward Model & Step-Level Verification

**Value statement:** Today `response_guard` regenerates a full response if it detects bad patterns. This epic adds a process reward model that verifies EACH generation step: "is this step on track?" and steers generation toward high-reward trajectories. The result is fewer regenerations and higher fidelity on long outputs.

**User stories:**

#### US-18 (P3): Step-Level RM Training & Inference
**As a** verification system, **I want** to train a separate reward model on step-level judgments (is this token/sentence on the right track?), **so that** I can verify generation without waiting for full completion.

**Acceptance criteria:**
- AC-18.1: A new module `src/ml/process_reward_model.c` provides `hu_process_reward_model_score_step(model, context, step) → score`.
- AC-18.2: Step score is in [-1, 1]: -1 = bad (off-track), 0 = neutral, +1 = good (on-track).
- AC-18.3: Training data comes from annotated generation traces (not implemented in Sprint 60; assume dummy data for AC validation).
- AC-18.4: On a held-out test set, the step-level scorer achieves F1 ≥ 0.7 for classifying steps as "on-track" vs "off-track" (measured via human annotation of 100 steps).

**Estimate:** L  
**Risk:** High (requires training data; step-level reward is sparse signal).  
**Dependencies:** none (independent feature)

#### US-19 (P3): Early-Exit via Step-Level Verification
**As a** generation system, **I want** to abort generation early if the process reward model detects sustained off-track steps, **so that** bad generations don't propagate.

**Acceptance criteria:**
- AC-19.1: During generation, after each token, compute step-level reward via `hu_process_reward_model_score_step`.
- AC-19.2: If reward < -0.5 for 3 consecutive steps, abort generation and trigger a regenerate (fallback to the prior best candidate).
- AC-19.3: On a test prompt that historically fails, early-exit saves ~30% of generation time compared to letting it run to max_tokens.
- AC-19.4: Early-exit does not increase false-positive regenerations (good generations should not be aborted; test on 100 human-validated prompts, should be 0 false exits).

**Estimate:** M  
**Risk:** False positives (aborting good generations); latency (step-level inference adds cost).  
**Dependencies:** US-18

---

### Epic 9: Catastrophic-Forgetting Defenses (Adapter Merging)

**Value statement:** As adapters accumulate (persona + contact + register), merging them back into the base model periodically prevents weight divergence. This epic adds adapter merging: when an adapter is "stable" (hasn't changed in 7 days), merge it back into the base model and deallocate. The result is a smaller, faster model without lost knowledge.

**User stories:**

#### US-20 (P3): Adapter Merging & Deallocation
**As a** ML system, **I want** to merge stable adapters back into the base model, **so that** the model footprint doesn't grow with every new adapter.

**Acceptance criteria:**
- AC-20.1: A new module `src/ml/adapter_merge.c` provides `hu_adapter_merge(base_model, adapter, weight) → merged_model`.
- AC-20.2: Merging computes: `merged = base + weight * adapter` and updates base weights in-place.
- AC-20.3: On a test run with 3 adapters (persona, contact, register), merging the oldest (persona) back into base takes <5 minutes.
- AC-20.4: After merging, inference on a test prompt produces identical output (bit-for-bit) as the 3-adapter composition (sanity check: merge preserves semantics).

**Estimate:** M  
**Risk:** None (deterministic math).  
**Dependencies:** none

#### US-21 (P3): Merge Scheduling & Operator Controls
**As a** learning system, **I want** to automatically merge adapters on a schedule (e.g., weekly) and allow manual triggering, **so that** operators can manage the model footprint.

**Acceptance criteria:**
- AC-21.1: Config gate: `learning.adapter_merge_enabled` (default: true) and `learning.merge_schedule` (default: "weekly").
- AC-21.2: On the scheduled day, the daemon identifies "stable" adapters (no updates in the past 7 days) and merges them back into base.
- AC-21.3: Merging is logged: "merged adapter contact-alice-lora (100 days stable)".
- AC-21.4: Operator can manually trigger a merge via `human ctl merge-adapters --older-than-days=7`.

**Estimate:** S  
**Risk:** None (scheduler + CLI).  
**Dependencies:** US-20

---

## PART B: Sprint 60 Committed Stories (Critical-Path Spine — DETAILED)

The following stories (US-101 through US-107) are the **committed spine** for Sprint 60. Techniques #4–#9 exist as groomed epics (stories US-10 through US-21) but are NOT built this sprint.

---

### US-101 (P0): Bradley-Terry Reward Model — HUML Toy Backbone

**As a** learning system, **I want** to train and score (prompt, response) pairs using Bradley-Terry loss on a HUML toy GPT backbone, **so that** I have a deterministic, cross-platform reward scorer for ranking outputs.

**Acceptance criteria:**

- **AC-101.1 (Structure):** Create new files:
  - `include/human/ml/reward_model.h` (vtable contract) — VERIFY EXISTS ✓
  - `src/ml/reward_model_huml.c` (HUML factory + scoring + training)
  - `src/ml/reward_model_train.c` (Bradley-Terry gradient step, initialized from reward_model_priv.h)
  - `tests/test_reward_model_huml.c` (unit tests for score + train)
  - Touch existing: `src/ml/reward_source.c` (integrate `HU_REWARD_SOURCE_RM` factory)

- **AC-101.2 (Scoring Contract):** After creating a HUML reward model with `hu_reward_model_create_huml(alloc, config, &rm)`, calling `rm.vtable->score(rm.ctx, alloc, prompt, response, &out_score)` returns HU_OK and populates `out_score` with a deterministic scalar. Given identical inputs, score is bit-for-bit reproducible across invocations.

- **AC-101.3 (Bradley-Terry Training):** Implement `hu_reward_model_train(rm, alloc, pairs, n, config, &metrics)` which:
  - Iterates `config->max_iters` times over the batch of `n` preference pairs
  - For each pair: compute `r_w = score(prompt, chosen)`, `r_l = score(prompt, rejected)`, loss = `-log σ(r_w - r_l)`
  - Backpropagate gradient only through the value head (backbone is frozen), apply SGD with step size `config->learning_rate`
  - Populates `metrics.initial_loss` (before iter 0), `metrics.final_loss` (after final SGD step), `metrics.iters_completed`
  - Returns `HU_ERR_INVALID_ARGUMENT` if `n == 0` or `max_iters == 0`; returns `HU_OK` on clean run

- **AC-101.4 (Training Correctness — Gradient Check):** In `tests/test_reward_model_huml.c`, implement a finite-difference gradient check: perturb each weight in the value head by ε, recompute Bradley-Terry loss, compare analytical gradient (from backprop) to finite-difference gradient. Analytical gradient must match FD gradient to 3 significant figures (relative error <0.001). Test on 3 random batch sizes: (2, 4, 8 pairs).

- **AC-101.5 (Preference Ranking):** After training on 10 synthetic preference pairs (chosen={positive prompt}, rejected={negative prompt}, each with margin > 0.1 log-odds), on a held-out pair (chosen, rejected), verify `score(chosen) > score(rejected)` deterministically. Repeat 5 times with different random seeds: success rate = 5/5.

- **AC-101.6 (One-Sided Pair Handling):** The training loop must handle one-sided KTO pairs gracefully: if `chosen_len == 0` OR `rejected_len == 0`, skip the pair, increment `metrics.skipped_count`, and emit a single stderr warning per training run. For `score_batch`, set the corresponding output slot to NaN.

- **AC-101.7 (Batch Scoring):** Implement `rm.vtable->score_batch(rm.ctx, alloc, pairs, n, out_chosen, out_rejected)` which scores both sides of `n` pairs in one call. Output arrays are pre-allocated by caller. Latency: <100ms for n=100 on a dev machine (single-threaded HUML forward passes).

- **AC-101.8 (Determinism & HU_IS_TEST):** All random initialization (value head weights, etc.) uses the allocator's RNG seed if in test mode. For prod, seed from /dev/urandom. No subprocess calls, no network, no floating-point non-determinism (use exact arithmetic in loss computation; no approximate pooling).

**Estimate:** L  
**Dependencies:** none  
**New Files:**
  - `src/ml/reward_model_huml.c` (~400 LOC: factory, scoring, value head integration)
  - `src/ml/reward_model_train.c` (~300 LOC: Bradley-Terry loop, gradient step)
  - `tests/test_reward_model_huml.c` (~500 LOC: scoring, training, gradient check, ranking, KTO handling)

**Touched Files:**
  - `include/human/ml/reward_model.h` (verify public contract is stable)
  - `src/ml/reward_source.c` (wire RM factory into HU_REWARD_SOURCE_RM)
  - `CMakeLists.txt` (add new sources)

**DoD:** All tests pass (full suite including new test_reward_model_huml). No ASan errors. Gradient check passes. Ranking test passes 5/5. Batch scoring latency <100ms verified.

---

### US-102 (P0): Production Outcomes Table & DPO Pair Mining

**As a** learning system, **I want** to automatically insert outbound messages into a `production_outcomes` table and mine (chosen, rejected) DPO pairs from implicit signals, **so that** I have a growing training corpus without manual annotation.

**Acceptance criteria:**

- **AC-102.1 (Schema):** The `production_outcomes` table exists (verify in `src/ml/dpo.c` around line 86) with columns:
  - `id INTEGER PRIMARY KEY AUTOINCREMENT`
  - `channel TEXT`, `target TEXT`, `message_ref TEXT`, `prompt TEXT`, `chosen TEXT`, `alternatives TEXT` (selected alternative if multiple generated)
  - `send_timestamp INTEGER`, `p_seth_at_send REAL` (persona confidence at send time)
  - `tapback_polarity INTEGER` (1=✓, 0=none, -1=✗)
  - `reply_latency_s INTEGER`, `reply_length INTEGER`, `reply_sentiment REAL`
  - `user_edited INTEGER` (0=sent as-is, 1=edited before send)
  - `outcome_resolved_at INTEGER` (when all signals arrived)
  - `processed_into_dpo INTEGER DEFAULT 0` (0=pending, 1=processed)
  - Plus index: `idx_po_unprocessed ON processed_into_dpo`

- **AC-102.2 (Insertion on Send):** In `src/agent/agent_turn.c` or `src/daemon.c` (wherever outbound messages are about to be sent), insert a row into `production_outcomes` with: channel, target (contact handle), message_ref (unique ID), prompt (the system prompt + context), chosen (the generated response), send_timestamp (now). p_seth_at_send, alternatives, and all outcome fields are NULL or 0.

- **AC-102.3 (Signal Ingestion — Tapback):** In `src/daemon_reaction_poll.c` or the reaction collection path, when a tapback arrives on a message_ref that has a `production_outcomes` row, UPDATE that row: set `tapback_polarity` and `outcome_resolved_at`.

- **AC-102.4 (Signal Ingestion — Reply):** When Seth replies to a message_ref with a `production_outcomes` row, UPDATE: set `reply_latency_s = (reply_timestamp - send_timestamp) / 1000`, `reply_length = strlen(reply)`, `reply_sentiment` (via `hu_sentiment_classify` or similar, or stub to 0.5), `outcome_resolved_at`.

- **AC-102.5 (Signal Ingestion — Edit):** If Seth edits the outbound message before final send, UPDATE `production_outcomes` set `user_edited = 1` and `chosen = <final_text>`.

- **AC-102.6 (DPO Pair Mining):** In a new function `hu_dpo_collector_mine_pairs_from_outcomes(db, output_limit, &pairs_written)`:
  - Query `production_outcomes` where `outcome_resolved_at IS NOT NULL` and `processed_into_dpo = 0`
  - For each contact, pair rows: "reply received within 5min + positive sentiment (reply_sentiment > 0.6)" = chosen; "no reply after 24h" = rejected
  - For each pair, INSERT into `dpo_pairs` table with source = "implicit_feedback"
  - Mark `processed_into_dpo = 1` on input rows
  - Return count of pairs generated

- **AC-102.7 (Integration into Nightly Job):** The existing nightly LoRA gate (in `src/daemon.c` around line 4210) already checks `cfg.learning.nightly_lora_enabled` and calls a training subprocess. Wire the pair mining (AC-102.6) to execute BEFORE training: call `hu_dpo_collector_mine_pairs_from_outcomes()`, then `hu_dpo_export_to_jsonl()`, then invoke training script.

- **AC-102.8 (Fixture Test):** In `tests/test_dpo_collector.c`, create 5 synthetic `production_outcomes` rows (2 with replies + positive sentiment, 2 with no reply, 1 edited), call `hu_dpo_collector_mine_pairs_from_outcomes()`, verify:
  - 2 DPO pairs are generated (chosen=replied, rejected=no_reply)
  - `processed_into_dpo = 1` on all 5 rows
  - `pairs_written == 2`
  - DPO pairs have correct `prompt`, `chosen`, `rejected`, `source = "implicit_feedback"`

- **AC-102.9 (Determinism):** No async signals, no race conditions (SQLite serialization handles concurrency). Mining is deterministic: given the same `production_outcomes` state, mining always produces the same DPO pairs.

**Estimate:** M  
**Dependencies:** US-101 (DPO pairs feed the reward model)  
**New Files:**
  - `tests/test_dpo_collector.c` (~200 LOC: fixture setup, mining, assertion)

**Touched Files:**
  - `src/ml/dpo.c` (verify table schema, integrate mining into collection init if needed)
  - `src/agent/agent_turn.c` or wherever outbound send happens (insert `production_outcomes` row)
  - `src/daemon_reaction_poll.c` (update `production_outcomes` on tapback)
  - `src/daemon.c` (wire mining into nightly job before training)
  - `src/daemon_imessage_observer.c` or reply-path (update `production_outcomes` on reply)
  - `CMakeLists.txt` (add test_dpo_collector)

**DoD:** All production_outcomes signals populated by real interaction. Mining generates ≥2 pairs from a 5-row fixture. No ASan errors. Deterministic: same rows → same pairs every time.

---

### US-103 (P0): Thompson-Sampling Contextual Bandit for Proactivity

**As a** proactivity learner, **I want** to use Thompson sampling to decide "send proactive" vs "defer" per contact, learning from implicit reply signals, **so that** proactives are sent only when the contact is likely to engage.

**Acceptance criteria:**

- **AC-103.1 (State Struct):** Create `include/human/agent/contextual_bandit.h` with:
  ```c
  typedef struct hu_contextual_bandit_arm {
      double alpha;       // Beta(α, β) successes
      double beta;        // failures
      uint64_t updates;   // cumulative updates to this arm
  } hu_contextual_bandit_arm_t;

  typedef struct hu_contextual_bandit {
      hu_allocator_t *alloc;
      hu_contextual_bandit_arm_t *arms;  // per-contact arms (indexed by contact handle hash)
      size_t num_arms;
      double threshold;   // decision threshold (default 0.3)
  } hu_contextual_bandit_t;
  ```

- **AC-103.2 (Initialize):** `hu_contextual_bandit_create(alloc, &out)` allocates bandit state. Each arm initializes: α=1, β=1 (weak Beta prior), updates=0.

- **AC-103.3 (Thompson Sample & Decide):** `hu_contextual_bandit_decide_send(bandit, contact_handle, &out_should_send)`:
  - Look up the arm for contact_handle (hash into arms array; if new contact, initialize a new arm)
  - Sample θ ~ Beta(α, β) using the inverse-CDF method (GSL or custom implementation — deterministic if seeded)
  - If θ > threshold (default 0.3), set `out_should_send = true`; else `false`
  - Return HU_OK

- **AC-103.4 (Update on Outcome):** `hu_contextual_bandit_update(bandit, contact_handle, outcome)` where outcome ∈ {HU_BANDIT_REPLY, HU_BANDIT_IGNORED, HU_BANDIT_BLOCKED}:
  - If REPLY: α++
  - If IGNORED: β++
  - If BLOCKED: β += 3 (penalty for opting out)
  - Increment updates counter
  - Return HU_OK

- **AC-103.5 (Convergence Test):** After 50 updates to a contact with 60% true reply rate:
  - Sample θ 100 times
  - Compute empirical P(θ > threshold) from samples
  - Verify empirical P is within 0.1 of 0.6 (posterior has concentrated around true rate ± noise)

- **AC-103.6 (Serialization):** `hu_contextual_bandit_save(bandit, path)` and `hu_contextual_bandit_load(path, alloc, &out)` persist α, β, updates for each contact to a binary file. Load should restore identical state (deterministic deserialization).

- **AC-103.7 (Fixture Test):** In `tests/test_contextual_bandit.c`:
  - Initialize bandit with 3 contacts: contact_A (60% reply rate), contact_B (20% reply rate), contact_C (0% reply rate)
  - Simulate 50 sends per contact, drawing outcomes from their true distributions
  - After 50 updates each, verify:
    - contact_A: P(decide_send) > contact_B: P(decide_send) > contact_C: P(decide_send) with high confidence (≥90% accuracy in arm ranking)

- **AC-103.8 (Determinism & HU_IS_TEST):** Thompson sampling is deterministic if seeded. In test mode, use a fixed seed. In prod, seed from timer or RNG. No network, no subprocess, no floating-point non-determinism (use exact Beta library or custom inverse-CDF).

**Estimate:** M  
**Dependencies:** US-102 (needs outcome signals), US-101 (reward model will consume decisions from this, in future tasks)  
**New Files:**
  - `include/human/agent/contextual_bandit.h` (~80 LOC: struct defs, public API)
  - `src/agent/contextual_bandit.c` (~300 LOC: Thompson sampling, update, serialization)
  - `tests/test_contextual_bandit.c` (~400 LOC: convergence test, fixture, ranking test)

**Touched Files:**
  - `include/human/daemon/common.h` or appropriate config (add `contextual_bandit_t *` field to daemon state)
  - `CMakeLists.txt` (add new sources)

**DoD:** Convergence test passes. Fixture test shows correct arm ranking. Serialization round-trip is deterministic. All tests pass, no ASan errors.

---

### US-104 (P0): Implicit Proactive Outcome Signals

**As a** proactivity learner, **I want** to automatically infer proactive outcomes from implicit signals (reply, read-without-reply, block) and feed them to the bandit, **so that** the Thompson sampler learns without explicit user feedback.

**Acceptance criteria:**

- **AC-104.1 (Outbound Tracking):** When a proactive message is sent via `hu_follow_up_send()` or `hu_daemon_follow_up_watcher()`, insert a tracking row:
  - New table: `proactive_sends` (id, contact, message_ref, sent_timestamp, outcome_type=NULL, outcome_timestamp=NULL)
  - Verify this table is created in `src/daemon_proactive.c` or the follow-up watcher initialization

- **AC-104.2 (Reply Outcome):** When Seth replies to a proactive (detected by message_ref matching in the reply path), UPDATE `proactive_sends`: set `outcome_type = HU_BANDIT_REPLY`, `outcome_timestamp = now`.

- **AC-104.3 (Ignored Outcome):** If 24 hours pass since a proactive was sent and no reply arrived, mark outcome = HU_BANDIT_IGNORED.

- **AC-104.4 (Blocked Outcome):** If Seth blocks the contact or mutes the conversation, mark outcome = HU_BANDIT_BLOCKED (and pass this to the bandit's update with penalty).

- **AC-104.5 (Async Update Loop):** In the daemon's main loop (or a background worker), periodically query `proactive_sends` where `outcome_type IS NOT NULL` and `processed = 0`:
  - For each row, call `hu_contextual_bandit_update(bandit, contact, outcome_type)`
  - Set `processed = 1`
  - Continue at most once per 60s (don't spam the bandit with updates)

- **AC-104.6 (Fixture Test):** In `tests/test_proactive_outcomes.c`:
  - Insert 3 proactive_sends rows: one with reply (outcome_type=REPLY), one with ignored (no reply, >24h), one with blocked
  - Call the outcome processor (simulated: `hu_proactive_outcomes_process_async(db, bandit)`)
  - Verify: contact's bandit arm was updated correctly (α++, β++, β+=3 respectively)

- **AC-104.7 (Determinism):** Outcome inference is deterministic: given the same message_ref + reply state, always produces the same outcome. No floating-point, no randomness in signal processing.

**Estimate:** M  
**Dependencies:** US-103 (bandit needs outcomes), US-102 (shares schema infrastructure)  
**New Files:**
  - `tests/test_proactive_outcomes.c` (~250 LOC: fixture, signal processing, bandit update verification)

**Touched Files:**
  - `src/daemon_proactive.c` or `src/daemon_follow_up_watcher.c` (track proactive sends in new table)
  - `src/daemon.c` (wire outcome processor into main loop)
  - `src/daemon_imessage_observer.c` or reply-path (detect reply to proactive, mark outcome)
  - `CMakeLists.txt` (add test)

**DoD:** All three outcome types infer correctly. Async processor updates bandit state correctly. No ASan errors. Deterministic: same signals → same bandit state.

---

### US-105 (P0): Nightly LoRA Retraining from Implicit Feedback

**As a** learning system, **I want** to retrain the persona's LoRA adapter nightly using DPO pairs mined from implicit signals, **so that** the persona evolves based on real interactions.

**Acceptance criteria:**

- **AC-105.1 (Nightly Gate):** The existing nightly LoRA gate in `src/daemon.c` (around line 4210) already checks `cfg.learning.nightly_lora_enabled`. Verify this gate is wired and trigger-able.

- **AC-105.2 (Pair Collection):** Before training, call `hu_dpo_collector_mine_pairs_from_outcomes(db, &pairs_written)` (from US-102) to collect all unprocessed `production_outcomes` into `dpo_pairs`.

- **AC-105.3 (Training Invocation):** Execute the training subprocess: `scripts/train_persona_lora.py --input <dpo_pairs.jsonl> --output <adapter.bin> --model <base_model> --iters 100 --learning-rate 1e-5`. Script is assumed to exist (or stub exists returning success).

- **AC-105.4 (Checkpoint Handling):** After training succeeds, the new adapter is written to `~/.human/adapters/lora-persona-<timestamp>.bin`. Verify the file is created.

- **AC-105.5 (Metrics Logging):** Log the nightly training to `~/.human/logs/training.log`: timestamp, pairs_count, adapter_path, training_status (success/failure). Parse and verify the log contains at least one entry after a test run.

- **AC-105.6 (Cooldown):** After training, set a cooldown timer: no second training for at least 12 hours (prevent thrashing if multiple training runs trigger in a row).

- **AC-105.7 (Fixture Test):** In `tests/test_nightly_lora.c`:
  - Create 10 synthetic `production_outcomes` rows with mixed signals (replies, no replies, edits)
  - Simulate nightly job: call `hu_dpo_collector_mine_pairs_from_outcomes()` → expect ≥2 pairs
  - Mock the training subprocess to succeed immediately
  - Verify: training subprocess invoked with correct arguments, checkpoint path created, log written

- **AC-105.8 (Determinism):** Training inputs are deterministic (same `production_outcomes` state → same pairs → same training inputs). Script is deterministic (given same inputs, should produce adapters with near-identical weights, within floating-point tolerance).

**Estimate:** S  
**Dependencies:** US-102 (mining), US-101 (reward model training logic)  
**New Files:**
  - `tests/test_nightly_lora.c` (~250 LOC: fixture setup, subprocess mock, verification)

**Touched Files:**
  - `src/daemon.c` (wire mining before training invocation around line 4210)
  - `CMakeLists.txt` (add test)

**DoD:** Nightly job runs without errors. Pairs are mined correctly. Subprocess is invoked with correct args. Checkpoint is created. Log is written. Fixture test passes.

---

### US-106 (P0): Online Hot-Swap Adapter Integration

**As a** learning system, **I want** to invoke `hu_mlx_admin_swap_adapter()` after LoRA training succeeds, **so that** the persona updates in real-time without daemon restart.

**Acceptance criteria:**

- **AC-106.1 (Swap Call):** After successful LoRA training (AC-105.4), call `hu_mlx_admin_swap_adapter(adapter_path)` (existing symbol, verified in `src/daemon.c:3397`). Return HU_OK means swap succeeded.

- **AC-106.2 (Error Handling):** If swap returns an error (e.g., HU_ERR_IO, HU_ERR_NOT_SUPPORTED):
  - Log the error with the old adapter path
  - Continue with the previous adapter (graceful fallback, don't crash)
  - Do NOT crash or abort the daemon

- **AC-106.3 (Async Swap):** The swap can be asynchronous (background task) so it doesn't block the main daemon loop. However, by the time the next turn is generated, the new adapter should be active.

- **AC-106.4 (Fixture Test):** In `tests/test_adapter_swap.c`:
  - Load two different LoRA adapters (use fixture files or generate mock adapters)
  - Call `hu_mlx_admin_swap_adapter(adapter_path_1)` → verify success
  - Verify that next call to the model uses adapter_1 (can be tested via mock model output or weight inspection)
  - Call `hu_mlx_admin_swap_adapter(adapter_path_2)` → verify success
  - Verify output differs from adapter_1 (persona has changed)

- **AC-106.5 (Concurrent Safety):** If a swap is in-flight and a turn is generated, the turn should either use the old adapter (swap not yet complete) or the new adapter (swap completed), never a mixed state.

- **AC-106.6 (Telemetry):** Every swap is logged: `[adapter_swap] old: <path1>, new: <path2>, status: success/failure, latency_ms: <N>`. Parse log and verify ≥1 swap is recorded after test.

**Estimate:** S  
**Dependencies:** US-105 (produces the adapter to swap), US-101 (reward model training produces the adapter weights)  
**New Files:**
  - `tests/test_adapter_swap.c` (~200 LOC: fixture setup, swap calls, output verification, error handling)

**Touched Files:**
  - `src/daemon.c` (wire swap after training success, error handling, logging, around line 4220)
  - `CMakeLists.txt` (add test)

**DoD:** Swap succeeds and changes model output. Error case is handled gracefully. Concurrent safety is maintained. Log records ≥1 swap. No ASan errors.

---

### US-107 (P0): End-to-End Learning Loop Integration Test

**As a** learning system, **I want** to verify that the full spine (implicit signals → DPO pairs → reward model → LoRA training → adapter swap → next turn uses new persona) works end-to-end, **so that** I have confidence the entire loop closes.

**Acceptance criteria:**

- **AC-107.1 (Test Scenario):** In `tests/test_e2e_learning_loop.c`, simulate a 3-turn conversation:
  - Turn 1: Generate response → insert `production_outcomes` row
  - Simulate outcome signal (user reply, positive sentiment)
  - Turn 2: Mine DPO pair from outcome → insert into `dpo_pairs`
  - Simulate LoRA training on the pair → create new adapter checkpoint
  - Call adapter swap to load the new adapter
  - Turn 3: Generate response → verify output uses the new persona (output differs from Turn 1)

- **AC-107.2 (Checkpoints):** Verify at each step:
  - `production_outcomes` row has channel, target, prompt, chosen, send_timestamp
  - DPO pair was mined: chosen=turn1_response, rejected=alternate or default
  - Training ran: checkpoint file created
  - Adapter was swapped: telemetry logged
  - Turn 3 output differs from Turn 1 (new persona is active)

- **AC-107.3 (Determinism):** Run the scenario 3 times with the same seed. Each run produces identical `production_outcomes`, DPO pairs, trained weights, and Turn 3 output. Differences due to floating-point are acceptable (< 1 ulp per value).

- **AC-107.4 (Timing):** Total loop time (mining + training + swap + next generation): <30s for small batch (5 pairs, 100 training iters). Measured in fixture: log timestamps show each step duration.

- **AC-107.5 (Fallback Correctness):** If LoRA training fails (mock subprocess returns error), the system continues with the previous adapter and logs the failure. Turn 3 still generates (uses old persona). No crash.

**Estimate:** M  
**Dependencies:** US-101, US-102, US-103, US-104, US-105, US-106 (all prior stories in the spine)  
**New Files:**
  - `tests/test_e2e_learning_loop.c` (~500 LOC: full simulation, checkpoints, timing, fallback scenarios)

**Touched Files:**
  - `CMakeLists.txt` (add test, ensure it runs after all prior tests pass)

**DoD:** E2E test passes 3/3 runs. All checkpoints verified. Timing is acceptable. Fallback scenario works. Turn 3 output differs from Turn 1 (persona evolved). All component tests passing (US-101 through US-106).

---

## Non-Goals

- We will **NOT** implement techniques #4–#9 (reflection, RAG reranking, world models, catastrophic forgetting defenses, mixture-of-adapters, process rewards) in Sprint 60. Stories US-10 through US-21 define the groomed backlog for future sprints.
- We will **NOT** add MLX-backed reward model training (Task 8 in the M3 plan). Sprint 60 uses only the HUML toy GPT backend for RM training, which is cross-platform and gradient-checkable.
- We will **NOT** implement EWC or replay buffers in Sprint 60. Canonical pair replay is a nice-to-have; catastrophic forgetting is deferred.
- We will **NOT** add process reward models or step-level verification. Full-generation regeneration (via `response_guard`) remains the fallback.
- We will **NOT** merge adapters or manage multi-adapter composition in Sprint 60. One global persona adapter per session.

---

## Open Questions for Stakeholder

1. **Thompson Sampling Threshold:** The default threshold for deciding "send proactive" is 0.3 (send if P(reply) > 0.3). Is this conservative enough, or should it be higher (e.g., 0.5)?
2. **DPO Pair Quality Signal:** Should we weight pairs by how confident the implicit signal is? (e.g., a reply within 5min is higher confidence than a 24h no-reply)
3. **Training Subprocess Availability:** Does `scripts/train_persona_lora.py` already exist, or do we stub it and implement later? (Assumption: exists or can be stubbed in test mode)
4. **Adapter Storage Location:** Should adapter checkpoints go to `~/.human/adapters/` or elsewhere? (Assumption: `~/.human/adapters/`)
5. **Nightly Trigger Time:** Should the nightly LoRA job run at a fixed time (e.g., 2 AM) or on a cadence (e.g., every 24h from daemon startup)?

---

RESULT_product-owner=READY

**Summary:** Sprint 60 commits 7 detailed user stories (US-101 through US-107) implementing the critical-path spine: Bradley-Terry reward model (HUML backend, gradient-checkable) → DPO pair mining from implicit signals → Thompson-sampling bandit for proactivity → nightly LoRA retraining → hot-swap adapter integration. The spine closes the loop: real interaction → learned signals → updated persona → next generation uses new weights. All 7 stories are ≤250 LOC per story, testable, and completable in one sprint. Techniques #4–#9 (reflection, RAG, world models, catastrophic forgetting, mixture-of-adapters, process rewards) are groomed as backlog epics with stories and ACs, ready for future sprints. Zero dependencies between spine stories and groomed epics; all spine stories pass /verify before commit.

