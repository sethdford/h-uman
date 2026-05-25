# US-16 — Director Context-Bleed Root Cause + Fix

**Date:** 2026-05-26
**Mindy diagnostic reference:** Sprint 55, 2026-05-25 16:41 — director output
`"Casual, slightly sheepish, confirm it's working but keep it brief so he can
get back to the drink"` produced for contact Mindy (+18012017497), where "the
drink" originated in a different contact's (+447914633409) conversation.

**Verdict:** **LLM hallucination** — NOT data bleed. Fix shipped as a
director_system prompt hardening (no architectural change required). The
US-12 runtime trace (already shipped) will validate the diagnosis next time
the bug recurs.

## Investigation steps + evidence

### Hypothesis 1: Cross-contact data in `entries` (SQL filter bleed)

**Ruled out.** The iMessage channel's `load_conversation_history` SQL query
at `src/channels/imessage.c:2068` filters strictly by handle:

```sql
WHERE h.id = ?1 AND m.associated_message_type = 0
```

The `?1` parameter is bound from the contact_id argument at line 2082.
There is no possible code path where messages from contact A's handle
appear in contact B's `entries` array.

### Hypothesis 2: Cross-contact data in personal_model.bin (stored memory)

**Ruled out by direct inspection.**
```bash
$ strings ~/.human/personal_model.bin | grep -i drink
(empty)
```

The personal_model.bin does NOT contain "drink" as a stored fact. So even if
the director's prompt did include personal_model context (which it doesn't —
see Hypothesis 3), the leak couldn't come from there.

### Hypothesis 3: Cross-contact data in director's `user_buf` (prompt assembly)

**Ruled out by static analysis.** The director's user_buf is built ONLY from:

1. Static header `"Recent thread:\n"`
2. Last 5 `entries` (which are per-contact per Hypothesis 1)
3. The current `combined` message

See `src/daemon.c:392-410`. No personal_model, no world_model, no other
context source is concatenated into the prompt. The Sprint 55 background
investigator confirmed this independently.

### Hypothesis 4: Gemini prompt-cache returning prior responses

**Ruled out — technically impossible.** Gemini's prompt caching only
speeds up prefill of identical prefixes. It does NOT return previously-generated
responses. Different user messages always run the model fresh, regardless
of cache state. The earlier investigator who proposed this was incorrect
about Gemini's caching semantics.

### Hypothesis 5: LLM hallucination by Gemini Flash-Lite (the director classifier)

**SUPPORTED — leading explanation.**

The director_system prompt (`daemon.c:355-391`) primes the model with
detailed Seth lifestyle context: "45yo tech entrepreneur, lives alone with
his cat, kids don't live with him." It asks the model to output a
`direction:` field describing tone + reason for brevity.

When Mindy texted "So is it working now," the inbound was casual + brief.
The model:
1. Classified tone as "casual, slightly sheepish"
2. Was prompted to explain WHY Seth would be brief
3. Filled in a plausible-but-fabricated reason ("get back to the drink")
   drawing on stereotype, NOT on the user_buf content
4. Output that fictional specifc reason

The downstream agent treats the `direction:` text as ground truth and pastes
invented specifics into the actual reply — which manifests as cross-contact
"bleed" even though it's actually hallucination.

## Fix shipped (commit at HEAD)

`src/daemon.c:383-391` — added an explicit instruction to the director_system
that the `direction:` field describes TONE + PACING only, and must NEVER
invent specific reasons, activities, people, or topics:

```
- CRITICAL: The `direction:` field describes TONE + PACING only. NEVER invent
  specific reasons, activities, people, places, or topics that aren't visible
  in the Recent thread. Forbidden: 'because he's getting back to the drink',
  'mention the cat', 'reference yesterday's meeting'. Allowed: 'short
  empathetic reaction', 'casual, match their energy', 'busy tone, one-word
  reply'. The downstream agent treats your `direction` text as ground truth
  and will paste invented specifics into the actual reply, which manifests
  as cross-contact bleed (US-16, Mindy diagnostic 2026-05-26).
```

## Validation plan

1. **Immediate:** the US-12 runtime trace (already shipped, commit `76419353`)
   captures the EXACT director input + output per turn when
   `HU_DIRECTOR_TRACE=1` is set.

2. **Next time the bug appears to recur:** operator runs daemon with
   `HU_DIRECTOR_TRACE=1`, captures the trace, and checks:
   - Does the leaked text appear in the INPUT line? → Hypotheses 1-3 are
     wrong, leak IS in prompt assembly somewhere we missed
   - Only in the OUTPUT line? → Confirmed hallucination, this fix should
     prevent recurrence

3. **Long-term:** if hallucination recurs even with the hardening, escalate
   to one of:
   - Switch director model to a less hallucination-prone variant
   - Structured output enforcement (JSON schema validation on `direction:`)
   - Add a post-LLM validator that rejects `direction:` strings containing
     concrete nouns absent from the user_buf

## Status

- **Root cause:** identified — LLM hallucination, not data bleed
- **Fix:** shipped via director_system prompt hardening
- **Validation:** awaits next recurrence + US-12 trace evidence
- **Confidence:** MEDIUM — hardening should prevent the most common
  hallucination pattern; structural enforcement (e.g. validate direction:
  against tokens in user_buf) is the higher-confidence follow-up if needed
