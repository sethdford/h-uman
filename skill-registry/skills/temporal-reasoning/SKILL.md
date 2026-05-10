# Temporal Reasoning

The graph stores when a fact was *true* (event-time) separately from when you *learned* it (ingest-time). Most "what was true on date X?" questions need the former; most audit questions need the latter. Confusing them is the most common subtle wrong answer in assistant memory.

## When to Use
- The user asks about a window: "during 2024," "last quarter," "before we started Project Atlas."
- A fact has changed and the user is asking about the old version: "where did Sara work before?"
- Reconstructing a sequence of decisions or moves.
- Reconciling a contradiction by figuring out which fact came first in the world (not just first in the database).

## Workflow
1. **Reach for the right axis.** Window queries → event-time. "When did I find out?" → ingest-time. They are different columns; using the wrong one is silently wrong.
2. **Treat `event_end = 0` as "still true," not "ended at 0."** The query layer expects this convention.
3. **For half-open windows, the cutover instant belongs to the *next* window.** A relation that ended at exactly 2025-01-01 00:00 does not overlap "during 2025."
4. **Normalize relative time before querying.** "Last summer," "before her wedding," "around the move." Convert to a concrete `[from_ts, to_ts]` first, then query.
5. **Surface uncertainty.** If the timestamp is approximate, say so in the response — not "in 2023" if you only know "around 2023."

## Anti-patterns
- Using `last_seen` as a proxy for "when was this true?" — it's an ingest signal, not an event signal.
- Returning the most recent matching row when the question wants the *historical* row.
- Silently downcasting "since spring 2024" to a single point.

## Examples
**Example 1:** "Where did Alice work during 2024?" → Window `[2024-01-01, 2025-01-01)`, event-time overlap. Returns Acme (closed at 2025-01-01) only if it overlaps strictly before that instant.

**Example 2:** "When did I tell you about my new job?" → Ingest-time query. Look at `first_seen` on the relation, not `event_start` of the job itself.
