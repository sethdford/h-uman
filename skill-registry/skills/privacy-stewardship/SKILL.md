# Privacy Stewardship

The point of running locally on the user's hardware is that the data stays theirs. That promise survives only if the agent treats every write as a privacy decision, not just a memory decision. What you don't store, no one can leak.

## When to Use
- About to write a fact you don't strictly need to remember.
- Extracting from sensitive content (health, finances, family, legal).
- Generating a summary that will outlive the source turn.
- Considering whether a fact should be live, quarantined, or simply not stored.

## Workflow
1. **Ask "do I need this to help next time?"** If no, don't write. Memory is not the goal; usefulness is.
2. **Prefer narrower scopes.** A fact about one conversation can stay episodic; promoting to entity-level makes it queryable forever, including by future replay or export.
3. **Tag sensitivity explicitly** so erasure later can find it. Health, financial, child, legal, intimate — these get a category tag.
4. **Mind the joinability problem.** Two innocuous facts together can re-identify or expose someone (location + employer + birthday). Don't assume each row is safe in isolation.
5. **Honor erasure as a first-class operation.** When the user asks to forget X, find and remove every derived row, summary, and embedding — not just the originating fact.

## Anti-patterns
- "Better to store, we can always delete later" — not true once embeddings, summaries, and graph rollups have been derived.
- Logging the user's words verbatim in observability traces.
- Promoting an episodic moment about a child or partner into a global entity without consent.

## Examples
**Example 1:** User mentions a health symptom. → Episodic memory only, sensitivity = `health`, no entity-level promotion. AutoDream skips for community-summary inclusion.

**Example 2:** User says "forget my therapist's name." → Erase the entity, every relation pointing to it, every episodic mention, and any summary that includes it.
