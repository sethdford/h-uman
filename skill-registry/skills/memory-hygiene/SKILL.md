# Memory Hygiene

Forgetting on purpose is a feature, not damage. A memory store that only grows becomes noise within a year. Keep what compounds value, summarize what doesn't, drop what's stale.

## When to Use
- A memory or relation hasn't been read in 60+ days and has low recall_count.
- A cluster of similar low-importance facts has accumulated (e.g. 30 "discussed lunch with X" episodes).
- A fact is high-volume but low-distinctiveness ("user said hi").
- A fact has been superseded for long enough that the prior version is no longer informative.

## Workflow
1. **Score retention, don't binary-keep.** Importance × recency × recall_count × distinctiveness. Score below floor → candidate for action.
2. **Summarize before deleting.** A 20-message lunch conversation collapses into one episodic memory: "Recurring lunches with Alice, mostly at Café Zinc, mostly about her startup." Keep the summary, drop the verbatim turns.
3. **Decay weight, don't drop edges.** Lowering an edge's weight gracefully degrades retrieval ranking. Outright deletion creates phantom references in older summaries.
4. **Erase on user request, not on guess.** GDPR / Right-to-Forget is explicit user input; agent-side hygiene is implicit decay. Don't conflate them.
5. **Log every consolidation.** Forgetting silently is indistinguishable from a bug.

## Anti-patterns
- "I'll just delete anything older than X." Loses context that compounded value (the user's first big idea, an early friendship moment).
- Summarizing twice from already-summarized inputs (lossy compression cascade).
- Dropping low-confidence facts before AutoDream has had a chance to review them.

## Examples
**Example 1:** Episodic memory of grocery lists from 6 months ago, recall_count = 0, distinctiveness low → consolidate into one "weekly groceries, mostly Whole Foods, ~$120 average" summary; delete originals.

**Example 2:** A relation `works_at(Alice, Acme)` superseded 3 years ago, recall_count = 1 (asked once), low distinctiveness → keep, but downweight; don't delete (history still relevant for "where has Alice worked?").
