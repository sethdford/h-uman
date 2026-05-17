# Design for US-7.9 (P2): Constitutional style self-critique at generation time

**Risk tier:** MEDIUM (touches agent response path).
**Sprint base sha:** `13b89763`.
**Decisions referenced:** D4 (per-process one-shot warning pattern).

---

## 1. Approach

A new pure-C pattern matcher (`src/persona/style_critique.c`) scans the **final draft string** against `agent->persona->style_rules[]` *after* the LLM call resolves but *before* the response leaves `hu_agent_turn`. The matcher is **literal** — case-insensitive prefix or substring, no regex, no wildcards. Each rule string is parsed once into a tiny in-memory `hu_style_rule_t` that carries (a) the original rule text (for logging / "rule name"), (b) the extracted pattern (the quoted phrase, if present), and (c) a match-kind (`PREFIX` or `SUBSTRING`). Rule-text parsing recognises the two patterns implied by AC-7.9.1 — `"never start with '<X>'"` → PREFIX on `<X>`; everything else (e.g. `"no em-dashes"` → keyword `em-dash`/`—`) → SUBSTRING. A short curated alias table inside `style_critique.c` maps human phrases (`"em-dashes"`, `"emoji"`, `"exclamation marks"`) to the literal sequence(s) to search for; if a rule doesn't match any known phrase the matcher treats the **last quoted span** as the SUBSTRING needle, and if there is no quoted span it falls back to the rule text itself (best-effort). All comparisons via `tolower((unsigned char)c)` over ASCII — no locale, no UTF-8 case-folding. Word-boundary anchoring is applied to the PREFIX kind (skip leading whitespace, then `strncasecmp` against the needle followed by either end-of-string or a non-alpha/digit char) to prevent the "Sure!" vs "We're sure!" false positive called out in the risk section.

The critique hook sits **inside the existing constitutional block** in `src/agent/agent_turn.c` near line 5372 (the `if (agent->constitutional_enabled)` clause). We do not add a new top-level block — we add a *sibling* call to `hu_style_critique_run` immediately after the LLM-judge `hu_constitutional_critique` returns, gated by the new `agent->cfg.agent.constitutional_style_rules_enabled` flag (default `false`, AC-7.9.4). The current constitutional block is wrapped in `#ifndef HU_IS_TEST` — the new style-rule block is **not** wrapped in that guard, because it does not call out to the network. It does, however, run only when `style_rules_count > 0` and the flag is true; otherwise it short-circuits without touching `final_content`.

The one regeneration is performed by calling `chat_with_system` a second time via the same provider vtable that the original draft used, with an **augmented system prompt suffix** of the form: `"\n\nIMPORTANT: do not begin your response with \"Sure!\". Do not include em-dashes. ..."` (one line per violating rule, only the rules that actually fired). This is the same provider call signature already used in the response_guard retry path (`include/human/agent/response_guard_retry.h`) — we reuse that pattern, not invent a new one. If the regenerated draft still violates *any* rule, we keep the second draft (best effort, AC-7.9.3), call `hu_log_info("style_critique", agent->observer, "style_rule_violation_unresolved rule=\"%s\"", ...)`, and bump a per-process counter test-visible behind `HU_IS_TEST` for AC-7.9.3 assertion.

---

## 2. Existing-code interface notes

| Symbol | Where | Notes |
|---|---|---|
| `char **style_rules; size_t style_rules_count;` | `include/human/persona.h:401-402` | NUL-terminated UTF-8 strings, owned by `hu_persona_t`. Already loaded from JSON. **No new persona fields needed.** |
| `bool constitutional_ai;` | `include/human/config.h:103` | Existing flag controls the **LLM-judge** critique (the current `hu_constitutional_critique` call). **We do not reuse it** — style-rule self-critique gets its own key to keep the two independently togglable. |
| `bool constitutional_enabled;` | `include/human/agent.h:432` | Mirror flag on the agent struct, fed from `cfg.agent.constitutional_ai`. We add a parallel `bool style_rules_enabled;` (or feed via cfg pointer — see file plan below). |
| Constitutional hook site | `src/agent/agent_turn.c:5372-5396` | Inside `hu_agent_turn`, after the LLM resolves `final_content`/`final_len`, before the metacognition loop (5398) and before history append / response_out write. **This is the seam.** |
| `chat_with_system` provider call | Used throughout `agent_turn.c`, e.g. line 7158 | Vtable signature on `hu_provider_t`. The regen uses this directly with a synthesised system prompt rather than going through `prompt_build_with_cache` (the system context is unchanged; we are only appending a constraint suffix). |
| `hu_observer_t` / structured events | `include/human/observer.h` | Observer is a **typed enum tag** vtable — not free-form strings. AC-7.9.3 asks for a `style_rule_violation_unresolved` event but the observer doesn't have an existing tag for it. **Plan:** emit via `hu_log_info("style_critique", agent->observer, ...)` with the rule name embedded in the message and a `HU_IS_TEST`-visible counter `s_style_unresolved_count` for the test assertion. Adding a new `hu_observer_event_tag_t` enum value is out of scope for this story (would force a cross-cutting change to every observer impl). |
| Config trio | `src/config_parse_agent.c:18-19`, `src/config_merge.c:217`, `src/config_serialize.c` | `constitutional_ai` is parsed/merged/serialized in this trio. We add `constitutional_style_rules_enabled` adjacent to each existing site. |

