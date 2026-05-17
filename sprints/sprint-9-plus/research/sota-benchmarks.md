# SOTA Persona / Digital-Twin Benchmarks — May 2026

**Question:** What benchmarks does the field use to rank persona-faithful generation, and which can h-uman defensibly target to claim "SOTA digital twin"?

**Bottom line:** The 2025-vintage role-play benchmarks (PersonaChat, RoleBench, CharacterEval, InCharacter, PersonaGym) are **all surface-style or fictional-character benchmarks**. The 2025-Q4 / ACL-2026 generation pivoted to **real-person digital twins with held-out behavioral prediction**: **TwinVoice**, **YNTP-100**, and **Twin-2K-500**. That is where h-uman should plant the flag.

---

## 1. Benchmark landscape (May 2026)

| Benchmark | Year | What it scores | Gameability | SOTA holder |
|---|---|---|---|---|
| **PersonaChat** | 2018 | Persona consistency in dialog (5-sentence personas) | High — sentence-overlap, lexical | Saturated; not used for ranking |
| **RoleBench / RoleLLM** | 2024 ACL | 100 fictional characters, instruction-following in role | Style mimicry; rewards token-overlap | RoleLLaMA / RoleGLM ~ GPT-4 parity |
| **CharacterEval** | 2024 | 77 Chinese-novel characters, 13 metrics, CharacterRM reward model | Reward-model gameable; fictional | GPT-4 + tuned 13B models |
| **InCharacter** | ACL 2024 | Personality fidelity via psych interviews (Big Five / 14 scales) | Susceptible to "personality prompting"; no real users | GPT-4 (~80% personality match) |
| **PersonaGym** | EMNLP 2025 Findings | 200 personas × 5 tasks (Action Justification, Expected Action, Linguistic Habits, Persona Consistency, Toxicity) | Dynamic, harder; still synthetic personas | Claude 3.5 Sonnet / GPT-4.5 — and notably they **fail to beat GPT-3.5** on persona consistency, suggesting persona quality ≠ raw capability |
| **RVBench / RPEval** | 2025 | Values + emotion + moral + in-character consistency | Better-scoped but small | GPT-4-class |
| **Twin-2K-500** | 2025 (Marketing Science 2026) | 2,058 real people, 500 Qs, replication of behavioral-econ experiments | Hard to game — predicts real held-out human choices | GPT-5 / Claude-4-Sonnet-Thinking / DeepSeek-R1 reach **moderate** behavioral alignment; gap remains vs. human test-retest |
| **YNTP-100** | 2025 (arXiv 2510.14398) | "Your Next Token Prediction": 100 real users, multi-day, English/JP/CN, predict day-N+1 token-by-token from prior history | Very low — true held-out next-token on real user history | Frontier models reported; no method closes gap to human baseline |
| **TwinVoice** | ACL 2026 Findings (arXiv 2510.25536) | 2,500 multilingual tasks, 3 persona dimensions (Social / Interpersonal / Narrative), 6 capabilities (opinion consistency, memory recall, logical reasoning, lexical fidelity, persona tone, syntactic style) | Distractor-driven multi-choice; lexical-mimicry alone fails | Advanced models reach "moderate accuracy" but **fall short on syntactic style and memory recall**; below human baseline |

