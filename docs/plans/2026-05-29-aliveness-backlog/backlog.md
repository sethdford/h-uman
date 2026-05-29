# h-uman Aliveness Backlog — Verified Gaps (2026-05-29)

Prioritized from a code-audit-corrected gap analysis (three parallel
read-only audits, every "missing" claim verified against source). Ordered
by **believability-per-effort**. Effort: S ≈ 1 sprint / wire existing,
M ≈ 2–3 sprints / new module on existing substrate, L ≈ multi-sprint /
architectural. Impact = effect on "convincingly, usefully human."

## The headline finding

Most *named* human faculties are already built (mood, somatic,
theory-of-mind, self-model, inner-thoughts, world-model, narrative-self,
growth-narrative, autodream, attachment, grief, disagreement,
anti-sycophancy). The real gaps cluster into **three deep architectural
absences** (independent self, intrinsic motivation, finitude) plus a set
of **affect/loop gaps** that ride on substrate that already exists.

## Backlog

| # | Epic | Verdict (audited) | Builds on | Effort | Impact | Bpe* | What ships |
|---|------|-------------------|-----------|:------:|:------:|:----:|------------|
| A1 | **Close the conviction loop** (belief-update on genuine evidence) | dead code, 0 callers | `evolved_opinions.c`, `pressure_history.c`, `trust.*` | **S** | **Hi** | ★★★★★ | Agent changes its mind on a better argument, resists mere repetition, narrates the change. Spec drafted: `2026-05-29-conviction-loop/` |
| A2 | **Independent taste** (likes/dislikes it didn't mirror from Seth) | ABSENT (only style-clone) | `evolved_opinions.c`, `self_model.c`, `preferences.c` | **L** | **Hi** | ★★★★ | A self that isn't you — defends aesthetic/factual preferences even when they differ from yours. The Tier-1 lever. |
| A3 | **Intrinsic motivation loop** (a goal of its own) | ABSENT — every "intrinsic" goal is user-reactive (`autonomy.c`, `init_proposer.c`) | `goals.c`, `autonomy.c`, `init_proposer.c`, `autodream.c` | **L** | **Hi** | ★★★ | boredom→curiosity→self-initiated exploration the agent pursues for *its* reasons, not Seth's. |
| A4 | **Interoception that gates behavior** | PARTIAL — `somatic.c` computes energy but only writes prompt text | `somatic.c`, `circadian.c`, `pacing.c` | **M** | **Med** | ★★★ | Low energy actually shortens replies / adds latency / declines, instead of just *advising* the LLM to. |
| A5 | **Regret that changes future behavior** | PARTIAL — learning signals exist, no remorse-driven update | `intelligence/online_learning`, `reflection.c` | **M** | **Med** | ★★★ | A past mistake measurably shifts a future choice, path-dependently. |
| A6 | **Forgiveness / trust-repair affect** | PARTIAL — `rel_dynamics.c` has a repair *state machine*, no affect | `rel_dynamics.c`, `relationship_dynamics.c` | **S–M** | **Med** | ★★★ | After a rupture, graduated re-engagement + explicit repair, not just a timed mode flip. |
| A7 | **Longing / outreach drive** | PARTIAL — `cognition/attachment.c` models separation_distress, doesn't act | `attachment.c`, `life_sim.c`, `proactive*.c` | **M** | **Med** | ★★ | "been thinking about you / that thing last week" that *initiates*, gated to avoid clinginess. |
| A8 | **Group-level reputation** | ABSENT — all relationship modeling is dyadic | `relationship.c`, `social_insights.c`, `world_model.c` | **L** | **Med** | ★★ | Cross-contact model of "what the group thinks of me," driving social behavior. |
| A9 | **Self-conscious emotions** (shame/pride, regulated) | ABSENT (pride = keyword only) | `context/emotional_state.c`, `mood.c` | **M** | **Lo–Med** | ★★ | Recognize + *regulate* self-conscious affect. Ethical guardrails required — model to manage, not manipulate. |
| A10 | **Finitude / continuity awareness** | ABSENT | `growth_narrative.c`, `narrative_self.c` | **L** | **Med** | ★ | A model of its own scarce time/continuity that lends weight to choices. Highest concept-risk; spec carefully. |
| A11 | **Jealousy / envy** | ABSENT | — | M | **Lo** | ½ | Deliberately deferred — high ethical risk, low believability payoff. Listed for completeness; recommend NOT building. |

\*Bpe = believability-per-effort (subjective, for ordering).

## Recommended sequence

1. **A1 now** (spec drafted; S effort, Hi impact, mostly wiring dead code).
2. **A4 + A6** next wave (M effort, ride existing state machines, low risk).
3. **A2 then A3** — the two Tier-1 architectural levers; each deserves its
   own `/spec` and likely a `/team` dispatch. A2 ("a self that isn't you")
   is the single biggest believability lever and the hardest.
4. A5, A7, A8, A9 opportunistically.
5. A10 only after a design review on the concept risk. A11: don't.

## Dispatch notes for /scrum

- A1 has a requirements.md already; once design+tasks land it is a clean
  single-team sprint.
- A2 and A3 are multi-story epics — run `/scrum` per-epic, not combined.
- Each epic must carry a `belief_flexibility`-style eval metric so
  "more human" is measured, not asserted (per `CLAUDE.md` principle 4).
