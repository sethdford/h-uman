# Beyond the September-2026 frontier — plan (2026-09-19)

Successor to `2026-09-06-better-than-human/README.md` (items 1-8 there stay the
program; this file says what "ahead of the papers" means per item and what
shipped today). Every paper below was fetched and verified on 2026-09-19.

## The unfair advantages nobody in the literature has

1. **One real person, one real corpus, real raters who know him.** Every paper
   measures on synthetic personas (PM-Bench, LoCoMo, StateMemBench) or
   crowd raters. Our gate is friends texting Seth's number.
2. **The reply loop is in production.** A claim about memory is checked against
   what the contact actually said next, not a held-out split.
3. **Seth's own later behaviour is a free ground-truth verifier.** If Seth
   (the human) later mentions a fact, the insight was right. No paper has a
   verifier that is the target person.
4. **Always-on nightly GPU** (reflection fix freed ~11 GPU-h/day) with a C
   daemon that can log a shadow metric at any seam for one line of code.

"Beyond" therefore means: take each paper's mechanism, close its loop with
production evidence, and gate it on the human/Seth-relative measurement.

## Per-item: frontier → what ships → what "beyond" is → gate

| # | Frontier (paper) | Shipped / status | Beyond | Gate |
|---|---|---|---|---|
| A | Prospective memory as a typed intention store, lifecycle in code, 82.9% Set-F1 on PM-Bench (PIS 2609.01272) | Same design, built 09-13, **deployed 09-19** (PR #419): fire-once, whole-word cues, one cue per intention, `[prospective] fired N of M` | (1) **Closure detection**: retire an intention when a later inbound shows it was fulfilled, not only when its cue fires. (2) **Time cues**: `expires_at` becomes a *proactive* prospective ("it's been two weeks since her interview") through the contextual-proactive path that is already live. (3) **Acknowledged-fire rate**: did the contact respond to the surfaced reminder? A live PM metric no benchmark has. | `fired=1` rows > 0 within 7 days; acknowledged-fire rate reported on the nightly card |
| B | "Ghost memory": outdated / current / transitional facts coexisting (A-TMA 2607.01935) | State-first resolver renders was/until (09-05, 12/12 held-out) | Add the **transitional** state: a `plan`-kind insight or an open prospective intention marks a relation *in flux* ("interviewing at publix" → rendered as pending until "started at publix" supersedes it). Extend the held-out changed-facts set from 3 moments to 4 (the transitional moment). | 4-moment chains retrieved at the right version; no regression on the 24×3 set |
| C | Memory-write admission: repeated self-consistency before any write (ConsistencyGate 2607.22962); persist verifier signals as metadata against duplicate drift (MemGuard 2608.21867) | **Built 09-19** (this PR): generate once, verify K=3 with shuffled order, confidence × agree/K, agreement persisted in `source` (`extractor:v2:k3:a2`). Live dry-run: 7/8 admitted, 1 rejected. Regenerate-and-intersect was tried first and admits 0/8 (documented in code). | **Seth-as-verifier**: an insight is *confirmed* when a later message authored by Seth (human, not daemon) references it; store `confirmed_by_id`. Rejected candidates become hard negatives for the extractor prompt bank. | nightly line: written / rejected / confirmed-by-Seth counts; confirmed share rising week over week |
| D | Persona nodes tied to event evidence by provenance edges so a persona claim retires with its fact (PGMem 2608.01708) | **Built 09-19**: `evidence_ids` (messages.id) per insight, `as_of_ms` = newest evidence date, `--retire-superseded` (K-verified pairs + newer-by-evidence guard, `superseded_by_id`), shadow by default | Wiki pages inherit evidence and drop retired notes; doctor ratchet **insights-without-evidence → 0** as old rows age out; supersession goes live after 3 nightly dry-runs with zero wrong refusals on review | `SELECT COUNT(*) FROM contact_insights WHERE evidence_ids IS NULL AND created_at_ms > <deploy>` = 0 |
| E | Persona overuse is systematic regardless of context; PAS appropriateness metric (SCONPOS 2609.04676, EMNLP 2026) | next | **Deterministic per-reply shadow metric**: tokens from the injected insight block that surface in the reply, split by whether the inbound touched them. Then the closed loop PAS cannot have: `HU_INSIGHT_MAX_ITEMS` adapts to the overuse rate. Baseline is **Seth's own** unprompted-memory rate from the corpus, so the target is "as name-droppy as Seth", not "less". | overuse rate on the nightly card; live ≤ Seth's own ± 0.05 |
| F | Fidelity = task framing + interpersonal stance + linguistic style (PRISM 2608.26674) | style (LUAR at the cross-author floor) + anti-AI + relationship register measured; stance not | Stance = the **emotion-register axis already in shadow** (rule 14; Seth card n=299, JSD vs twin) made direction-aware and promoted to a card axis. PRISM judges stance with an LLM; ours is a distribution distance to the real person. | emotion card n≥100 per rule-14 gate; stance JSD on the card; rule 14 shadow→live only if the human gate holds |
| G | Fine-tuned personas are MORE detectable by surprisal variability (DivEye 2509.18880); Raschka detector-from-scratch (Aug 15) | **Built 09-19** (PR #420): std / burstiness / kurtosis of the per-token surprisal series for both observers, AUCs on the nightly summary | Use it on the **generator side**: candidate adapters must match Seth's `div_std` distribution (two-sample KS) before promotion — variability parity as a promotion gate, which no detection paper enforces. Feed `div_*` into `binoculars_to_dpo` pair mining. | `diveye_auc.std` on the nightly summary; KS p > 0.05 for a promoted candidate |
| H | Weight-space drift control for nightly LoRA (SDC-LoRA, ACL 2026 Findings) | **not adopted** — needs per-layer SVD of the dequantized base; not feasible nightly for a 4-bit 106B MoE in a 90-min window. Also the 09-18 anti-AI dip is NOT a retrain: the served adapter is unchanged since 09-05 (every candidate staged, `promotion_gate=BLOCK`) | **Output-space drift control** instead: held-out SimPO loss (live) + LUAR twin distance + DivEye parity (G) + classifier gate + human gate. Promotion needs all five. | candidate promoted only with all five green; doctor `blind_ab_gate` clears |
| I | Serving: Qwen3.8-Flash-Next (125B/6B active, native MTP), MTPLX weekly (v2.11.3, +27% decode) | not run | One raw-base probe in the 02:00-05:00 training window (prod GLM + Flash-Next do not co-reside in 128 GB). No mlx-community 4-bit; community mixed-4/8-bit exists (ddalcu, 08-27). Same driver and criterion as the failed Qwen3.8-27B probe. | lower \|d−0.5\| AND ≤2 "lol"/160 vs GLM; a pass still needs a human sheet |
| J | Industry: Managed-Agents "Dreaming" (plain-text consolidation between sessions); Mem0 6.9K tokens/query | wiki-nightly (05:20) is our dreaming, provenance-tagged and linted; reflection healthy | Report **memory tokens per turn** and prompt_trim bytes on the nightly card; `HU_WIKI_HEAD` shadow→live only when bytes drop and composite holds | prompt bytes/turn down with composite flat |
| K | Simulate ONE evaluator from their past judgments (PersonaJudge, SIGDIAL 2026) | not built; the human gate itself is stalled (0/48 answered since 09-06, four rows skipped after three asks) | Seth-judge trained on the ~76 rated trials as the daily proxy — **requires Seth to answer the drip first**; nothing downstream can be trusted while the verdict is 52 days old | human verdict age < 45 days; Seth-judge agreement with Seth ≥ 0.8 on held-out rows |

## Order of work

1. Deploy #419 (done 09-19), plist `HU_WIKI_HEAD=shadow`, gate A/J readings for 7 days.
2. Merge this PR; nightly extractor runs `--consistency-k 3` and `--retire-superseded` (dry-run) → C/D gates.
3. E: shadow overuse metric in the daemon (one log line), aggregate to the card.
4. Seth answers the drip → K unblocks → H's five-gate promotion can actually fire.
5. B (transitional state), F (stance axis), G (parity gate), I (probe) in that order.

## Do NOT
Another voice round; steering; Q6; base-swap *program* (one probe only); weight-space drift control on this hardware.
