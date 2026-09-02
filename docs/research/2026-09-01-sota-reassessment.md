---
title: "h-uman SOTA reassessment — 2026-09-01"
created: 2026-09-01
status: snapshot
audience: maintainers, research
---

# h-uman SOTA reassessment — 2026-09-01

Successor to `2026-07-25-sota-gap-analysis.md`. That document's #1 finding was "the
measurement loop is the biggest gap, and it's internal." This pass confirms it, and finds
the same shape one level down: **most of what we believed about the running system was a
report, not a measurement.** Every number below was re-derived from artifacts on
2026-09-01 — chat.db, memory.db, the daemon log, launchd, the registry — not from
config flags or log lines that say "sent" / "live" / "PASS".

Confidence: local state is verified. External claims cite arXiv/vendor sources and are
marked; blog-sourced numbers are directional.

## TL;DR — five things that change the picture

1. **The reactive product has had almost no exposure, ever.** `allow_from` has been
   exactly one handle in every config backup since May. This month: **1,494 inbound from
   121 handles, 0 from the allowlisted one, 0 reactive replies.** The 57 daemon sends were
   proactive check-ins to three family members. Every quality judgement of "the
   assistant's replies" — including the 08-02 "nowhere near me" — was made on that.
2. **The knowledge graph has never been fed.** `causal_nodes = 0` in the live DB *and* in
   the DB it replaced. No caller of `hu_world_model_record*` exists outside its own file.
   `HU_GRAPH_GROUNDING=live` reads an empty structure by construction; the live fact
   extractor's 38 merges went to a 64-slot C struct (`personal_model.bin`), never the graph.
3. **The memory store was discarded on 08-04 as "corrupt" and it isn't.** The quarantined
   file passes `PRAGMA quick_check` = ok today: 1,074 memories, 2,996 opinions, 962
   dpo_pairs — all recoverable. The daemon has run for a month on 37 / 68 / 138.
4. **Self-measurement has been dark since 08-07/08.** Humanness verdicts stop 08-07,
   eval-archive stops 08-08, registry last eval 08-08. Cause: the machine was mostly
   asleep and all three nightlies are calendar-only. Separately, nightly LoRA has failed
   *every* night since it was enabled: the probe parses a `"pairs"` JSON key the miner
   never prints — and the "probe" is a writer.
5. **The serving queue is ~99.8% internal machinery.** 27,336 conversational-shaped
   generations this month vs 57 real sends. Judges, arena self-play, proposers and the
   intelligence cycle own the GPU the product is supposed to be using.

## Part 1 — Where we actually are (verified 2026-09-01)

