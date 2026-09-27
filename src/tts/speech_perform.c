/* Voice direction D1; see include/human/tts/speech_perform.h. */
#include "human/tts/speech_perform.h"
#include "human/tts/speech_text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define PERFORM_SYS_CAP 16384

static const char k_cast[] =
    "You are an actor voicing a real person in a play. Your line is recorded as a voice memo "
    "on a phone and played to the person it is meant for. Every word you write is spoken aloud "
    "by a voice clone of the speaker, so write only what the speaker would actually say.\n\n"
    "THE LINE\n"
    "- Say what the intent says, the way the speaker would say it out loud to this listener: "
    "contractions, short sentences; fragments are fine.\n"
    "- Add nothing new: no new names, times, numbers, days, places, plans, promises or "
    "questions. Keep every question the intent asks. About as long as the intent.\n"
    "- Never start with \"Well\", \"So\", \"Hmm\" or \"Um\". Never say \"good question\".\n"
    "- No asterisks, parentheses, emoji, stage directions (\"warmly\") or narrated actions "
    "(\"*laughs*\").\n\n"
    "DIRECTING YOUR DELIVERY\n"
    "Direct your own voice with these tags, placed right before the words they affect:\n"
    "  <emotion value=\"NAME\"/>  NAME is one of the emotions listed below\n"
    "  <speed ratio=\"0.85\"/> to <speed ratio=\"1.10\"/>  slower for weight, faster for "
    "excitement\n"
    "  <volume ratio=\"0.85\"/> to <volume ratio=\"1.15\"/>  softer for tender moments\n"
    "  <break time=\"300ms\"/>  a real pause, 100ms to 800ms\n"
    "  [laughter]  only when the moment is actually funny\n"
    "Tags are for emphasis, not every sentence: at most one emotion change every two "
    "sentences, one laugh, one speed change and one volume change. The emotion must match the "
    "words; a tag that fights the words sounds fake.\n"
    "Emotions: ";

/* Placeholder voice DNA — replaced from Seth's calibration takes (spec
 * §Calibration). Deliberately generic: no invented catchphrases. */
static const char k_voice[] =
    "\n\nTHE SPEAKER'S VOICE\n"
    "- Natural, unhurried pace. Usually content or affectionate; excited when something is "
    "genuinely good; sympathetic when it's hard. Rarely dramatic.\n"
    "- Reacts the way people do out loud (\"oh\", \"ha\", \"yeah\") only when it fits.\n\n"
    "EXAMPLES\n"
    "Intent: that's amazing, so proud of you\n"
    "Line: <emotion value=\"excited\"/>Wait, that's amazing! <break time=\"250ms\"/>"
    "<emotion value=\"proud\"/>I'm so proud of you.\n"
    "Intent: ugh I'm sorry, that sounds rough\n"
    "Line: <emotion value=\"sympathetic\"/><speed ratio=\"0.92\"/>Oh, I'm sorry. That sounds "
    "really rough.\n"
    "Intent: haha you're ridiculous\n"
    "Line: [laughter] You're ridiculous.\n\n"
    "Output only the line.\n";

static size_t put(char *out, size_t cap, size_t o, const char *s) {
    size_t n = strlen(s);
    if (o >= cap || o + n >= cap)
        return cap;
    memcpy(out + o, s, n);
    out[o + n] = '\0';
    return o + n;
}

size_t hu_speech_perform_system_prompt(char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    size_t o = put(out, cap, 0, k_cast);
    size_t ne = hu_direction_emotion_count();
    for (size_t i = 0; o < cap && i < ne; i++) {
        o = put(out, cap, o, hu_direction_emotion_at(i));
        o = put(out, cap, o, i + 1 < ne ? ", " : ".");
    }
    o = put(out, cap, o, k_voice);
    return o >= cap ? 0 : o;
}

static const char *part_of_day(int h) {
    if (h >= 5 && h < 12)
        return "morning";
    if (h >= 12 && h < 17)
        return "afternoon";
    if (h >= 17 && h < 22)
        return "evening";
    return "night";
}

