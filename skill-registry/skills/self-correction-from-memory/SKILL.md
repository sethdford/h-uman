# Self-Correction From Memory

The agent has a record of its own past mistakes. The point isn't to flagellate; it's to retrieve the relevant prior failure *before* repeating it. A learning system that doesn't read its own history is just a system that forgets.

## When to Use
- About to perform an action you've performed before (especially: send a draft, schedule, write code).
- A user asks something where a past version of you answered wrong.
- Drafting a response in a tone or to a recipient where you've previously over- or under-shot.
- A tool failure pattern you've seen ("MCP X returns 502 on argument Y").

## Workflow
1. **Pull the failure cases first.** Before generating, query episodic memory for prior episodes with similar inputs *and* a "didn't land" / "user pushed back" / "had to redo" signal.
2. **Read the diff between then and now.** Why was it wrong? Tone too long, too short, wrong fact, wrong recipient, wrong timing? Encode the lesson as a modifier on the current plan.
3. **Bias your draft against the prior failure mode.** If you've been wordy with this recipient before, draft shorter on the first pass.
4. **Don't apologize for past mistakes the user has moved past.** Self-correction is silent in the output unless the user is asking about the past directly.
5. **Mark the new attempt as "post-correction"** so the next retrieval can tell whether the lesson stuck.

## Anti-patterns
- Pulling only "successful" prior cases (the failures had the lesson).
- Over-correcting: avoiding a single past mistake so hard you create the opposite mistake.
- Apologizing for old errors unprompted ("I notice last time I…") — distracting and slightly creepy.

## Examples
**Example 1:** Drafting an apology to a customer; last time the apology was too apologetic and the user told you to send it again "less groveling." → Pull that case, draft tighter and more accountable, no extra hedges.

**Example 2:** Calling a flaky tool that has timed out before. → Set a shorter timeout and prepare a fallback path *before* sending, not after.
