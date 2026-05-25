/* include/human/channels/imessage_ingest.h
 *
 * iMessage → personal-model ingest (Phase 1 of docs/plans/2026-05-18-imessage-sota.md).
 *
 * Phase 1a (this header): pure synthesis primitives that render an
 * iMessage event into a canonical English sentence. The sentence is the
 * input to hu_personal_model_ingest() — see Phase 1b for the wiring.
 *
 * The synthesis layer is testable in isolation (no chat.db, no provider
 * calls, no personal-model dependency). That's the whole point of
 * splitting it out: pin the wording before the wiring. */

#ifndef HU_CHANNELS_IMESSAGE_INGEST_H
#define HU_CHANNELS_IMESSAGE_INGEST_H

#include "human/channels/reaction_event.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Balloon (iMessage app-message) kinds we synthesize. Mapped from the
 * balloon_bundle_id column in chat.db. UNKNOWN means "we recognize that
 * this is a balloon but don't ingest type-specific facts" — synthesis
 * falls back to a generic label. */
typedef enum {
    HU_IMESSAGE_BALLOON_UNKNOWN = 0,
    HU_IMESSAGE_BALLOON_URL_PREVIEW,
    HU_IMESSAGE_BALLOON_APPLE_PAY,
    HU_IMESSAGE_BALLOON_PLACEMARK,
    HU_IMESSAGE_BALLOON_MUSIC,
    HU_IMESSAGE_BALLOON_POLL,
    HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT,
} hu_imessage_balloon_kind_t;

/* Map a balloon_bundle_id (from chat.db `message.balloon_bundle_id`) to
 * our enum. Returns HU_IMESSAGE_BALLOON_UNKNOWN for any bundle ID we
 * don't currently decode. NULL/empty input → UNKNOWN. */
hu_imessage_balloon_kind_t hu_imessage_balloon_kind_from_bundle_id(const char *bundle_id);

/* Render a tapback as: "<sender> reacted with <symbol> to <target>".
 *
 * `event->kind` selects the symbol ("❤️", "👍", "👎", "😂", "‼️", "❓", or
 * a custom emoji glyph when `custom_emoji` is non-NULL).
 *
 * `target_text_preview` is the truncated text of the message being
 * reacted to (the "what"). May be NULL/empty — synthesis omits it.
 *
 * `is_from_me_target` true means "reacted to my message"; false means
 * "reacted to a message" (we don't have the target sender's name in
 * the typical poll path).
 *
 * For removal events (event->is_removal=1) synthesis produces a "removed
 * the reaction" form, which the fact extractor will treat as a retraction.
 *
 * Returns bytes written (excluding NUL), or 0 on invalid input or
 * insufficient capacity. */
size_t hu_imessage_synth_reaction(const hu_reaction_event_t *event, const char *custom_emoji,
                                  const char *target_text_preview, bool is_from_me_target,
                                  char *out, size_t out_cap);

/* Render an edit. If `old_text` is provided, synthesis describes the
 * delta ("<sender> edited their message from '<old>' to '<new>'"),
 * which is the persona-revealing version. If `old_text` is NULL, falls
 * back to a generic form. `is_from_me` flips perspective to first
 * person ("I edited..."). */
size_t hu_imessage_synth_edit(const char *sender_handle, bool is_from_me, const char *old_text,
                              const char *new_text, char *out, size_t out_cap);

/* Render an unsend ("<sender> retracted a message" optionally with a
 * preview of the redacted text). When is_from_me=true → first person. */
size_t hu_imessage_synth_unsend(const char *sender_handle, bool is_from_me,
                                const char *redacted_preview, char *out, size_t out_cap);

/* Render a reply-in-thread: "<sender> replied to <parent> with <reply>". */
size_t hu_imessage_synth_reply(const char *sender_handle, bool is_from_me,
                               const char *parent_text_preview, const char *reply_text, char *out,
                               size_t out_cap);

/* Render a balloon (app-message) event. `detail` carries the type-
 * specific string:
 *   URL_PREVIEW       → og:title or the URL itself
 *   APPLE_PAY         → counterparty handle (NO amounts — privacy)
 *   PLACEMARK         → place name or address
 *   MUSIC             → "<track> by <artist>"
 *   POLL              → poll question/topic
 *   AUDIO_TRANSCRIPT  → the transcribed text
 *   UNKNOWN           → ignored; falls back to generic label
 *
 * Apple Pay deliberately drops the amount from synthesis: only the
 * fact-of-payment enters the personal model, not the financial precision.
 * This is a privacy decision documented in the Phase 5 plan. */
size_t hu_imessage_synth_balloon(const char *sender_handle, bool is_from_me,
                                 hu_imessage_balloon_kind_t kind, const char *detail, char *out,
                                 size_t out_cap);

/* ── Phase 1b: ingest wrappers ────────────────────────────────────────
 *
 * Each wrapper synthesizes the canonical text, builds a provenance stamp
 * (channel = "imessage_dm" or "imessage_group" per `in_group_chat`,
 * handle = `sender_handle`, ts = `timestamp_unix`), and routes through
 * hu_personal_model_ingest with `from_user = is_from_me_*`.
 *
 * Forward-declared to keep this header free of the full personal_model.h
 * inclusion (avoids pulling memory subsystem types into channels code). */
struct hu_personal_model;

#include "human/core/error.h"

#ifdef HU_ENABLE_IMESSAGE
hu_error_t hu_reaction_ingest_personal_model(struct hu_personal_model *model,
                                             const hu_reaction_event_t *event,
                                             const char *custom_emoji,
                                             const char *target_text_preview,
                                             bool is_from_me_target, bool in_group_chat);
