# Case-Based Reasoning

When a current task resembles ones you've handled before, retrieve the prior case and adapt — don't replan from scratch. Most decisions in a long-running assistant are not novel; they are variations of decisions already made well or already made badly.

## When to Use
- Planning a task with a verb the agent has executed before ("draft a release notes," "schedule a 1:1").
- Choosing tone for a known recipient or channel.
- Estimating time or risk for a similar past project.
- Recovering from an error that has occurred before.

## Workflow
1. **Generate the retrieval key.** Goal verb + key entities + channel/contact + (optionally) constraints. Encode it.
2. **Search the episodic memory** for top-K nearest cases. Cross-graph: an event in 2023 may include the contact graph (recipient), entity graph (project), and emotional graph (how it landed).
3. **Read the prior outcome, not just the prior plan.** A plan that *failed* is often more informative than one that succeeded — it tells you what to avoid.
4. **Adapt, don't copy.** Identify the deltas: different recipient? different deadline? different context? Apply minimal changes to the recovered plan.
5. **Record the new case** so the next retrieval has one more data point.

## Anti-patterns
- Copying the prior plan verbatim and ignoring the deltas (cargo-culting).
- Retrieving only "successful" cases. The failure cases often hold the lesson.
- Treating cases as a free-form text blob; without typed structure (verb, entities, outcome), retrieval becomes vague.

## Examples
**Example 1:** User asks to draft a release-notes email. → Retrieve last 3 release-notes drafts, note that the September one was praised for its candor about a regression. Mirror that tone; adapt to current release content.

**Example 2:** Calendar conflict for a 1:1 with Sara. → Last conflict was resolved by offering two new slots in async DM, not via meeting reschedule. Reuse that pattern.
