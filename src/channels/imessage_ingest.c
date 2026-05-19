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
#include "human/channels/imessage.h"
#include "human/memory/fact_extract.h"
#include "human/memory/personal_model.h"
#include "human/util/bplist.h"
#include "human/util/typedstream.h"

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

/* Phase 2 gap-closer (the "option b" from the prior gap analysis):
 * construct a hu_heuristic_fact_t DIRECTLY from a reaction event and
 * merge it into the personal model, bypassing the text→extract round-
 * trip that was producing zero facts because the extractor only matches
 * first-person user patterns ("i like X").
 *
 * The fact we record:
 *   subject   = sender_handle ("Alice")
 *   predicate = kind-derived verb ("reacted_with_love_to", "laughed_at")
 *   object    = target_text_preview (truncated to fit HU_FACT_MAX_FIELD)
 *
 * After this lands, the prior documented-gap test must flip: instead of
 * asserting fact_count == 0 on the synthesis text, the new contract is
 * "after ingest_reaction, model->fact_count >= 1 with the contact as
 * subject." */
static const char *reaction_predicate(hu_reaction_kind_t kind) {
    switch (kind) {
    case HU_REACTION_LOVE:
        return "reacted_with_love_to";
    case HU_REACTION_LIKE:
        return "reacted_with_like_to";
    case HU_REACTION_DISLIKE:
        return "reacted_with_dislike_to";
    case HU_REACTION_LAUGH:
        return "laughed_at";
    case HU_REACTION_EMPHASIZE:
        return "emphasized";
    case HU_REACTION_KIND_QUESTION:
        return "questioned";
    case HU_REACTION_KIND_CUSTOM_EMOJI:
        return "reacted_with_emoji_to";
    case HU_REACTION_UNKNOWN:
    default:
        return "reacted_to";
    }
}

static hu_error_t construct_and_merge_reaction_fact(hu_personal_model_t *model,
                                                    const hu_reaction_event_t *event,
                                                    const char *target_text_preview,
                                                    const hu_provenance_t *prov) {
    if (!model || !event)
        return HU_ERR_INVALID_ARGUMENT;
    /* Skip if we don't know who reacted — a NULL subject is useless. */
    if (!event->sender_handle || !event->sender_handle[0])
        return HU_OK;
    /* Skip removals: a retraction is a negative-signal event, harder to
     * model as a positive (subject, predicate, object) triple. The
     * existing synthesis path already feeds the model a removal narration
     * for style metrics; we don't add a fact. */
    if (event->is_removal)
        return HU_OK;

    hu_fact_extract_result_t result;
    memset(&result, 0, sizeof(result));

    hu_heuristic_fact_t *fact = &result.facts[0];
    fact->type = HU_KNOWLEDGE_PROPOSITIONAL;
    /* strncpy + explicit NUL: defensive against handles longer than
     * HU_FACT_MAX_FIELD (256). */
    strncpy(fact->subject, event->sender_handle, sizeof(fact->subject) - 1);
    fact->subject[sizeof(fact->subject) - 1] = '\0';
    const char *pred = reaction_predicate(event->kind);
    strncpy(fact->predicate, pred, sizeof(fact->predicate) - 1);
    fact->predicate[sizeof(fact->predicate) - 1] = '\0';
    const char *object_str = (target_text_preview && target_text_preview[0]) ? target_text_preview
                                                                             : "an unknown message";
    strncpy(fact->object, object_str, sizeof(fact->object) - 1);
    fact->object[sizeof(fact->object) - 1] = '\0';
    fact->confidence = 0.75f; /* heuristic-but-directly-observed */
    strncpy(fact->source_hint, "reaction_ingest", sizeof(fact->source_hint) - 1);
    fact->source_hint[sizeof(fact->source_hint) - 1] = '\0';
    fact->last_seen_at = event->timestamp_unix;
    if (prov)
        fact->provenance = *prov;
    result.fact_count = 1;
    result.propositional_count = 1;

    /* merge_facts_checked respects trust-tier overwrite rules and routes
     * THIRD_PARTY facts into the pending-quarantine queue rather than
     * stable facts[]. Group-chat reactions therefore don't pollute the
     * main fact set without a corroborating user-direct signal. */
    return hu_personal_model_merge_facts_checked(model, &result, prov);
}