size_t hu_speech_perform_user_message(const hu_perform_scene_t *s, const char *intent,
                                      size_t intent_len, char *out, size_t cap) {
    static const char *const days[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                       "Thursday", "Friday", "Saturday"};
    if (!out || cap == 0 || !intent)
        return 0;
    const char *who = s && s->listener ? s->listener : "a friend";
    bool rel = s && s->relationship && s->relationship[0];
    bool day = s && s->weekday >= 0 && s->weekday < 7;
    int n = snprintf(out, cap,
                     "Scene: %s is recording a quick voice memo on the phone to %s%s%s%s. "
                     "It's %s%s%s.\n",
                     s && s->speaker ? s->speaker : "The speaker", who, rel ? " (" : "",
                     rel ? s->relationship : "", rel ? ")" : "", day ? days[s->weekday] : "",
                     day ? " " : "", part_of_day(s ? s->hour_local : 12));
    if (n < 0 || (size_t)n >= cap)
        return 0;
    size_t o = (size_t)n;
    if (s && s->inbound && s->inbound_len > 0) {
        n = snprintf(out + o, cap - o, "%s last said: \"%.*s\"\n", who,
                     (int)(s->inbound_len > 600 ? 600 : s->inbound_len), s->inbound);
        if (n < 0 || (size_t)n >= cap - o)
            return 0;
        o += (size_t)n;
    }
    n = snprintf(out + o, cap - o, "Intent (what the memo must say): \"%.*s\"\nSay the line.\n",
                 (int)intent_len, intent);
    if (n < 0 || (size_t)n >= cap - o)
        return 0;
    return o + (size_t)n;
}

/* Models wrap lines in quotes or prefix "Line:"; neither is spoken. */
static void trim_line(char *s, size_t *n) {
    size_t a = 0, b = *n;
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    if (b - a >= 5 && strncmp(s + a, "Line:", 5) == 0)
        a += 5;
    while (a < b && isspace((unsigned char)s[a]))
        a++;
    while (b > a && isspace((unsigned char)s[b - 1]))
        b--;
    if (b - a >= 2 && s[a] == '"' && s[b - 1] == '"') {
        a++;
        b--;
    }
    memmove(s, s + a, b - a);
    *n = b - a;
    s[*n] = '\0';
}

hu_error_t hu_speech_perform(hu_allocator_t *alloc, const hu_provider_t *provider,
                             const char *model, size_t model_len, const hu_perform_scene_t *scene,
                             const char *intent, size_t intent_len, hu_perform_result_t *out) {
    if (!alloc || !out || !intent)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->reason = "empty";
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system) {
        out->reason = "no_provider";
        return HU_OK;
    }
    char *sys = alloc->alloc(alloc->ctx, PERFORM_SYS_CAP);
    if (!sys)
        return HU_ERR_OUT_OF_MEMORY;
    char msg[2048];
    size_t sn = hu_speech_perform_system_prompt(sys, PERFORM_SYS_CAP);
    size_t mn = hu_speech_perform_user_message(scene, intent, intent_len, msg, sizeof(msg));
    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = HU_ERR_INVALID_ARGUMENT;
    if (sn > 0 && mn > 0)
        err = provider->vtable->chat_with_system(provider->ctx, alloc, sys, sn, msg, mn, model,
                                                 model_len, 0.7, &raw, &raw_len);
    alloc->free(alloc->ctx, sys, PERFORM_SYS_CAP);
    if (err != HU_OK || !raw) {
        out->reason = "provider_error";
        return HU_OK;
    }
    size_t alloc_len = raw_len; /* speech_rewrite.c's convention: free raw_len + 1 */
    trim_line(raw, &raw_len);
    hu_direction_verdict_t v = hu_direction_parse(raw, raw_len, NULL, &out->dir);
    alloc->free(alloc->ctx, raw, alloc_len + 1);
    if (v != HU_DIRECTION_OK) {
        out->reason = hu_direction_verdict_name(v);
        return HU_OK;
    }
    hu_speech_drift_t dr =
        hu_speech_drift_check(intent, intent_len, out->dir.words, out->dir.words_len);
    if (dr != HU_SPEECH_DRIFT_OK) {
        out->reason = hu_speech_drift_name(dr);
        return HU_OK;
    }
    out->ok = true;
    out->reason = "ok";
    return HU_OK;
}