**Key red-team finding** (from the field, May 2026): persona prompting yields **stylistic but not behavioral** differences ([arXiv 2512.06867](https://arxiv.org/html/2512.06867v1)). This is the exact thesis h-uman's "lexical fingerprint is gameable" critique embodies — and it is now a citable result.

## 2. In-context vs. in-weights — quantitative comparisons

- **CharLoRA** ([Beyond Profile, ACL 2025 Findings](https://aclanthology.org/2025.findings-acl.1094/)) is the strongest published prompt-vs-LoRA contrast: a structured LoRA decomposition (general linguistic matrix + task-specific matrices + shared knowledge matrix) trained on Lu Xun. Beats prompt-only and vanilla-LoRA baselines on multiple-choice QA, generative QA, and style transfer. **The takeaway is structural:** persona-specific weight updates beat prompting on tasks that require *cognitive* consistency, not just style.
- **P-Tailor** ([arXiv 2406.12548](https://arxiv.org/html/2406.12548v1)) — MoE of LoRA experts indexed by Big-Five traits. Shows in-weights personality is composable; prompting is not.
- **LoRA Without Regret** (Thinking Machines Lab, 2026): LoRA recovers 90–95% of full-FT performance at a fraction of memory. Standard reference for the in-weights side.

## 3. Behavioral consistency & held-out prediction

These are the two benchmark families that **cannot be gamed by a lexical fingerprint**:

- **Held-out next-token / next-utterance**: YNTP-100 (real users, multi-day) and Sim4IA-Bench ([arXiv 2511.09329](https://arxiv.org/html/2511.09329), next query/utterance prediction).
- **Held-out behavior**: Twin-2K-500 (predicts a real person's answer to a question they have never seen, given their other 499 answers). This is the digital-twin equivalent of held-out classification accuracy.

## 4. Emergent-behavior gap

Multiple 2025-2026 papers converge on the same negative result: **persona adaptation improves tone and lexical fidelity but does not transfer to decision-making, planning, or strategic reasoning** ([Do Persona-Infused LLMs Affect Performance in a Strategic Reasoning Game?, arXiv 2512.06867](https://arxiv.org/html/2512.06867v1); TwinVoice's "memory recall" and "opinion consistency" dimensions). This is the honest ceiling on what an h-uman LoRA can claim, and it is consistent with our M3 stance.

## 5. Recommendation for Sprint 9

**Target: YNTP-100 + a Twin-2K-500-style held-out subset.**

Rationale:
1. **Not gameable by lexical-surface fingerprints** — the task is next-token / held-out-answer on real users, scored against ground truth. Our current synthetic fingerprint cannot fake this.
2. **Public datasets** ([HuggingFace LLM-Digital-Twin/Twin-2K-500](https://huggingface.co/datasets/LLM-Digital-Twin/Twin-2K-500); YNTP-100 release on arXiv).
3. **Frontier models do not saturate** — there is real headroom for an on-device LoRA to show a measurable delta vs. base.
4. **Aligns with M3 thesis**: privacy-by-architecture personalization should show up as held-out user prediction improvement, not as style mimicry.

### Recommended Sprint-9 evaluation harness

- **Primary metric: held-out next-utterance log-likelihood** on a 10-user subset of a YNTP-100-style corpus we generate from real h-uman session transcripts (with user consent). LoRA-adapted base vs. base+prompt-only vs. base alone.
- **Secondary metric: behavioral held-out accuracy** — 50 forced-choice questions per twin in a Twin-2K-500-style format; predict the held-out 10 from the other 40. LoRA must beat prompt-only by ≥1 stderr at n≥10 users.
- **Tertiary (TwinVoice-style) capability decomposition**: report opinion consistency, memory recall, persona tone, syntactic style separately. This prevents a single composite from hiding regressions.
- **A/B harness**: reuse `scripts/ab_eval_30.py` skeleton; replace lexical fingerprint with token log-prob and forced-choice accuracy.

### What to publish

A short technical note titled *"h-uman LoRA improves held-out next-utterance log-likelihood by X% over base + system-prompt persona on a YNTP-100-style protocol (n=10 users, p<0.05)"* is a credible, falsifiable, citation-worthy claim. *"SOTA digital twin"* without a held-out prediction number is not.

---

## Sources

- [PersonaGym (arXiv 2407.18416)](https://arxiv.org/html/2407.18416v4) — dynamic persona-agent evaluation, 200 personas × 5 tasks.
- [RoleLLM / RoleBench (arXiv 2310.00746)](https://arxiv.org/abs/2310.00746) — 100 roles, RoleLLaMA / RoleGLM.
- [CharacterEval (arXiv 2401.01275)](https://arxiv.org/html/2401.01275v1) — Chinese RPCA benchmark, 4-dim / 13-metric.
- [InCharacter](https://incharacter.github.io/) — personality fidelity via psych interviews.
- [TwinVoice (arXiv 2510.25536)](https://arxiv.org/abs/2510.25536) — ACL 2026 Findings; 2,500 tasks, 3 persona dims, 6 capabilities.
- [YNTP-100 (arXiv 2510.14398)](https://arxiv.org/abs/2510.14398) — Your Next Token Prediction, 100 real users multi-day.
- [Twin-2K-500 (arXiv 2505.17479)](https://arxiv.org/abs/2505.17479) — 2,058 real people, 500 Q digital-twin dataset.
- [Beyond Profile / CharLoRA (ACL 2025 Findings)](https://aclanthology.org/2025.findings-acl.1094/) — structured LoRA for deep persona.
- [P-Tailor (arXiv 2406.12548)](https://arxiv.org/html/2406.12548v1) — MoE-of-LoRA Big-Five personality.
- [Do Persona-Infused LLMs Affect Strategic Reasoning? (arXiv 2512.06867)](https://arxiv.org/html/2512.06867v1) — persona prompting changes style, not behavior.
- [Sim4IA-Bench (arXiv 2511.09329)](https://arxiv.org/html/2511.09329) — next-query/utterance simulation benchmark.
- [LoRA Without Regret (Thinking Machines Lab, 2026)](https://thinkingmachines.ai/blog/lora/) — LoRA recovers 90–95% of full-FT.
- [Twin-2K-500 dataset on HuggingFace](https://huggingface.co/datasets/LLM-Digital-Twin/Twin-2K-500) — public release.

---

RESULT_research=READY
