/* src/channels/imessage_ingest.c
 *
 * Phase 1a + 1b of docs/plans/2026-05-18-imessage-sota.md.
 *
 * Phase 1a: pure synthesis primitives that render iMessage events into
 * canonical English. No chat.db access, no Apple frameworks, no
 * personal-model dependency: testable in isolation.
 *
 * Phase 1b: hu_imessage_ingest_* wrappers that synthesize + build a
 * provenance stamp + call hu_personal_model_ingest. The personal-model
 * dependency is local to this TU and gated by NULL check so the wrappers
 * still link in builds that exclude the memory subsystem. */

#include "human/channels/imessage_ingest.h"

#include "human/agent/channel_trust.h"
#include "human/memory/personal_model.h"

#include <stdio.h>
#include <string.h>

/* ── Internal helpers ─────────────────────────────────────────────── */

/* Apple's six standard tapback glyphs, ordered to match hu_reaction_kind_t
 * (UNKNOWN, LOVE, LIKE, DISLIKE, LAUGH, EMPHASIZE, QUESTION, CUSTOM_EMOJI).
 * CUSTOM_EMOJI's slot is intentionally empty; the caller passes the
 * actual emoji glyph from the `associated_message_emoji` column. */
static const char *reaction_glyph(hu_reaction_kind_t kind, const char *custom_emoji) {
    switch (kind) {
    case HU_REACTION_LOVE:
        return "\xe2\x9d\xa4\xef\xb8\x8f"; /* ❤️ */
    case HU_REACTION_LIKE:
        return "\xf0\x9f\x91\x8d"; /* 👍 */
    case HU_REACTION_DISLIKE:
        return "\xf0\x9f\x91\x8e"; /* 👎 */
    case HU_REACTION_LAUGH:
        return "\xf0\x9f\x98\x82"; /* 😂 */
    case HU_REACTION_EMPHASIZE:
        return "\xe2\x80\xbc\xef\xb8\x8f"; /* ‼️ */
    case HU_REACTION_KIND_QUESTION:
        return "\xe2\x9d\x93"; /* ❓ */
    case HU_REACTION_KIND_CUSTOM_EMOJI:
        return (custom_emoji && custom_emoji[0]) ? custom_emoji : "a sticker";
    case HU_REACTION_UNKNOWN:
    default:
        return "a reaction";
    }
}

/* Choose the actor noun. NULL/empty sender_handle becomes "someone" so
 * synthesis never produces "  reacted" with a leading space. */
static const char *actor_or_someone(const char *sender_handle) {
    return (sender_handle && sender_handle[0]) ? sender_handle : "someone";
}

/* Truncate a text preview at `max_chars` UTF-8 bytes, appending "…" if
 * we cut. Writes into `buf` (cap bytes incl. NUL). Returns buf for chaining.
 * Note: this is byte-truncation; we don't split UTF-8 sequences carefully
 * because the consumer (fact_extract) is robust to truncated tails. */
static const char *preview_or_empty(const char *text, char *buf, size_t cap, size_t max_chars) {
    if (!text || !text[0] || cap < 4) {
        if (cap > 0)
            buf[0] = '\0';
        return "";
    }
    size_t n = strlen(text);
    if (n <= max_chars && n < cap) {
        memcpy(buf, text, n);
        buf[n] = '\0';
        return buf;
    }
    size_t cut = (max_chars < cap - 4) ? max_chars : cap - 4;
    memcpy(buf, text, cut);
    /* "…" in UTF-8 is 3 bytes: E2 80 A6 */
    buf[cut + 0] = (char)0xE2;
    buf[cut + 1] = (char)0x80;
    buf[cut + 2] = (char)0xA6;
    buf[cut + 3] = '\0';
    return buf;
}

/* ── Balloon bundle-ID classifier ─────────────────────────────────── */

hu_imessage_balloon_kind_t hu_imessage_balloon_kind_from_bundle_id(const char *bundle_id) {
    if (!bundle_id || !bundle_id[0])
        return HU_IMESSAGE_BALLOON_UNKNOWN;

    /* These prefixes come from imessage-exporter's bundle-ID table and
     * Apple's documented balloon plugin namespaces. The trailing
     * extension after the dot varies across iOS releases, so we match
     * by prefix to avoid version drift. */
    if (strstr(bundle_id, "URLBalloonProvider") || strstr(bundle_id, "richlink"))
        return HU_IMESSAGE_BALLOON_URL_PREVIEW;
    if (strstr(bundle_id, "PassbookUIService.PeerPayment") || strstr(bundle_id, "ApplePay"))
        return HU_IMESSAGE_BALLOON_APPLE_PAY;
    if (strstr(bundle_id, "Placemark") || strstr(bundle_id, "Maps"))
        return HU_IMESSAGE_BALLOON_PLACEMARK;
    if (strstr(bundle_id, "Music") || strstr(bundle_id, "music"))
        return HU_IMESSAGE_BALLOON_MUSIC;
    if (strstr(bundle_id, "Polls") || strstr(bundle_id, "PollBalloon"))
        return HU_IMESSAGE_BALLOON_POLL;
    if (strstr(bundle_id, "MoVoMessageBalloon") || strstr(bundle_id, "AudioMessage"))
        return HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT;

    return HU_IMESSAGE_BALLOON_UNKNOWN;
}

