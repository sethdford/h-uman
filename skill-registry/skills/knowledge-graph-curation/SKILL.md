# Knowledge Graph Curation

Turning conversation into a graph well is half engineering, half editorial. Default to fewer, sharper edges over many fuzzy ones. The graph the agent reasons over tomorrow is whatever you let in today.

## When to Use
- About to extract entities or relations from a turn, document, or tool output.
- Multiple plausible relation types could fit the same observation.
- A new entity name is similar but not identical to one already in the graph (alias vs duplicate).
- A fact is observed but its time-window is unclear ("she works at..." — since when?).

## Workflow
1. **Name the smallest fact.** Single subject, single predicate, single object. If you can't, you're trying to write two facts.
2. **Choose a relation type** from the closed vocabulary (`works_at`, `lives_in`, `knows`, `family_of`, `interested_in`, ...). If nothing fits, prefer not writing over inventing.
3. **Pick the time window**, not just the moment. `event_start` = when the fact became true. `event_end = 0` = "still true." Recording an unknown start as `now` is a known lie; prefer leaving it 0.
4. **Set confidence honestly.** 1.0 is reserved for things the user typed verbatim. Inferred facts start at 0.7. Facts from open web start lower.
5. **Write the provenance.** A graph row without provenance is unauditable later. Use `<channel>:<turn-id>` or a stable URL.
6. **Resolve aliases up front.** "Alex" and "Alexander Smith" likely point to the same entity. Look up before inserting.

## Anti-patterns
- Writing `related_to` because the relationship feels true but you can't name it.
- Backfilling `event_start = now` for a fact the user said happened "years ago."
- Inserting one entity per spelling variant (creates duplicate communities later).

## Examples
**Example 1:** "She moved to Berlin in 2023." → `lives_in(Sara, Berlin), event_start = 2023-01-01 00:00, confidence = 0.9, provenance = imessage:turn-4812`. Do **not** write `lives_in` with `event_start = now` — that's a different fact.

**Example 2:** Web article says "Alice works at Acme." → confidence 0.5, provenance is the URL. The conflict resolver may flag it against a higher-confidence user-typed fact; that's the point.