#else
/* HU_ENABLE_IMESSAGE=OFF stub. Without the iMessage channel compiled in,
 * imessage_ingest.c isn't built — so callers in reaction_handler.c need an
 * inline no-op to satisfy the link contract (per
 * ~/.claude/rules/test-source-gate-symmetry.md). The reaction-handler path
 * still works for non-iMessage channels; this stub silently skips the
 * personal-model bump that only iMessage tapbacks provide. */
static inline hu_error_t
hu_reaction_ingest_personal_model(struct hu_personal_model *model, const hu_reaction_event_t *event,
                                  const char *custom_emoji, const char *target_text_preview,
                                  bool is_from_me_target, bool in_group_chat) {
    (void)model;
    (void)event;
    (void)custom_emoji;
    (void)target_text_preview;
    (void)is_from_me_target;
    (void)in_group_chat;
    return HU_OK;
}
#endif

hu_error_t hu_imessage_ingest_edit(struct hu_personal_model *model, const char *sender_handle,
                                   bool is_from_me, const char *old_text, const char *new_text,
                                   int64_t timestamp_unix, bool in_group_chat);

hu_error_t hu_imessage_ingest_unsend(struct hu_personal_model *model, const char *sender_handle,
                                     bool is_from_me, const char *redacted_preview,
                                     int64_t timestamp_unix, bool in_group_chat);

hu_error_t hu_imessage_ingest_reply(struct hu_personal_model *model, const char *sender_handle,
                                    bool is_from_me, const char *parent_text_preview,
                                    const char *reply_text, int64_t timestamp_unix,
                                    bool in_group_chat);

hu_error_t hu_imessage_ingest_balloon(struct hu_personal_model *model, const char *sender_handle,
                                      bool is_from_me, hu_imessage_balloon_kind_t kind,
                                      const char *detail, int64_t timestamp_unix,
                                      bool in_group_chat);

/* ── Phase 3: bplist payload extractors ───────────────────────────────
 *
 * These read raw bplist00 blobs from chat.db columns (payload_data for
 * voice messages, message_summary_info for edits). They use the
 * dependency-free hu_bplist parser in src/util/bplist.c. */

/* Extract the auto-transcribed text from a voice-message payload_data
 * plist. Voice messages carry their Speech-recognizer transcript under
 * either "transcribed_text" (older iOS) or "transcription" (newer iOS).
 * Returns bytes written to `out` (excluding NUL), or 0 if neither key
 * is present, the blob is malformed, or any argument is invalid. */
size_t hu_imessage_extract_audio_transcript(const unsigned char *payload_blob, size_t payload_len,
                                            char *out, size_t cap);

/* Extract the recording duration (seconds) from a voice-message payload_data
 * plist. Apple has used several keys across iOS releases; we try them in
 * order: "duration", "audio_duration", "recording_duration". Returns the
 * first numeric (integer or real) value found, or 0.0 if no recognized key
 * is present. 0.0 is also returned for any parse / arg failure — callers
 * must treat 0.0 as "unknown" (not "instantaneous"). */
double hu_imessage_extract_audio_duration(const unsigned char *payload_blob, size_t payload_len);

/* B5 production wire: classify a voice-message's tone heuristically (via
 * hu_audio_tone_classify) and ingest the resulting fact into the personal
 * model. No-op when classification returns UNKNOWN (e.g. duration==0,
 * empty transcript). Safe to call even when model is NULL (returns OK).
 * Designed to be called alongside the transcript-ingest in the iMessage
 * audio path, so a single voice message produces:
 *   (a) the transcript ("alice said: ...")
 *   (b) the tone fact   ("alice's voice message sounded energetic.")
 * Both are stamped with the same provenance and timestamp. */
hu_error_t hu_imessage_ingest_audio_tone(struct hu_personal_model *model, const char *sender_handle,
                                         bool is_from_me, const char *transcript_text,
                                         double duration_seconds, int64_t timestamp_unix,
                                         bool in_group_chat);

/* Extract the edit chain from a message_summary_info plist. Apple
 * stores per-part edit histories under the "ec" key as a dict keyed by
 * part-index strings ("0", "1", ...). Each part value is an array of
 * edit-event dicts whose "t" key holds the historical text (either a
 * raw string or a typedstream blob extractable via
 * hu_imessage_extract_attributed_body).
 *
 * This function flattens ALL parts' histories into one ordered list
 * (part 0 first, oldest-edit first). Writes each entry into
 * out_buf[i * entry_cap], NUL-terminated and truncated to entry_cap-1
 * bytes. Returns the number of entries written, capped at
 * out_count_max. Returns 0 if no edits are present or the blob fails
 * to parse. */
size_t hu_imessage_extract_edit_chain(const unsigned char *summary_blob, size_t summary_len,
                                      char *out_buf, size_t out_count_max, size_t entry_cap);

/* ── Phase 4: typedstream attribute-run synthesis ─────────────────────
 *
 * Render a single attributedBody blob into a personal-model-ingestable
 * sentence, INCLUDING attribute-run signal. OTP-containing blobs return
 * 0 (caller should skip — these are 2FA codes, NOT persona signal).
 * Mentions get "(@<handle>)" inline. `sender_handle` and `is_from_me`
 * select the perspective ("I said: ..." vs "<handle> said: ...").
 *
 * Returns bytes written (excluding NUL), 0 on OTP/malformed/empty. */
size_t hu_imessage_synth_attributed_message(const unsigned char *blob, size_t blob_len,
                                            const char *sender_handle, bool is_from_me, char *out,
                                            size_t out_cap);

#ifdef __cplusplus
}
#endif
#endif /* HU_CHANNELS_IMESSAGE_INGEST_H */