/* ── Synthesis ────────────────────────────────────────────────────── */

size_t hu_imessage_synth_reaction(const hu_reaction_event_t *event, const char *custom_emoji,
                                  const char *target_text_preview, bool is_from_me_target,
                                  char *out, size_t out_cap) {
    if (!event || !out || out_cap < 16)
        return 0;

    const char *actor = actor_or_someone(event->sender_handle);
    const char *glyph = reaction_glyph(event->kind, custom_emoji);
    const char *target = is_from_me_target ? "my message" : "a message";

    char preview_buf[160];
    const char *preview =
        preview_or_empty(target_text_preview, preview_buf, sizeof(preview_buf), 80);

    int n;
    if (event->is_removal) {
        if (preview[0]) {
            n = snprintf(out, out_cap, "%s removed their %s reaction from %s: \"%s\".", actor,
                         glyph, target, preview);
        } else {
            n = snprintf(out, out_cap, "%s removed their %s reaction from %s.", actor, glyph,
                         target);
        }
    } else {
        if (preview[0]) {
            n = snprintf(out, out_cap, "%s reacted with %s to %s: \"%s\".", actor, glyph, target,
                         preview);
        } else {
            n = snprintf(out, out_cap, "%s reacted with %s to %s.", actor, glyph, target);
        }
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < out_cap ? (size_t)n : out_cap - 1;
}

size_t hu_imessage_synth_edit(const char *sender_handle, bool is_from_me, const char *old_text,
                              const char *new_text, char *out, size_t out_cap) {
    if (!out || out_cap < 16 || !new_text || !new_text[0])
        return 0;

    const char *actor = is_from_me ? "I" : actor_or_someone(sender_handle);
    const char *verb = is_from_me ? "edited my" : "edited their";

    char old_buf[160], new_buf[160];
    const char *old_preview = preview_or_empty(old_text, old_buf, sizeof(old_buf), 80);
    const char *new_preview = preview_or_empty(new_text, new_buf, sizeof(new_buf), 80);

    int n;
    if (old_preview[0]) {
        n = snprintf(out, out_cap, "%s %s message from \"%s\" to \"%s\".", actor, verb, old_preview,
                     new_preview);
    } else {
        n = snprintf(out, out_cap, "%s %s message to \"%s\".", actor, verb, new_preview);
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < out_cap ? (size_t)n : out_cap - 1;
}

size_t hu_imessage_synth_unsend(const char *sender_handle, bool is_from_me,
                                const char *redacted_preview, char *out, size_t out_cap) {
    if (!out || out_cap < 16)
        return 0;

    const char *actor = is_from_me ? "I" : actor_or_someone(sender_handle);
    const char *verb = is_from_me ? "retracted a" : "retracted their";

    char preview_buf[160];
    const char *preview = preview_or_empty(redacted_preview, preview_buf, sizeof(preview_buf), 80);

    int n;
    if (preview[0]) {
        n = snprintf(out, out_cap, "%s %s message: \"%s\".", actor, verb, preview);
    } else {
        n = snprintf(out, out_cap, "%s %s message.", actor, verb);
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < out_cap ? (size_t)n : out_cap - 1;
}

size_t hu_imessage_synth_reply(const char *sender_handle, bool is_from_me,
                               const char *parent_text_preview, const char *reply_text, char *out,
                               size_t out_cap) {
    if (!out || out_cap < 16 || !reply_text || !reply_text[0])
        return 0;

    const char *actor = is_from_me ? "I" : actor_or_someone(sender_handle);

    char parent_buf[160], reply_buf[200];
    const char *parent = preview_or_empty(parent_text_preview, parent_buf, sizeof(parent_buf), 80);
    const char *reply = preview_or_empty(reply_text, reply_buf, sizeof(reply_buf), 120);

    int n;
    if (parent[0]) {
        n = snprintf(out, out_cap, "%s replied to \"%s\", saying \"%s\".", actor, parent, reply);
    } else {
        n = snprintf(out, out_cap, "%s replied in thread, saying \"%s\".", actor, reply);
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < out_cap ? (size_t)n : out_cap - 1;
}

size_t hu_imessage_synth_balloon(const char *sender_handle, bool is_from_me,
                                 hu_imessage_balloon_kind_t kind, const char *detail, char *out,
                                 size_t out_cap) {
    if (!out || out_cap < 16)
        return 0;

    const char *actor = is_from_me ? "I" : actor_or_someone(sender_handle);
    const char *verb_subject = is_from_me ? "I" : actor;
    (void)verb_subject; /* future symmetry — keeps the actor pattern uniform */

    char detail_buf[200];
    const char *d = preview_or_empty(detail, detail_buf, sizeof(detail_buf), 120);

    int n = 0;
    switch (kind) {
    case HU_IMESSAGE_BALLOON_URL_PREVIEW:
        n = d[0] ? snprintf(out, out_cap, "%s shared a link about \"%s\".", actor, d)
                 : snprintf(out, out_cap, "%s shared a link.", actor);
        break;
    case HU_IMESSAGE_BALLOON_APPLE_PAY:
        /* No amount in the synthesis — only the fact of payment. */
        n = d[0] ? snprintf(out, out_cap, "%s sent an Apple Pay payment to %s.", actor, d)
                 : snprintf(out, out_cap, "%s sent an Apple Pay payment.", actor);
        break;
    case HU_IMESSAGE_BALLOON_PLACEMARK:
        n = d[0] ? snprintf(out, out_cap, "%s shared a location: \"%s\".", actor, d)
                 : snprintf(out, out_cap, "%s shared a location.", actor);
        break;
    case HU_IMESSAGE_BALLOON_MUSIC:
        n = d[0] ? snprintf(out, out_cap, "%s shared music: \"%s\".", actor, d)
                 : snprintf(out, out_cap, "%s shared a song.", actor);
        break;
    case HU_IMESSAGE_BALLOON_POLL:
        n = d[0] ? snprintf(out, out_cap, "%s created a poll: \"%s\".", actor, d)
                 : snprintf(out, out_cap, "%s created a poll.", actor);
        break;
    case HU_IMESSAGE_BALLOON_AUDIO_TRANSCRIPT:
        n = d[0] ? snprintf(out, out_cap, "%s sent a voice message saying: \"%s\".", actor, d)
                 : snprintf(out, out_cap, "%s sent a voice message.", actor);
        break;
    case HU_IMESSAGE_BALLOON_UNKNOWN:
    default:
        n = snprintf(out, out_cap, "%s sent an iMessage app payload.", actor);
        break;
    }
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < out_cap ? (size_t)n : out_cap - 1;
}

/* ── Phase 1b: ingest wrappers ────────────────────────────────────── */

/* Build a provenance stamp for any reaction-bearing channel. Channel
 * string follows the "channel_qualifier" convention from
 * src/agent/channel_trust.c — qualified strings let the classifier
 * distinguish 1:1 from group/channel (FIRST_PARTY vs THIRD_PARTY tier).
 *
 * Mapping (matches the equals_ci / starts_with_ci entries in
 * src/agent/channel_trust.c):
 *   imessage  → imessage_dm     / imessage_group
 *   slack     → slack_dm        / slack_channel
 *   discord   → discord_dm      / discord_channel
 *   telegram  → telegram_dm     / telegram_group
 *   <other>   → <channel>_dm    / <channel>_group  (THIRD_PARTY fallback —
 *               unrecognized channels are conservatively low-trust). */
static const char *qualifier_suffix(const char *channel_id, bool in_group_chat) {
    if (!in_group_chat)
        return "_dm";
    if (!channel_id)
        return "_group";
    if (strcmp(channel_id, "slack") == 0 || strcmp(channel_id, "discord") == 0)
        return "_channel";
    return "_group";
}

static hu_provenance_t event_provenance(const char *channel_id, const char *sender_handle,
                                        int64_t ts, bool in_group_chat) {
    char buf[64];
    const char *base = (channel_id && channel_id[0]) ? channel_id : "imessage";
    const char *suffix = qualifier_suffix(base, in_group_chat);
    snprintf(buf, sizeof(buf), "%s%s", base, suffix);
    return hu_channel_trust_stamp(buf, strlen(buf), sender_handle,
                                  sender_handle ? strlen(sender_handle) : 0, ts);
}

/* iMessage-specific compatibility shim — preserves the Phase 1a/1b API
 * surface. The generalized provenance helper now derives the channel
 * string from the event itself. */
static hu_provenance_t imessage_provenance(const char *sender_handle, int64_t ts,
                                           bool in_group_chat) {
    return event_provenance("imessage", sender_handle, ts, in_group_chat);
}

/* Shared helper: synthesize → ingest. Returns HU_OK on success even when
 * synthesis produced an empty string (defensive: a synth bug should not
 * propagate as ingest failure). */
static hu_error_t ingest_synthesized(hu_personal_model_t *model, const char *text, size_t text_len,
                                     bool from_user, int64_t ts, const hu_provenance_t *prov) {
    if (!model || !text || text_len == 0)
        return HU_OK;
    return hu_personal_model_ingest(model, text, text_len, from_user, ts, prov);
}

hu_error_t hu_imessage_ingest_reaction(struct hu_personal_model *model,
                                       const hu_reaction_event_t *event, const char *custom_emoji,
                                       const char *target_text_preview, bool is_from_me_target,
                                       bool in_group_chat) {
    if (!model || !event)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[512];
    size_t n = hu_imessage_synth_reaction(event, custom_emoji, target_text_preview,
                                          is_from_me_target, buf, sizeof(buf));
    if (n == 0)
        return HU_OK; /* no content to ingest */

    /* Phase 2 generalization: the synthesis output is channel-agnostic
     * ("<sender> reacted with <glyph> to <target>"), so the same wrapper
     * works for Slack, Discord, etc. The provenance channel string is
     * derived from event->channel_id so reaction_handler can fire this
     * for any reaction-bearing channel — see also the lifted gate at
     * src/agent/reaction_handler.c. */
    hu_provenance_t prov = event_provenance(event->channel_id, event->sender_handle,
                                            event->timestamp_unix, in_group_chat);

    /* from_user = true: the synthesized text is the AGENT's narration of an
     * observed event ("Alice reacted with ❤️ to my message: '...'"), not
     * raw third-party content. hu_personal_model_ingest gates fact
     * extraction + style update on from_user=true; passing false would
     * short-circuit the pipeline at personal_model.c:942. The provenance
     * tier (FIRST_PARTY for DM, THIRD_PARTY for group) still drives the
     * MINJA gate at personal_model.c:966 — group-chat reactions get
     * routed through the quarantine path if they look like injection. */
    return ingest_synthesized(model, buf, n, /*from_user=*/true, event->timestamp_unix, &prov);
}

hu_error_t hu_imessage_ingest_edit(struct hu_personal_model *model, const char *sender_handle,
                                   bool is_from_me, const char *old_text, const char *new_text,
                                   int64_t timestamp_unix, bool in_group_chat) {
    if (!model)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[512];
    size_t n =
        hu_imessage_synth_edit(sender_handle, is_from_me, old_text, new_text, buf, sizeof(buf));
    if (n == 0)
        return HU_OK;

    hu_provenance_t prov = imessage_provenance(sender_handle, timestamp_unix, in_group_chat);
    return ingest_synthesized(model, buf, n, /*from_user=*/true, timestamp_unix, &prov);
}

hu_error_t hu_imessage_ingest_unsend(struct hu_personal_model *model, const char *sender_handle,
                                     bool is_from_me, const char *redacted_preview,
                                     int64_t timestamp_unix, bool in_group_chat) {
    if (!model)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[512];
    size_t n =
        hu_imessage_synth_unsend(sender_handle, is_from_me, redacted_preview, buf, sizeof(buf));
    if (n == 0)
        return HU_OK;

    hu_provenance_t prov = imessage_provenance(sender_handle, timestamp_unix, in_group_chat);
    return ingest_synthesized(model, buf, n, /*from_user=*/true, timestamp_unix, &prov);
}

hu_error_t hu_imessage_ingest_reply(struct hu_personal_model *model, const char *sender_handle,
                                    bool is_from_me, const char *parent_text_preview,
                                    const char *reply_text, int64_t timestamp_unix,
                                    bool in_group_chat) {
    if (!model || !reply_text)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[640];
    size_t n = hu_imessage_synth_reply(sender_handle, is_from_me, parent_text_preview, reply_text,
                                       buf, sizeof(buf));
    if (n == 0)
        return HU_OK;

    hu_provenance_t prov = imessage_provenance(sender_handle, timestamp_unix, in_group_chat);
    return ingest_synthesized(model, buf, n, /*from_user=*/true, timestamp_unix, &prov);
}

hu_error_t hu_imessage_ingest_balloon(struct hu_personal_model *model, const char *sender_handle,
                                      bool is_from_me, hu_imessage_balloon_kind_t kind,
                                      const char *detail, int64_t timestamp_unix,
                                      bool in_group_chat) {
    if (!model)
        return HU_ERR_INVALID_ARGUMENT;

    char buf[512];
    size_t n = hu_imessage_synth_balloon(sender_handle, is_from_me, kind, detail, buf, sizeof(buf));
    if (n == 0)
        return HU_OK;

    hu_provenance_t prov = imessage_provenance(sender_handle, timestamp_unix, in_group_chat);
    return ingest_synthesized(model, buf, n, /*from_user=*/true, timestamp_unix, &prov);
}
