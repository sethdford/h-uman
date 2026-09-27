---
title: F1 Spoken Memos Implementation Plan
description: Cleanup, drift guard, restraint, rewrite-for-the-ear and pre-shape capture for voice memos.
category: plans
---

# F1 Spoken Memos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A voice memo speaks a cleaned, spoken-register version of the reply with Ferni's restraint, never a text message read aloud.

**Architecture:** New pure module `src/tts/speech_text.c` (S2 cleanup + S3 drift guard), restraint changes in `src/tts/transcript_prep.c` (S5), new `src/tts/speech_rewrite.c` (S1 prompt + provider call + pipeline, `HU_SPEECH_REWRITE` off/shadow/live), all driven from `hu_daemon_voice_reply`, which runs the W1 gates again on the spoken text (S4) and, last, receives the reply captured before text shaping.

**Tech Stack:** C11, project test framework, provider vtable `chat_with_system`.

**Spec:** `docs/superpowers/specs/2026-09-27-f1-spoken-memos-design.md`

## Global Constraints

- C11 `-Werror`; ASan clean; tests hermetic (no network, no spawning); `HU_IS_TEST` guards for effects.
- Default `HU_SPEECH_REWRITE` unset ⇒ off. Gate comment at the activation site names the measurement (A/B drip preference for rewrite over cleanup, then W5).
- Never log reply/inbound text or handles; log reasons only.
- Word-boundary matching for every token rule (substring-classifier-pitfalls).
- Every commit keeps dead-strip A/B, clone, sqlite, isolation ratchets flat: each new object gets a product caller in the task that creates it.
- String literals ≤ 4095 bytes (`-Wpedantic` overlength-strings on GCC).

## Review Focus

- A rewrite that adds a time/number/name ("see you at 7") must never be spoken — drift guard + test.
- "not funny" / "funnyman" must not trigger laughter — cue matching is word-boundary and cue-list only.
- A reply that is only an emoji or only a URL must not produce an empty memo — cleanup yields empty ⇒ voice declined (text goes).
- Rewrite provider error/timeout/empty output ⇒ S2-only text, never an empty memo.
- The spoken text must pass the W1 gates even when the original did (S4) — test with a rewrite that introduces companion-safety language.

---

### Task 1: `speech_text` — cleanup (S2) and drift guard (S3), wired into the voice path

**Files:** Create `include/human/tts/speech_text.h`, `src/tts/speech_text.c`, `tests/test_speech_text.c`. Modify `CMakeLists.txt` (source next to `src/tts/transcript_prep.c` in the same gate block; test registered the same way as `tests/test_transcript_prep.c`), `tests/test_main.c`, `src/daemon/daemon_voice_reply.c`.

**Produces:**
```c
/* Spoken form of a reply. Returns output length (0 = nothing speakable).
 * *laughter_cue is set when the text laughed (lol/haha/lmao/hahaha...). */
size_t hu_speech_cleanup(const char *in, size_t in_len, char *out, size_t cap, bool *laughter_cue);

typedef enum { HU_SPEECH_DRIFT_OK = 0, HU_SPEECH_DRIFT_NEW_NUMBER, HU_SPEECH_DRIFT_NEW_NAME,
               HU_SPEECH_DRIFT_QUESTION, HU_SPEECH_DRIFT_LENGTH, HU_SPEECH_DRIFT_BANNED
} hu_speech_drift_t;
hu_speech_drift_t hu_speech_drift_check(const char *original, size_t original_len,
                                        const char *rewritten, size_t rewritten_len);
const char *hu_speech_drift_name(hu_speech_drift_t d);
```
Cleanup rules (spec S2): strip `*...*`, `(...)`, `[...]` except `[laughter]`, emoji (reuse the ranges in `transcript_prep.c:79-104` by moving them to a shared static helper only if the clone ratchet requires it), URLs (whole-message URL ⇒ "I'll send you the link", otherwise dropped); expand lmk, tmrw, idk, rn, bc, ngl, tbh, omw, u, ur (ur→"you're" before an -ing word or "gonna/going/welcome/right", else "your"); laugh tokens (lol, lmao, haha+, hehe) → removed and `*laughter_cue = true` — isolated in `static bool laugh_token_rule(...)` with a comment inviting Seth's rule. Word-boundary matching, case-insensitive; collapse double spaces; trim.

Drift rules (spec S3): digits/time/currency tokens in rewritten ⊄ original ⇒ NEW_NUMBER; a capitalized word (not sentence-initial, not "I") in rewritten whose case-folded form is absent from original ⇒ NEW_NAME; `?` present in exactly one side ⇒ QUESTION; word count ratio outside [0.5, 1.6] ⇒ LENGTH; starts with Well/So/Hmm (word-boundary) or contains "good question", `*`, `[`, `(` ⇒ BANNED.