**Pinned types for the matcher:**

```c
/* style_critique.h — internal types */
typedef enum {
    HU_STYLE_MATCH_PREFIX = 0,    /* must NOT start with needle */
    HU_STYLE_MATCH_SUBSTRING = 1, /* must NOT contain needle anywhere */
} hu_style_match_kind_t;

typedef struct {
    const char *rule_text;     /* borrowed, points into persona->style_rules[i] */
    size_t rule_text_len;
    const char *needle;        /* borrowed if from rule text, owned if from alias table */
    size_t needle_len;
    hu_style_match_kind_t kind;
    bool needle_owned;         /* free needle on cleanup if true */
} hu_style_rule_t;
```

Compiled rule set is built once per critique call (max ~16 rules in practice). No heap alloc for the rule array itself — a stack-allocated `hu_style_rule_t rules[32]` is sufficient; rules beyond 32 are silently dropped and a `hu_log_warn` is emitted (D4-style one-shot warning).

---

## 3. Concrete file plan

| Action | File | Estimated LOC |
|---|---|---|
| ADD | `src/persona/style_critique.c` | +240 |
| ADD | `include/human/persona/style_critique.h` | +50 |
| MODIFY | `src/agent/agent_turn.c` (insert hook after line 5396) | +35 |
| MODIFY | `include/human/config.h` (add `bool constitutional_style_rules_enabled;` to `hu_agent_config`) | +1 |
| MODIFY | `src/config_parse_agent.c` (parse new key) | +3 |
| MODIFY | `src/config_merge.c` (default false at line 217 area) | +1 |
| MODIFY | `src/config_serialize.c` (write key, matching neighbours) | +3 |
| MODIFY | `src/config_schema.c` (register new key, if schema file lists agent flags) | +3 |
| MODIFY | `include/human/agent.h` (add `bool style_rules_enabled;` to agent struct OR thread cfg pointer) | +1 |
| MODIFY | `src/agent/agent.c` (wire cfg → agent at construction time) | +3 |
| MODIFY | `CMakeLists.txt` (add new source files) | +2 |
| ADD | `tests/test_style_self_critique.c` | +260 |
| ADD | `tests/test_style_critique_patterns.c` | +180 |

**Public header (`include/human/persona/style_critique.h`):**

```c
hu_error_t hu_style_critique_check(const char *draft, size_t draft_len,
                                   char *const *style_rules, size_t style_rules_count,
                                   const char **violated_rule_out,        /* borrowed */
                                   size_t *violated_rule_len_out);

/* Returns HU_OK if no rule violated (violated_rule_out set to NULL),
 * HU_OK with violated_rule_out non-NULL if a rule fired.
 * Returns HU_ERR_INVALID_ARGUMENT on NULL draft. */

/* HU_IS_TEST counters — reset between tests */
#ifdef HU_IS_TEST
extern int hu_style_critique_test_unresolved_count;
extern int hu_style_critique_test_check_invocations;
void hu_style_critique_test_reset(void);
#endif
```

The orchestration (call provider, augment prompt, rerun, decide best-effort) lives at the call site in `agent_turn.c`, **not** in `style_critique.c`. The library file is pure string matching plus the rule parser. That keeps the matcher trivially unit-testable without a provider mock (AC-7.9.5 → `tests/test_style_critique_patterns.c`).

---

## 4. Test plan

### `tests/test_style_critique_patterns.c` (pure, no provider) — AC-7.9.5