hu_error_t hu_reaction_ingest_personal_model(struct hu_personal_model *model,
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

    /* Two-track ingest (post gap-fix):
     *
     * 1. Text path — keeps style metrics + interaction_count updating.
     *    from_user=true is documented at personal_model.c:942; the
     *    provenance tier still drives the MINJA gate on group-chat
     *    content.
     *
     * 2. Direct-fact path (the gap-closer) — constructs a structured
     *    (subject, predicate, object) triple and merges via
     *    hu_personal_model_merge_facts_checked. Bypasses the text-
     *    extraction round-trip that produced zero facts for third-
     *    person narration. THIS is what makes the personal model
     *    actually learn about contacts from reactions.
     *
     * Both are best-effort; track 2's failure doesn't propagate (we
     * still want style metrics to land even if fact merge fails). */
    hu_error_t text_err =
        ingest_synthesized(model, buf, n, /*from_user=*/true, event->timestamp_unix, &prov);
    (void)construct_and_merge_reaction_fact(model, event, target_text_preview, &prov);
    return text_err;
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

/* ── Phase 3: bplist payload extractors ───────────────────────────── */

size_t hu_imessage_extract_audio_transcript(const unsigned char *payload_blob, size_t payload_len,
                                            char *out, size_t cap) {
    if (!payload_blob || payload_len == 0 || !out || cap == 0)
        return 0;
    out[0] = '\0';

    hu_bplist_t *p = NULL;
    if (hu_bplist_parse(payload_blob, payload_len, &p) != HU_OK)
        return 0;

    /* Try both schema versions: older "transcribed_text", newer
     * "transcription". The root is typically a dict; some iOS versions
     * wrap the dict in an NSKeyedArchiver $objects array. We try the
     * direct path first and let the caller handle archiver-wrapped
     * blobs if needed. */
    size_t root = hu_bplist_root(p);
    size_t n = 0;

    if (hu_bplist_kind(p, root) == HU_BPLIST_DICT) {
        size_t v = hu_bplist_dict_lookup(p, root, "transcribed_text");
        if (v == SIZE_MAX)
            v = hu_bplist_dict_lookup(p, root, "transcription");
        if (v != SIZE_MAX && hu_bplist_kind(p, v) == HU_BPLIST_STRING)
            n = hu_bplist_get_string(p, v, out, cap);
    }

    hu_bplist_free(p);
    return n;
}

/* Resolve a single edit-event's text. The "t" key may either be:
 *   - a plain string  → copy directly
 *   - a typedstream data blob → run hu_imessage_extract_attributed_body
 * Anything else yields zero bytes written. */
static size_t edit_event_extract_text(const hu_bplist_t *p, size_t event_idx, char *out,
                                      size_t cap) {
    if (cap == 0)
        return 0;
    out[0] = '\0';
    if (hu_bplist_kind(p, event_idx) != HU_BPLIST_DICT)
        return 0;
    size_t t = hu_bplist_dict_lookup(p, event_idx, "t");
    if (t == SIZE_MAX)
        return 0;
    hu_bplist_kind_t k = hu_bplist_kind(p, t);
    if (k == HU_BPLIST_STRING)
        return hu_bplist_get_string(p, t, out, cap);
    if (k == HU_BPLIST_DATA) {
        size_t blen = 0;
        const unsigned char *b = hu_bplist_get_data(p, t, &blen);
        if (b && blen > 0)
            return hu_imessage_extract_attributed_body(b, blen, out, cap);
    }
    return 0;
}

size_t hu_imessage_extract_edit_chain(const unsigned char *summary_blob, size_t summary_len,
                                      char *out_buf, size_t out_count_max, size_t entry_cap) {
    if (!summary_blob || summary_len == 0 || !out_buf || out_count_max == 0 || entry_cap == 0)
        return 0;

    hu_bplist_t *p = NULL;
    if (hu_bplist_parse(summary_blob, summary_len, &p) != HU_OK)
        return 0;

    size_t written = 0;
    size_t root = hu_bplist_root(p);
    if (hu_bplist_kind(p, root) != HU_BPLIST_DICT) {
        hu_bplist_free(p);
        return 0;
    }
    size_t ec = hu_bplist_dict_lookup(p, root, "ec");
    if (ec == SIZE_MAX || hu_bplist_kind(p, ec) != HU_BPLIST_DICT) {
        hu_bplist_free(p);
        return 0;
    }

    /* Iterate parts in numeric ascending order ("0", "1", ...) up to
     * 16. Apple keeps at most ~5 edits per part, so 16 is a generous
     * upper bound that avoids unbounded loops. */
    for (size_t part = 0; part < 16 && written < out_count_max; part++) {
        char key[8];
        snprintf(key, sizeof(key), "%zu", part);
        size_t arr = hu_bplist_dict_lookup(p, ec, key);
        if (arr == SIZE_MAX)
            continue;
        if (hu_bplist_kind(p, arr) != HU_BPLIST_ARRAY)
            continue;
        size_t count = hu_bplist_array_count(p, arr);
        for (size_t i = 0; i < count && written < out_count_max; i++) {
            size_t event = hu_bplist_array_at(p, arr, i);
            if (event == SIZE_MAX)
                continue;
            char *slot = out_buf + written * entry_cap;
            size_t n = edit_event_extract_text(p, event, slot, entry_cap);
            if (n > 0)
                written++;
        }
    }

    hu_bplist_free(p);
    return written;
}

/* ── Phase 4: typedstream attribute-run synthesis ───────────────────── */

size_t hu_imessage_synth_attributed_message(const unsigned char *blob, size_t blob_len,
                                            const char *sender_handle, bool is_from_me, char *out,
                                            size_t out_cap) {
    if (!blob || blob_len == 0 || !out || out_cap < 2)
        return 0;
    out[0] = '\0';

    char text[1024];
    hu_attribute_run_t runs[16];
    size_t runs_n = 0;
    hu_error_t err = hu_imessage_extract_attribute_runs(blob, blob_len, text, sizeof(text), runs,
                                                        sizeof(runs) / sizeof(runs[0]), &runs_n);
    if (err != HU_OK)
        return 0;
    if (text[0] == '\0')
        return 0;

    /* OTP / 2FA messages: refuse to ingest. The presence of a one-time-
     * code attribute run is Apple's marker that this text is a credential
     * the user copy-pastes — not persona signal. */
    if (hu_imessage_runs_contain_otp(runs, runs_n))
        return 0;

    /* Find first mention (if any) for inline rendering. */
    const hu_attribute_run_t *m = hu_imessage_runs_first_mention(runs, runs_n);

    const char *who = (is_from_me || !sender_handle || !sender_handle[0]) ? "I" : sender_handle;
    const char *verb = (is_from_me) ? "said" : "said";

    int n;
    if (m && m->detail[0]) {
        n = snprintf(out, out_cap, "%s %s: \"%s\" (@%s)", who, verb, text, m->detail);
    } else {
        n = snprintf(out, out_cap, "%s %s: \"%s\"", who, verb, text);
    }
    if (n < 0)
        return 0;
    if ((size_t)n >= out_cap)
        return out_cap - 1;
    return (size_t)n;
}
