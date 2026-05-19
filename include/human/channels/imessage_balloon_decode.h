/* include/human/channels/imessage_balloon_decode.h
 *
 * Phase 5 of docs/plans/2026-05-18-imessage-sota.md: per-balloon-type
 * payload decoders that walk a chat.db `payload_data` plist (parsed via
 * the bplist00 parser shipped in Phase 3) into typed detail strings
 * ready for hu_imessage_synth_balloon / hu_imessage_ingest_balloon.
 *
 * Five balloon kinds are decoded; UNKNOWN bundles fall through to the
 * generic "sent an iMessage app payload" narration.
 *
 * Privacy contract (pinned by tests):
 *   - Apple Pay decoders NEVER read amount / currency / value keys.
 *     Detail = recipient handle only.
 *   - Placemark decoders NEVER read latitude / longitude. Detail =
 *     locality / country / place name only.
 *
 * Original Phase 5 spec authored by parallel agent a41aad0cbc10f99b4
 * (2026-05-19); reimplemented in the main session after the agent's
 * worktree was auto-cleaned before integration. */

#ifndef HU_CHANNELS_IMESSAGE_BALLOON_DECODE_H
#define HU_CHANNELS_IMESSAGE_BALLOON_DECODE_H

#include "human/channels/imessage_ingest.h"
#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hu_personal_model;

#ifdef __cplusplus
extern "C" {
#endif

/* Each decoder takes a payload_data blob (bplist00 binary plist) and
 * writes a NUL-terminated `detail_out` string of at most `cap-1` bytes,
 * suitable for passing as the `detail` arg to hu_imessage_synth_balloon
 * or hu_imessage_ingest_balloon. Returns bytes written excluding NUL.
 * Returns 0 on parse failure / kind mismatch / required key absent —
 * caller falls back to generic "<actor> sent an iMessage app payload". */

size_t hu_imessage_decode_url_preview(const unsigned char *payload, size_t payload_len,
                                      char *detail_out, size_t cap);

size_t hu_imessage_decode_apple_pay(const unsigned char *payload, size_t payload_len,
                                    char *detail_out, size_t cap);

size_t hu_imessage_decode_placemark(const unsigned char *payload, size_t payload_len,
                                    char *detail_out, size_t cap);

size_t hu_imessage_decode_music(const unsigned char *payload, size_t payload_len, char *detail_out,
                                size_t cap);

size_t hu_imessage_decode_poll(const unsigned char *payload, size_t payload_len, char *detail_out,
                               size_t cap);

/* Dispatch helper: classify by balloon_bundle_id, then call the right
 * decoder. AUDIO_TRANSCRIPT delegates to hu_imessage_extract_audio_transcript
 * since that path was wired earlier. Returns the classified kind;
 * UNKNOWN means no decoder fired and `detail_out` is empty. */
hu_imessage_balloon_kind_t hu_imessage_balloon_decode(const char *bundle_id,
                                                      const unsigned char *payload,
                                                      size_t payload_len, char *detail_out,
                                                      size_t cap);

/* High-level row-ingest: decode + synthesize + ingest in one call.
 * Returns HU_OK even when the balloon kind is UNKNOWN (caller continues).
 * Intended to be called from src/daemon_imessage_observer.c per-row. */
hu_error_t hu_imessage_ingest_balloon_row(struct hu_personal_model *model, const char *bundle_id,
                                          const unsigned char *payload, size_t payload_len,
                                          const char *sender_handle, bool is_from_me,
                                          int64_t timestamp_unix, bool in_group_chat);

#ifdef __cplusplus
}
#endif

#endif /* HU_CHANNELS_IMESSAGE_BALLOON_DECODE_H */