Wiring: in `hu_daemon_voice_reply`, after the W1 gate, compute `spoken = hu_speech_cleanup(response)`; if 0 bytes ⇒ return false (text goes); pass `spoken` (not `response`) to `hu_voice_reply_build_request` and `hu_voice_tts`; run `hu_voice_reply_gates_clear(spoken, combined)` (S4) — decline on fail. `laughter_cue` is carried into Task 2.

- [ ] Step 1: failing tests — `test_speech_cleanup_*` table incl. `"yeah lol that sounds good, lmk when ur free tmrw"` → `"yeah that sounds good, let me know when you're free tomorrow"`-shaped (assert exact), `"*laughs* ok"` → `"ok"`, `"https://x.example"` → link sentence, `"👍"` → 0; `test_speech_drift_*` one per reason plus an OK case; daemon test through the fallback arm asserting the Cartesia mock received "let me know" not "lmk" (use the capture accessor next to `cartesia_test_capture` in `src/tts/cartesia.c`; if no accessor exists, add `hu_cartesia_test_last_transcript()` under `HU_IS_TEST`).
- [ ] Step 2: build, see the undefined-symbol RED.
- [ ] Step 3: implement; Step 4: GREEN + mutation (drop the ur→you're rule; drop NEW_NUMBER) each makes a named test fail.
- [ ] Step 5: full suite; commit `feat(voice): speak a cleaned reply — S2 cleanup + S3 drift guard + S4 gate on spoken text`.

### Task 2: Restraint in `transcript_prep` (S5)

**Files:** Modify `include/human/tts/transcript_prep.h` (add `bool laughter_cue;` to `hu_prep_config_t`), `src/tts/transcript_prep.c`, `src/tts/voice_reply.c` + `include/human/tts/voice_reply.h` (add `bool laughter_cue` parameter to `hu_voice_reply_build_request`; callers: `daemon_voice_reply.c`, `src/app/main.c` preview (passes the cue from its own cleanup of `--text`), tests), `tests/test_transcript_prep.c`, `tests/test_voice_reply.c`.

Changes:
- `pick_nonverbal`: `[laughter]` only if `config->laughter_cue` or the sentence contains a laugh token (word-boundary: haha+, lol, lmao); probability 18%; at most one `[laughter]` per output; never for sympathetic/contemplative/sad. Remove the random `[laughter]`/`"Hmm... "` fallbacks entirely. Keep the contemplative/sympathetic pause breaks.
- `hu_voice_reply_build_request`: `.thinking_sounds = false`, `.discourse_rate = 0.0f` (library knobs unchanged).
- `inject_clause_breaks`: no break at a comma unless the next word is but/yet/however/although (250 ms); `;`/`:` keep 200 ms; em dash unchanged.
- Budget in the assembly loop: ≤ 2 inline `<emotion>` tags, ≤ 1 `<volume>` tag per output; speed: at most one sentence gets a non-base speed (a later speed-tagged sentence is emitted at base speed without opening a tag, so the existing open/reset pairing stays balanced — keep `test_voice_reply_speed_tags_are_relative_and_reset` green).
- Update pinning tests with a one-line comment each: `test_prep_*clause*` (commas no longer pause; add "I wanted to, but it rained." ⇒ ≥1 break), nonverbal tests (cue text still finds `[laughter]`).

- [ ] Step 1: failing tests — no `[laughter]` over 300 seeds for non-laughing text with `nonverbals_enabled` + joking emotion; ≤1 `[laughter]` for laughing text; `"Hmm"` never over 300 seeds; comma vs "but" breaks; emotion-tag budget on a 6-sentence mixed-emotion text; `laughter_cue` alone enables laughter.
- [ ] Step 2 RED; Step 3 implement; Step 4 GREEN + mutation (restore the random fallback ⇒ the no-laughter test fails).
- [ ] Step 5: full suite; commit `feat(voice): Ferni restraint in transcript prep`.

### Task 3: `speech_rewrite` — rewrite for the ear (S1), off/shadow/live

**Files:** Create `include/human/tts/speech_rewrite.h`, `src/tts/speech_rewrite.c`, `tests/test_speech_rewrite.c`. Modify CMake/test_main, `src/daemon/daemon_voice_reply.c`.

**Produces:**
```c
typedef enum { HU_SPEECH_REWRITE_OFF = 0, HU_SPEECH_REWRITE_SHADOW, HU_SPEECH_REWRITE_LIVE } hu_speech_rewrite_mode_t;
hu_speech_rewrite_mode_t hu_speech_rewrite_mode_parse(const char *s);
/* System prompt: fixed mechanics block + persona style lines. Returns length or 0. */
size_t hu_speech_rewrite_system_prompt(const struct hu_persona *p, char *out, size_t cap);
typedef struct { char spoken[2048]; size_t spoken_len; bool used_rewrite; const char *reason; bool laughter_cue; } hu_speech_result_t;
/* S1 -> S2 -> S3. provider may be NULL (S2 only). Never returns empty unless the cleaned reply is empty. */
hu_error_t hu_speech_prepare(hu_allocator_t *alloc, const hu_provider_t *provider, const char *model,
                             size_t model_len, const struct hu_persona *persona,
                             hu_speech_rewrite_mode_t mode, const char *reply, size_t reply_len,
                             const char *inbound, size_t inbound_len, hu_speech_result_t *out);
```
Mechanics block: a `static const char[]` (< 4095 bytes) porting Ferni's `voice-guidance.md` + `dynamic-speech-guidance.ts` rules for a memo from Seth (spec S1 list). Persona lines from `preferred_vocab`, `avoided_vocab`, `slang`, `anti_patterns` (first 12 of each). The inbound message is included as context ("reply to this, don't answer it again"). Provider call: `provider->vtable->chat_with_system(provider->ctx, alloc, sys, sys_len, msg, msg_len, model, model_len, 0.7, &out, &out_len)`; free with `alloc`. OFF ⇒ S2 only (no call). SHADOW ⇒ run S1+S3, return S2 text as `spoken`, and append one JSON line `{ts, mode, used, reason, original_len, cleaned, rewritten}` to `hu_paths_state("voice/shadow/%s.jsonl", date)` (non-`HU_IS_TEST` only; the JSON line builder is pure and tested). LIVE ⇒ rewritten text when S3 passes, else S2.

Wiring: `hu_daemon_voice_reply` replaces its Task-1 cleanup call with `hu_speech_prepare(alloc, &agent->provider, <agent model>, agent->persona, hu_speech_rewrite_mode_parse(getenv("HU_SPEECH_REWRITE")), ...)` and passes `result.laughter_cue` to `hu_voice_reply_build_request`; S4 gate runs on `result.spoken`. Gate comment names the measurement.

- [ ] Step 1: failing tests with a mock provider (vtable whose `chat_with_system` returns a canned string and counts calls): OFF makes 0 calls and returns cleaned text; LIVE returns the rewrite; LIVE with a rewrite adding "at 7" returns the cleaned text and reason `new_number`; SHADOW makes 1 call but returns the cleaned text; provider error ⇒ cleaned text; empty rewrite ⇒ cleaned text; system prompt contains the mechanics and a persona slang line and is < 4095; the shadow JSON line builder escapes quotes and contains no inbound text.
- [ ] Step 2 RED; Step 3 implement; Step 4 GREEN + mutation (skip the drift check in LIVE ⇒ the "at 7" test fails).
- [ ] Step 5: full suite; commit `feat(voice): rewrite voice replies for the ear (HU_SPEECH_REWRITE, default off)`.

### Task 4: Speak the reply captured before text shaping

**Files:** Modify `src/daemon.c` (~8570–8690), `include/human/daemon.h`, `src/daemon/daemon_voice_reply.c`, `tests/test_daemon_voice_reply.c`.

Add `const char *unshaped, size_t unshaped_len` to `hu_daemon_voice_reply` (NULL ⇒ use `response`). In `daemon.c`, before `hu_daemon_shape_text_inplace`, when the channel's daemon config has `voice_enabled`, copy `response` into an allocator buffer; pass it; free after the call. The voice pipeline speaks `unshaped` (W1 gate still runs on both the unshaped text and, via S4, the spoken text).

- [ ] Step 1: failing daemon test through the fallback arm: `unshaped="let me know when you're free"`, `response` with injected typo/"haha " prefix ⇒ the captured Cartesia transcript contains no "haha" and no typo.
- [ ] Step 2 RED; Step 3 implement; Step 4 GREEN; full suite; commit `fix(voice): speak the reply as written, not the text-styled copy`.

### Task 5: Hear it — preview flag + shadow samples

**Files:** Modify `src/app/main.c` (`human voice preview --rewrite off|shadow|live`, default follows `HU_SPEECH_REWRITE`; prints `used_rewrite`, `reason`, and the spoken text before synthesis).

- [ ] Step 1: build; run preview in `live` on 5 of Seth's real texts from `~/.human/voice_corpus.jsonl` (the same source the A/B drip uses) with the persona's provider; save clips to `/private/tmp/claude-501/f1/`; record `used_rewrite`/`reason` per clip in the ledger. No messages are sent.
- [ ] Step 2: commit `feat(voice-cli): preview the spoken rewrite`.
