# Community Summarization

For "global" questions about a person, project, or theme, naive retrieval pulls a handful of edges and misses the gestalt. The right answer is the cluster summary — the writeup of an entity community produced during consolidation — not raw turns.

## When to Use
- The user asks an open question about a person or topic ("how have things been with Sara?").
- A retrospective or check-in across a long period.
- Drafting an introduction, reference, or thank-you that needs more than one anecdote.
- The query touches many small facts that are individually trivial but cumulatively meaningful.

## Workflow
1. **Detect the global shape.** Pronouns, possessives, theme words ("the project," "your year"). If retrieval would need 50+ rows to cover, you want a summary.
2. **Look for the precomputed community summary first.** AutoDream produces them during idle cycles; they're cheaper than synthesizing on the hot path.
3. **If no summary exists, run a one-shot consolidation.** Score the top-K most-recalled, highest-weighted edges in the cluster and summarize. Cache the result.
4. **Cite, even at the cluster level.** A summary without provenance becomes hallucination on revisit.
5. **Refresh when stale.** Summaries older than a month or behind a wave of new edges should be invalidated.

## Anti-patterns
- Summarizing on every query (latency cost).
- Summarizing without writing back the summary (next query repeats the work).
- Letting one outlier turn dominate the summary because it was emotionally loaded.

## Examples
**Example 1:** "How are things with the design team?" → Pull the design-team community: members, recent decisions, mood arc, open threads. One paragraph, ~300 words, with two or three concrete anchors.

**Example 2:** "Tell me about my mom's hobbies." → Cluster is small but specific; precomputed summary exists. Read directly, augment with anything new since `last_summarized`.