| Test | Pattern | Input draft | Expect violated |
|---|---|---|---|
| `test_prefix_sure_fires` | `"never start with 'Sure!'"` | `"Sure! Here you go."` | yes |
| `test_prefix_sure_word_boundary` | `"never start with 'Sure!'"` | `"Surely yes."` | **no** (word-boundary, addresses risk #3) |
| `test_prefix_sure_leading_whitespace` | `"never start with 'Sure!'"` | `"  Sure! ..."` | yes (skips leading WS) |
| `test_substring_em_dash` | `"no em-dashes"` | `"yes — agreed"` (U+2014) | yes |
| `test_substring_em_dash_clean` | `"no em-dashes"` | `"yes - agreed"` (hyphen) | no |
| `test_case_insensitive_prefix` | `"never start with 'sure!'"` | `"SURE! yo"` | yes |
| `test_emoji_alias` | `"no emoji"` | `"ok 👍"` | yes (alias table) |
| `test_empty_rules_no_match` | `[]` | anything | no |
| `test_null_draft_returns_invalid_arg` | any | `NULL` | `HU_ERR_INVALID_ARGUMENT` |
| `test_unknown_rule_phrase_falls_back_to_quoted_span` | `"avoid the phrase 'as an AI'"` | `"As an AI, I..."` | yes |

### `tests/test_style_self_critique.c` (mock provider) — AC-7.9.1 / 2 / 3 / 4

Uses a deterministic mock provider whose `chat_with_system` returns a queue of fixed strings. The mock counts invocations.

| Test | AC | Setup | Assertion |
|---|---|---|---|
| `test_sure_prefix_triggers_regen` | 7.9.1 | mock returns `"Sure! Hi"` then `"Hi"`; rules `["never start with 'Sure!'"]`; `style_rules_enabled = true` | response_out == `"Hi"`; mock invocation count == 2; `unresolved_count == 0` |
| `test_em_dash_triggers_regen` | 7.9.1 | mock returns `"yes — ok"` then `"yes - ok"`; rules `["no em-dashes"]` | response contains no `—`; invocations == 2 |
| `test_clean_draft_no_regen` | 7.9.2 | mock returns `"Hi there."`; rules `["never start with 'Sure!'", "no em-dashes"]` | invocations == 1; `check_invocations == 1` |
| `test_max_one_regen_on_persistent_violation` | 7.9.3 | mock returns `"Sure! a"` then `"Sure! b"`; rules `["never start with 'Sure!'"]` | response_out == `"Sure! b"`; invocations == 2 (NOT 3); `unresolved_count == 1`; log line containing `style_rule_violation_unresolved` and `never start with 'Sure!'` |
| `test_critique_disabled_short_circuits` | 7.9.4 | `style_rules_enabled = false`; rules `["never start with 'Sure!'"]`; mock returns `"Sure! hi"` | response_out == `"Sure! hi"`; invocations == 1; `check_invocations == 0` |
| `test_no_rules_short_circuits` | 7.9.4-adjacent | `style_rules_enabled = true`; `style_rules_count == 0` | invocations == 1; `check_invocations == 0`; D4-style one-shot warning emitted (verify via log capture) |

All tests use `HU_IS_TEST` guards and the standard `human_tests` harness. Each test calls `hu_style_critique_test_reset()` at start.

### AC traceability

| AC | Test |
|---|---|
| 7.9.1 | `test_sure_prefix_triggers_regen`, `test_em_dash_triggers_regen` |
| 7.9.2 | `test_clean_draft_no_regen` |
| 7.9.3 | `test_max_one_regen_on_persistent_violation` |
| 7.9.4 | `test_critique_disabled_short_circuits` |
| 7.9.5 | All of `tests/test_style_critique_patterns.c` (≥10 patterns, covers the AC's "≥5 patterns" bar) |

---

## 5. Risks (top 3)

### R1 — Doubled inference cost on every turn that violates (MED probability / MED impact)
**Failure mode:** Every reply containing an em-dash triggers a regen. A chatty persona could spend 30%+ extra tokens.
**Mitigation:** (a) Default OFF (`constitutional_style_rules_enabled = false`, AC-7.9.4); (b) the check runs ONCE per turn — no inner loop; (c) docs note that aggressive rules ("no exclamation marks") will materially increase cost. We do **not** add a cost cap in this story (YAGNI — the existing token budget machinery already covers it). Tracking-only: add a `hu_log_info` line at regen-fire so operators can grep the daemon log.

### R2 — Regen loops produce identical output (HIGH probability if mitigation absent / MED impact)
**Failure mode:** Calling `chat_with_system` a second time with identical system prompt + identical user message yields the **same** response from a deterministic provider (or one with `temperature=0`). The single regeneration is wasted.
**Mitigation:** The regen system prompt is augmented with an explicit constraint suffix listing **only the rules that fired**, e.g. `"\n\nIMPORTANT: rewrite the previous answer. Do not begin with \"Sure!\". Keep the same meaning."` This forces a different token distribution. The mock provider in tests proves this works at the *call-site contract* level (mock returns different strings on call 1 vs 2). For real providers, AC-7.9.3 explicitly handles the "still failed" case — the design **does not promise** that one regen always succeeds.

### R3 — Pattern matcher false positives via naive substring (MED probability / SMALL impact, but user-visible)
**Failure mode:** Rule `"never start with 'Sure!'"` naively implemented as `strstr` would fire on `"We're sure!"`, `"Make sure you turn it off."`, etc. User loses trust in the feature.
**Mitigation:** (a) PREFIX kind requires anchoring at start-of-string (skipping leading whitespace) and a word-boundary at the end of the needle; (b) explicit unit test `test_prefix_sure_word_boundary` pins the contract; (c) SUBSTRING kind is intentionally looser (matches anywhere) but is reserved for tokens like `—`, emoji, `as an AI` where false positives are acceptable; (d) the alias table is a hand-maintained set of vetted patterns, not a generic NL parser.

---

## 6. Sequencing (implementer order)

1. **Scaffold the header and stub.** Create `include/human/persona/style_critique.h` with the declared interface; create `src/persona/style_critique.c` with a stub returning "no violation". Add both to `CMakeLists.txt`. Verify with `cmake --build --preset dev` (no behavior change).
2. **Add the config key.** Modify `include/human/config.h`, `src/config_parse_agent.c`, `src/config_merge.c`, `src/config_serialize.c`, `src/config_schema.c`. Verify with `./build/human_tests --filter=config` — existing tests must still pass; new key default-false.
3. **Write the pattern tests first (TDD).** Create `tests/test_style_critique_patterns.c` covering the 10 cases in §4. All should fail against the stub. Verify: `./build/human_tests --filter=style_critique_pattern` — expect 10 fails.
4. **Implement the matcher.** Fill in `style_critique.c`: alias table, rule parser, PREFIX/SUBSTRING evaluator with case-insensitive ASCII compare and word-boundary anchoring. Verify: `./build/human_tests --filter=style_critique_pattern` — expect 10 passes.
5. **Wire the hook in `agent_turn.c`.** Add the block at line 5396 (sibling to existing constitutional block, no `#ifndef HU_IS_TEST` wrap). Augmented-system-prompt builder lives inline (small enough). Add the `HU_IS_TEST` counters. Verify: `cmake --build --preset dev && ./build/human_tests --suite=AGENT` — no regression on existing tests.
6. **Write the self-critique integration tests.** Create `tests/test_style_self_critique.c` with the mock provider per §4. Verify: `./build/human_tests --filter=style_self_critique` — all 6 pass.
7. **Full-suite + ASan.** Run `./build/human_tests` end-to-end. Expect: 0 failures, 0 ASan errors, no leak in style_critique paths. Run `scripts/agent-preflight.sh`.
8. **Verify via `/verify`.** Spawn verifier agent with the 5 AC as the contract. Expect `RESULT_verifier=PASS`.

---

## 7. Open questions

1. **Should violating drafts feed DPO pair mining?** A natural extension: when style critique fires, the violating draft is a *negative* example and the clean regen is a *positive* example — exactly the (rejected, chosen) pair shape that the DPO collector (US-7.1) consumes. **Recommendation: out of scope for US-7.9.** File as a follow-on story candidate ("US-7.X: route style-critique decisions into DPO pair collector"). Reasons: (a) the AC don't ask for it; (b) requires a decision on whether style violations are good DPO signal (they are mechanical, not preference-based, so they may bias the trained model toward rule-compliance over helpfulness); (c) the DPO collector's vtable may need a new "source" tag (`HU_DPO_SOURCE_STYLE_CRITIQUE`) to keep mining counts honest. Recommend a 30-minute design spike in a future sprint before committing.

2. **Should we add a new `hu_observer_event_tag_t` value for `STYLE_RULE_VIOLATION_UNRESOLVED`?** AC-7.9.3 says "emits an event" but the observer is typed-enum-driven, not free-string. The cheaper read is `hu_log_info` + a test counter (chosen in §2). If a downstream caller actually wants to *react* to this event programmatically (e.g. a feedback UI), we would need the enum value. **Recommendation: defer until a caller asks.** YAGNI.

3. **Alias table location — code or JSON?** The phrase→needle alias table (`em-dashes` → `—`, `emoji` → multi-codepoint scan) is hand-maintained. Putting it in JSON (`config/persona/style_aliases.json`) would let users extend it without recompile, but that's a feature the AC don't require. **Recommendation: hard-code for v1, document the location, and only JSON-ify if users actually ask.**

---

RESULT_tech-lead=DESIGN_READY
