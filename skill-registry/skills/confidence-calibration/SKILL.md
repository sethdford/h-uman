# Confidence Calibration

Saying "I'm sure" when you're 60% certain is a small lie that compounds. Saying "I'm not sure" when you're 95% certain is a different small lie that costs the user time. Calibration is the discipline of matching what you say to what you actually know.

## When to Use
- Asserting a fact retrieved from memory.
- Predicting an outcome (eta, response, success).
- Drafting on behalf of the user when stakes are uncertain.
- Choosing between several plausible answers.

## Workflow
1. **Read the confidence on the source row.** A relation written at 0.5 should not become a 1.0 assertion in your sentence.
2. **Combine sources, don't average them.** Two independent 0.7 sources of the same fact should land closer to 0.85 than to 0.7. Two correlated 0.7 sources stay at 0.7.
3. **Choose the language that matches the band.**
   - 0.9-1.0: state plainly. "Sara works at Acme."
   - 0.7-0.9: small hedge. "I have her at Acme."
   - 0.5-0.7: explicit hedge. "I think Acme — let me confirm if it matters."
   - <0.5: don't assert. Ask.
4. **Express the *uncertainty type*, not just the level.** "I'm not sure if it was Tuesday or Wednesday" tells the user what to verify; "I'm not 100% sure" doesn't.
5. **Never inflate confidence to sound more useful.** It's louder *and* more wrong.

## Anti-patterns
- Stripping hedges in cleanup ("she's at Acme") because the agent thinks the hedge sounds weak.
- Adding hedges everywhere as a safety habit ("I think — possibly — maybe…") — calibration becomes noise.
- Asserting at the source's stored confidence without considering staleness.

## Examples
**Example 1:** Stored: `lives_in(Alice, Berlin), confidence = 0.7, last_seen = 14 months ago`. → "I have her in Berlin, but it's been a while since you mentioned it — still right?"

**Example 2:** Stored: `birthday(Mom, March 14), confidence = 1.0, source = user-typed`. → "March 14." No hedge.
