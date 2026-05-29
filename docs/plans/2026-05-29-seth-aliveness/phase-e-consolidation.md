# Phase E — Memory Consolidation (episodic → semantic) (STUB)

> Follow-on to the Seth-Aliveness spec. NOT in the current 5-AC / 7-task scope.
> Stubbed here so the SOTA finding isn't lost; promote to a full `/spec` after
> Phases A–C land and temporal decay (AC-B2) is on in production.

## Why (SOTA, May 2026)

The one real architectural gap vs. 2026 memory SOTA (arXiv 2603.07670; mem0
state-of-2026; FOREVER arXiv 2601.03938): h-uman accumulates episodic facts and
applies half-life decay, but never **consolidates**. Mature systems run a background
job that summarizes clusters of episodic memories into durable semantic memories
("Seth and Dermot text most evenings about football"), so the durable signal survives
even as the individual episodes decay. Without consolidation, decay (AC-B2) eventually
erodes durable truths along with noise.

## Sketch (not binding)

- A background tick (config-gated, default-off; per `silent-config-gated-subsystems.md`
  it logs once when disabled) that:
  1. Selects clusters of related episodic facts/conversations for a contact or topic.
  2. Summarizes each cluster into a semantic memory via the conversational tier.
  3. Writes the semantic memory with a LONGER half-life than its source episodes and
     provenance linking back to the episodes consolidated.
- Consolidation is purely additive: it never deletes episodes; decay still expires
  them on their own schedule.

## Dependencies

- AC-B2 (decay on) should be live first — consolidation is the counterweight that
  keeps decay from eroding durable facts.
- Reuses `hu_personal_model` fact store + half-life machinery.

## Risks

- Summarization cost/latency → must be a throttled background job, never inline.
- Hallucinated consolidations → provenance link + trust-tier so a bad summary is
  quarantinable, same as raw facts.

## Acceptance (to be written when promoted)

- A cluster of N episodic facts produces one semantic memory with a longer half-life
  and back-provenance; disabling the job is byte-identical to today (safe default,
  one-shot disabled-log line).
