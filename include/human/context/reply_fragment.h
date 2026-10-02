/* Reply fragment detection — is an outbound text cut off mid-thought?
 *
 * 2026-09-30, two real sends: the guard's repair retry produced "Wait, did we
 * actually lock" (a sentence cut mid-clause), and a bubble split sent
 * "Nah too windy. just" | "hung out by the water" | "peaceful". Both read as a
 * broken phone, not a person. These helpers name that shape so the repair
 * path, the bubble splitters and the final outbound check can refuse it.
 *
 * Casual texting is NOT a fragment: "lol", "peaceful", "did you eat",
 * "yeah we should" all end without punctuation and are complete. A fragment
 * is one of:
 *   - ends on a function word no complete utterance ends on ("just", "and",
 *     "the", "because", a contracted auxiliary like "it's"),
 *   - ends on clause punctuation that promises more (",", ";", ":", a dash),
 *   - has an unclosed paren or quote,
 *   - is sentence-case ("Wait, ...") yet ends its final clause, an inverted
 *     question ("did we actually lock"), with no "?" — the register says the
 *     writer punctuates, so the missing "?" means the text was cut.
 *
 * Pure functions; no allocation. */
#ifndef HU_CONTEXT_REPLY_FRAGMENT_H
#define HU_CONTEXT_REPLY_FRAGMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when `text` reads as cut off mid-thought (see file comment). NULL or
 * blank text is not a fragment. */
bool hu_reply_is_fragment(const char *text, size_t len);

/* True when the word `word[0..len)` (any case, straight or curly apostrophe)
 * is a function word a complete utterance never ends on. */
bool hu_reply_word_is_dangling(const char *word, size_t len);

/* Length of the longest prefix of `text`, at most `cap` bytes, that ends at a
 * sentence end (. ! ? or an ellipsis) followed by whitespace or the end of the
 * text. 0 when there is none. */
size_t hu_reply_trim_to_sentence(const char *text, size_t len, size_t cap);

/* When `text` ends with a lone dangling function word standing after a
 * sentence or clause break ("Nah too windy. just", "sounds good, and"),
 * returns the length without it (and without the break's comma/semicolon or
 * trailing space). Otherwise returns `len` unchanged. Never invents text. */
size_t hu_reply_drop_dangling_tail(const char *text, size_t len);

/* True when cutting `text` at byte `cut` makes a clean bubble break: the left
 * side does not end on a dangling function word — nor, since a splitter can
 * always pick another space, on a preposition, pronoun or bare auxiliary —
 * and the right side has at least two words (no 1-word tail). */
bool hu_reply_cut_is_clean(const char *text, size_t len, size_t cut);

/* Final outbound check, run on the text about to be bubbled and sent. When it
 * is a fragment, logs one WARN line (lengths and a counter only, never text)
 * and bumps the process counter. Returns the length to send: `len` minus a
 * lone dangling function word (hu_reply_drop_dangling_tail), else `len`.
 * `observer` is an hu_observer_t * (may be NULL). */
size_t hu_reply_final_check(const char *text, size_t len, void *observer);

/* Process-wide count of fragments seen by hu_reply_final_check. */
uint64_t hu_reply_final_fragment_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_CONTEXT_REPLY_FRAGMENT_H */