| Axis | Believed | Measured | Evidence |
|---|---|---|---|
| Adapter | v6-orpo-real live since 08-02 | true — but **never human-rated**; gate still 07-29 n=40 on the v5 arm; smoke 14/16 vs v5 15/16 | registry, `blind_ab_gate.json` |
| Reactive exposure | "serving Seth's real iMessages" | **one allowed handle, 0 msgs from it this month**; 121 senders dropped at trust gate | chat.db ROWID>67977 join handle; config backups |
| Proactive | live, 30-min tick | 57 sends to 3 family handles; **filler bank fired 21×** (3 canned strings, no cap) | daemon log; `daemon_routing.c:102` |
| Excluded contact | — | **correctly protected**: 287 silent drops + 31 pre-send aborts; the 8 from-me rows are Seth | daemon log, decoded chat.db |
| Memory store | 966 memories, dedup'd opinions | **replaced 08-04**; quarantined copy healthy & recoverable; live DB 37/68/138 | `memory.db.corrupt-1785839267` quick_check=ok |
| Graph | GraphRAG live | **0 nodes, no ingestion path**; grounding 0/16 (was 19.5%) | `causal_nodes` both DBs; grep callers |
| Fact extraction | live, populating | runs (87), merges (38) → 64-slot struct; yield **0.62 facts/msg** on clean own-messages (measured), ~792 over history | `eval_fact_extract_yield.py` |
| Semantic retrieval | keyword only | Phase 1 proven: **+30 pts recall@5** over FTS5 (0.20→0.50), MRR 0.150→0.422, n=20/962; sqlite-vec vendored | `phase1-results.json` |
| Preference training | ORPO "over-corrected" (May) | **stock mlx-lm-lora ORPO is a zero-gradient no-op** (forward pass outside `value_and_grad`); patched; v6 margin −0.255→+0.285 | 60-s repro, `lora_b` 0/28→28/28 |
| Nightly LoRA | enabled 08-02 | **0 runs**: probe/miner contract mismatch | `lora_retrain_probe_failed {unparseable_pair_count}` nightly |
| Humanness nightly | daily PASS ~0.90 | **silent since 08-07**; last verdicts were `SKIP` with a composite number attached | verdict files; `pmset -g log` |
| Product gate (eval-nightly) | measures the product | last ran 08-08; blind-ab **SKIPPED** (gateway) — gateway is up now | `eval-nightly.log` |
| Harness identity | fixed 07-27 | fixed 08-02 (`bbe7e2a1e`): it had been scoring "Vanguard / King of Prussia / 45" | `eval_blinded_ab.py` |
| Opinions faucet | fixed by scoping | scoping was insufficient; **UNIQUE index fixed it** (+1 / 8 min); `current_events` still 2.3× | `check-silent-success.sh` baseline |
| Latency levers (07-25 #2) | not taken | **taken**: `prompt_cache_lcp: live`, 3 slots; compact head live (~5 KB) | config, `dump_prompt_head` |

## Part 2 — What SOTA looks like now, by axis

**Memory with time.** Zep/Graphiti's bi-temporal edges carry `t_valid/t_invalid` (world
time) and `t_created/t_expired` (system time); a contradicting fact *invalidates* the old
edge rather than deleting it. On LongMemEval that lifts gpt-4o from 60.2% → 71.2%, with
temporal reasoning 45.1 → 62.4 and knowledge-update 78.2 → 83.3
([arXiv 2501.13956](https://arxiv.org/abs/2501.13956)); Zep beats Mem0 63.8 vs 49.0 on the
same benchmark ([Mem0 report](https://mem0.ai/blog/state-of-ai-agent-memory-2026)).
HorizonBench shows frontier models pick *outdated* preferences ~60% of the time
([2604.17283](https://arxiv.org/abs/2604.17283)). **This is the exact shape of 3 of the 9
human detections at n=40** — "done moving all settled in now", "last day's friday" — and
h-uman's fact tables carry no validity window. Four INTEGER columns on SQLite.

**Evaluating human-likeness.** The Computational Turing Test finds calibrated LLM output
*remains* distinguishable by classifiers even when humans can't tell, most strongly on
affective tone, and reports a human-likeness ↔ semantic-fidelity trade-off
([2511.04195](https://arxiv.org/html/2511.04195v2)); CT2 detectors reach F1≈1.0
([2605.20761](https://arxiv.org/abs/2605.20761)). PRISM reframes persona fidelity as
structured inference over Task Framing / Interpersonal Stance / Linguistic Style to avoid
holistic-judge hallucination ([2608.26674](https://arxiv.org/abs/2608.26674), EMNLP 2026).
PersonaArena still sets κ≥0.6 vs humans as the bar for any LLM judge. Implication: our
0.225 human PASS and a classifier-based measure are *different questions*; we should run
both, and we already ship a Binoculars scorer.

**Persona fidelity training.** SFT and vanilla DPO increase drift at scale; ORPO/KL-anchored
objectives mitigate ([2601.12639](https://arxiv.org/abs/2601.12639)). Contrastive
persona-consistency training ([2503.17662](https://arxiv.org/html/2503.17662v1)),
CharLoRA-style adapters that don't disturb base knowledge, and Persona-Pruner
([2606.14695](https://arxiv.org/html/2606.14695v1)) are the 2026 shapes. A 2026 line claims
training-free personality control via approximately orthogonal activation-space vectors
([2602.15669](https://arxiv.org/pdf/2602.15669)) — note this contradicts our CLOSED steering
verdict; that verdict was on gemma and GLM-shape mismatch, so it's untested here, not
refuted.

**Learning from users online.** RLUF trains a `P[Love]` reward from emoji reactions — i.e.
*tapbacks* — and folds it into multi-objective policy optimisation
([2505.14946](https://arxiv.org/abs/2505.14946)); Apple's P-GRPO handles heterogeneous
preferences ([Apple ML](https://machinelearning.apple.com/research/personalized-group));
OFS-DPO streams preferences ([2406.05534](https://arxiv.org/abs/2406.05534)). h-uman already
collects tapbacks (`reaction_collection.enabled=true`); the missing piece is a loop that runs.

**Extracting knowledge with small local models.** Constrained decoding buys syntax, not
content ([JSONSchemaBench 2501.10868](https://arxiv.org/abs/2501.10868)); large schemas
hurt ([ExtractBench 2602.12247](https://arxiv.org/html/2602.12247)). The frugal-KG paper
takes local Gemma-4 from F1 0.039 → 0.702 with an explicit relation list + prohibition of
`none`, adds +2 with self-consistency and +9 with a low-agreement cascade, and warns that
*high* consensus can be collective hallucination ([2604.11104](https://arxiv.org/pdf/2604.11104)).
Small domain ontologies work for personal KGs ([2607.00003](https://arxiv.org/abs/2607.00003)).
Our measured predicate drift (10/18 off-vocabulary → 83.9% in-vocab with a closed list)
is this paper's finding reproduced on our data.

**Proactive timing.** Pare-Bench (143 scenarios): best systems ~42%; good timing looks like
a *low* proposal rate with *high* acceptance (Claude 12.8% / highest), bad timing like
Gemma's 74.7% premature-proposal rate ([2604.00842](https://arxiv.org/html/2604.00842v1)).
We cannot compute either number: the proactive path logs "sent" for undelivered messages
(fixed 08-02) and the send-recency/bandit tables were trained on those phantom sends.

**On-device retrieval + serving.** sqlite-vec (pure C, in-process, exact KNN) fits a 10^4
corpus; uSearch's HNSW only pays past ~10^6 ([sqlite-vec](https://github.com/asg017/sqlite-vec),
[uSearch](https://github.com/unum-cloud/USearch)). nomic-embed via MLX; MLX ~50% faster than
llama.cpp on embedding (prefill-bound) workloads
([Contra Collective](https://contracollective.com/blog/local-embeddings-apple-silicon-nomic-bge-qwen3-m5-max-2026)).
Both are vendored/installed here.

## Part 3 — Gaps, ranked by leverage

Each item names the measurement that would prove it worked. No item is "done" without it.

1. **Open the allowlist (product decision, not code).** One handle since May means every
   downstream judgement — v5 vs v6, "sounds like me", the proactive tone — rests on ~57
   messages/month to three relatives. Proof: reactive reply count > 0 for >1 contact, and a
   cycle-5 sheet drawn from *those* replies.
2. **Restore the quarantined store and fix the false-corrupt path.** `quick_check` is only
   run after an unclean shutdown, treats any non-"ok" row as corruption, has no retry, and
   quarantines a WAL-mode DB whose checkpoint may simply have been pending. Proof:
   memories back to 1,074+, and a test that a busy/locked DB is *not* quarantined.
3. **Give facts a validity window (Zep bi-temporal on SQLite).** `valid_from / valid_to /
   created_at / expired_at` on facts + opinions; supersede by invalidating. This is the
   fix for the event-state detections, and it's four columns. Proof: a held-out set of
   "changed" facts (moving date, last day) retrieved at the right version.
4. **Feed the graph from the backfill, with the three filters.** Exclude reactions
   (`associated_message_type≠0`, 10.2%, the mental-health mis-attribution case), drop
   daemon-authored text, validate predicates against the closed list. ~792 facts ≈ 1.4
   years of live accrual. Proof: `causal_nodes > 0` and grounding match rate > 19.5%.
5. **Semantic retrieval Phase 2** — `store_sqlite_vec.c`, an embeddings endpoint on the
   server the daemon already calls, hybrid rank (semantic *lost* one probe keyword won),
   gated `off|shadow|live`. Proof: Phase-1 harness re-run on the live path.
6. **Make self-measurement robust to a sleeping laptop.** `StartInterval` fallback or
   run-on-wake; and `SKIP` must not emit a composite number. Proof: verdict files dated
   every day for a week.
7. **Fix nightly LoRA's probe.** Read the count from SQLite directly; never run a writer as
   a probe. Proof: one nightly run that reaches training and asserts `lora_b ≠ 0`.
8. **Cap the filler bank.** Per-contact cooldown + draw from the *measured* rate (0.29%),
   not a 3-string table. Proof: bank share of daemon sends < 1%.
9. **Record reactive sends with a `message_ref`** as `proactive_sends` already does.
   Without it, daemon output is inseparable from Seth's typing in chat.db, which poisons
   backfill and every corpus built from it.
10. **Starve the machinery.** Judges/arena/proposer/intelligence own 99.8% of the queue.
    Move them to a spare port or a schedule that yields to live traffic. Proof: p50 TTFT
    on a real reply while the arena runs.
11. **Add a classifier-tier eval beside the human gate** (Binoculars is in-tree). Proof:
    AUC on the n=40 pairs; disagreement with humans is signal, not noise.
12. **Drift monitor** (fixed probe prompts embedded nightly; 07-25 #6, still open).
13. **`current_events` UNIQUE index** — 280k / 121k distinct, baselined not fixed.

## Part 4 — Where we are at or ahead (don't spend here)

- **Real-person twin with a working reference-free preference round.** After the ORPO
  patch, v6 is the first adapter in this lineage where the objective demonstrably engaged
  (margin crossed zero, val acc 1.0). No shipped competitor does per-person preference
  training on-device.
- **Measurement discipline is now enforced, not aspirational.** `check-silent-success.sh`
  (pre-commit), `lora_b≠0` asserted at train and at registration, refuse-don't-fallback in
  every harness, the harness scoring the production head. These are ahead of what the
  papers describe for eval hygiene.
- **Proactive safety.** Excluded-contact handling and "real user active → abort" both
  verified working under load (287 / 31 events). Pare-Bench's failure mode is
  over-messaging; ours is under-delivery, which is the safer side to be wrong on.
- Blind A/B protocol, compact head + prefix caching, GLM base choice, anti-tell
  sanitisation — unchanged from 07-25; still aligned.

## Part 5 — Recommended sequence

1. Allowlist decision (yours; 0 code) → 2. restore store + fix false-corrupt (S) →
3. bi-temporal columns (S) → 4. backfill with filters, graph fed (M) → 5. retrieval
Phase 2 (M) → 6/7 nightlies robust + LoRA probe (S) → 8/9 filler cap + reactive
`message_ref` (S) → then a cycle-5 sheet drawn from real reactive replies, rated by you.

Nothing in 2–9 needs a new model. The frontier gap is not the adapter; it's that the
adapter has had nothing to retrieve, almost no one to talk to, and no working instrument
watching it.

## Part 6 — Corrections to what we believed

- "Grounding is live and will fill" → it reads a graph with no ingestion path.
- "The store shrank" → it was replaced; the original is intact and passes integrity.
- "The nightlies pass" → they stopped; the last outputs were SKIPs carrying numbers.
- "Nightly LoRA is on" → it has never trained; the probe can't parse the miner.
- "The daemon messaged an excluded contact" → it did not; my regex matched batch lines.
- "The extractor yields 1.38 facts/msg" → 0.62 once reactions and inbound are excluded.
- "The ORPO negative result was tuning" → it was a zero-gradient bug in the trainer.
- "Sub-linear retrieval needs ANN" → exact KNN is sub-ms at our corpus size.

## Addendum — outage found during this review (2026-09-01 20:35–20:41)

While re-deriving the numbers above, production went down and was restored:

- `mlx-server` died with **`LastExitStatus = 9` (SIGKILL)** and was revived by
  KeepAlive at 20:31:21. `service-loop` logged normal work at 20:35:29, then was
  **booted out of launchd entirely** (job absent, plist intact and enabled, binary
  unchanged at `3cbda5499`, no `.staged-*` leftover, auto-updater aborted on
  integrity as usual). This session issued no `bootout`. The sequence — serving
  stack torn down, daemon unloaded, no restore — matches a peer session's
  deploy/training window whose restore step did not complete; four other
  claude-code processes were running. Attribution needs `sudo log show`.
- Restart was **gated** on the live store: `PRAGMA quick_check = ok`, WAL 0 B, so
  the unclean-shutdown `quick_check` path could not quarantine it. Bootstrapped at
  ~20:41; verified same inode (148374916), memories 52 → 52, gateway :3006 up,
  both gates live in the process environment.
- This is the single-owner problem from `session-worktree-isolation.md` made
  concrete: two sessions' prod-stop traps are not aware of each other. A
  `bootout` that is not paired with a verified `bootstrap` in the *same* trap
  leaves production dark until someone notices. Recommendation #6 (robust
  self-measurement) would have caught it in minutes; tonight it was caught by a
  sanity check at the end of an unrelated task.

Also confirmed for recommendation #2: the quarantined store has 102 tables vs the
live 93 (the live one lacks `embeddings`, `relational_episodes`, `eval_*`,
`hula_tasks`, `ab_tests`, `celebrations` — created lazily by subsystems that have
not run since). A straight swap-back would lose the 52 memories written since
08-04, so the restore is a **merge** (old ← new deltas), not a swap.

## Sources

arXiv 2501.13956 (Zep/Graphiti) · 2604.17283 (HorizonBench) · 2601.02845 (TiMem) ·
2605.01386 (MemORAI) · 2511.04195 (Computational Turing Test) · 2605.20761 (CT2) ·
2608.26674 (PRISM) · 2605.17044 (PersonaArena) · 2601.12639 (SFT/DPO drift) ·
2503.17662 (persona-aware contrastive) · 2606.14695 (Persona-Pruner) · 2602.15669
(personality vectors) · 2505.14946 (RLUF) · Apple P-GRPO · 2406.05534 (OFS-DPO) ·
2501.10868 (JSONSchemaBench) · 2602.12247 (ExtractBench) · 2604.11104 (frugal KG) ·
2607.00003 (PKG triples) · 2604.00842 (Pare-Bench) · asg017/sqlite-vec ·
unum-cloud/USearch · Mem0 "State of Agent Memory 2026" · Contra Collective embeddings
benchmark (blog; directional).
