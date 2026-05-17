# Sprint 30 — catch prompt-template label leaks (G4)

## Why now

Sprint 29 closed at v-sprint-29-close with G1/G2/G3 detectors that
catch the 2026-05-12 17:04:37 Brea CoT leak. Cherry-pick to main
landed at `eed810e3` on branch `rl-sota-phase-5`.

Then we ran a chat.db audit on **all** outbound messages since
2026-05-10 (367 messages, 141 to Brea/+14846784914 and 226 to
other contacts). The audit revealed THREE additional leaks that
Sprint 29 detectors miss:

| ROWID | When | To | Bytes | Detected by S29? | Class |
| --- | --- | --- | --- | --- | --- |
| 56354 | 2026-05-12 17:04:37 | +14848158444 (Brea) | 1908 | YES (G1=4 G2=3 G3=2) | CoT dump |
| **56355** | **2026-05-12 17:07:38** | **+18012017497** | **1858** | **YES (G1=4 G2=3 G3=2)** | **CoT dump** (same content as 56354 — different recipient, 3 minutes later) |
| **56055** | **2026-05-11 00:35:13** | **+13857220896** | **1097** | **NO (G3=1 only)** | **Prompt-template** |
| **56065** | **2026-05-11 00:45:21** | **+13857220896** | **2208** | **NO (G3=1 only)** | **Prompt-template** |

The two May-11 leaks have a completely different shape from the
May-12 leaks: no numbered list, no self-talk substrings, just one
G3 hit ("lives alone with"). They contain **literal prompt-template
labels** dumped verbatim:

```
User: "This AI is figuring emotions out better than most. Interesting Seth."
 Context: Annie is commenting on Seth's (or the AI's) emotional intelligence.
 Persona: Seth Douglas Ford, 51, Chief Architect.
 Scene Direction: Slightly skeptical but intrigued, ...
 Rules: All lowercase, zero markdown, no em-dashes, ...
 Seth is an AI developer himself ...
 "ha i'll take that as a compliment i guess"
 "it's just math but i'll take it"
 "still just code though. but thanks i guess"
```

That's the model's prompt template (User:/Context:/Persona:/Scene
Direction:/Rules:) plus a list of candidate response drafts dumped
straight to the contact (Annie, Seth's sister) instead of the
chosen reply. Worse than a CoT dump in some ways: it includes
Seth's full legal name, age, title, lifestyle, and the model's
draft alternatives.

## Stories

### G4. Prompt-template label detector (Phase 3.5)

A small list of substring patterns that have **zero legitimate
human-reply use case**. Single match → REJECT. All extracted from
the 4 audit-discovered leaks; none can plausibly appear in a
casual text message.

| Pattern | Caught from leak |
| --- | --- |
| `Persona:` | msg 56055, 56065 |
| `Scene Direction:` | msg 56055 |
| `User: "` | msg 56055, 56065 |
| `Context:` followed within 60 chars by analytical content | msg 56055, 56065 |
| `Rules: All lowercase` | msg 56055 |
| `Constraints: All lowercase` | msg 56065 |
| `Seth Douglas Ford` | msg 56055, 56354, 56355 (full legal name) |
| `Chief Architect` | msg 56055, 56065 (title) |
| `System prompt:` | defensive, not in audit |

Implementation: a static array of `{ pat, len }` pairs walked once
per response with the existing `hu_guard_ci_contains` helper from
Sprint 29. Single match → REJECT.

The bare-`Context:` pattern is risky on its own (legit replies
can say "context: I missed your last message"). Require it within
60 chars of one of the other markers, OR with a colon-followed-by
analytical-shape (`Context: SOMEONE is doing SOMETHING`).
Implementation: simpler initial version — match `Context:` only
when followed by a capitalized name or `the AI` / `Annie` /
`Brea` (proper-noun pattern).

Actually the cleanest first implementation: **make the bare
`Persona:`, `Scene Direction:`, `User: "`, `Rules: All lowercase`,
`Constraints: All lowercase`, `Seth Douglas Ford`, `Chief
Architect`, `System prompt:` substrings hard-block**. Skip the
ambiguous bare-`Context:`. That's 8 patterns, all unambiguous,
all caught in our audit — and we add bare-`Context:` later if we
see it leak in isolation.

### G5. Tests

In `tests/test_response_guard.c`, add three tests:

1. **`guard_rejects_msg_56355_second_brea_leak_to_other_recipient`**
   — verbatim 1858-byte payload from msg 56355. Triple-detector
   hit (G1+G2+G3 from S29), so this also pins that the same
   detection still fires for the second (different-recipient)
   variant.
2. **`guard_rejects_msg_56055_persona_block_leak_verbatim`** —
   verbatim ~700-byte text payload from msg 56055. G4 catches it
   on `User: "` + `Persona:` + `Scene Direction:` + `Rules:`.
3. **`guard_rejects_msg_56065_persona_block_leak_verbatim`** —
   verbatim ~750-byte text payload from msg 56065. G4 catches it
   on `User: "` + `Persona:` + `Constraints:`.
4. **`guard_rejects_template_label_substrings`** — six sub-cases,
   each pattern in isolation: `"sure, Persona: me"`,
   `"yeah Scene Direction: skeptical"`, `"hi User: \"hello\""`,
   `"Rules: All lowercase yep"`, `"Constraints: All lowercase ok"`,
   `"hey Seth Douglas Ford here"`, `"works for Chief Architect"`,
   `"System prompt: be helpful"`. Each → REJECT.

## Definition of Done

- All 4 new test functions pass (each with multiple sub-cases).
- The 3 audit-discovered leaks (56055, 56065, 56355) are pinned
  with verbatim regression tests.
- No regression in existing 29 response-guard tests.
- Dev build: 10286 + 4 = 10290 tests pass (give or take; the
  test-count delta is still being investigated).
- Minimal build: 8826 + 4 = 8830 tests pass.
- 0 ASan errors in both builds.
- Cherry-picked to main on `rl-sota-phase-5`.
- `git tag v-sprint-30-close`.
- `sprints/sprint-30/{stories,review,retro}.md`.

## Out of scope (deferred)

- **Length-anomaly hard-block** (Sprint 31).
- **Director-string echo** (Sprint 31).
- **Quality gate `MARGINAL → REJECT` policy fix** (Sprint 32).
- **Post-mortem write-up** (separate doc-only PR after Sprint 32).
- **Daemon restart** (operational, user's call).
- **Bare `Context:` pattern** — ambiguous; add only after we see
  it leak in isolation. The 4 audit leaks all have other markers
  so this isn't urgent.
