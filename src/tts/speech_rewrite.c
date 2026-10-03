/* F1 S1: rewrite a voice reply for the ear. See
 * include/human/tts/speech_rewrite.h. */
#include "human/tts/speech_rewrite.h"

#include "human/core/json.h"
#include "human/core/paths.h"
#include "human/persona.h"
#include "human/tts/speech_text.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Ferni's mechanics (voiceai voice-guidance.md + dynamic-speech-guidance.ts),
 * rewritten for a memo from this person to someone they know. The character
 * is the persona's, not Ferni's. Kept < 4095 bytes (ISO C string minimum). */
static const char k_speak_it[] =
    "You turn a text message into what the same person would actually say out loud in a "
    "quick voice memo to someone they know.\n"
    "- Say the same thing the way you'd say it out loud: always use contractions; join related "
    "thoughts with and, so, but, because, and mix longer and shorter sentences (a run of short "
    "sentences sounds stop, pause, stop); \"...\" only for trailing off, no em dashes.\n"
    "- A natural reaction word is fine when it fits (oh, wait, yeah, ha). Don't force one.\n"
    "- Never start with \"Well\", \"So\", \"Hmm\" or \"Um\". Never say \"good question\".\n"
    "- No emoji, asterisks, brackets, stage directions, or narrated actions or thinking.\n"
    "- Add nothing that isn't in the text: no new names, times, numbers, places, plans, "
    "promises or questions. Keep every question that is there.\n"
    "- Keep it about as long as the text.\n"
    "- It replies to the message shown; don't answer that message again.\n"
    "Output only the words to speak.\n";

#define REWRITE_LIST_MAX 12

hu_speech_rewrite_mode_t hu_speech_rewrite_mode_parse(const char *s) {
    if (s && strcmp(s, "live") == 0)
        return HU_SPEECH_REWRITE_LIVE;
    if (s && strcmp(s, "shadow") == 0)
        return HU_SPEECH_REWRITE_SHADOW;
    return HU_SPEECH_REWRITE_OFF;
}

static void put(char *out, size_t cap, size_t *o, const char *s, size_t n) {
    if (*o + 1 >= cap)
        return;
    size_t room = cap - *o - 1;
    if (n > room)
        n = room;
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
}

static void put_list(char *out, size_t cap, size_t *o, const char *label, char *const *items,
                     size_t count) {
    if (!items || count == 0)
        return;
    put(out, cap, o, label, strlen(label));
    for (size_t i = 0; i < count && i < REWRITE_LIST_MAX; i++) {
        if (!items[i])
            continue;
        if (i > 0)
            put(out, cap, o, ", ", 2);
        put(out, cap, o, items[i], strlen(items[i]));
    }
    put(out, cap, o, "\n", 1);
}

size_t hu_speech_rewrite_system_prompt(const struct hu_persona *p, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    size_t o = 0;
    out[0] = '\0';
    put(out, cap, &o, k_speak_it, sizeof(k_speak_it) - 1);
    if (p) {
        put(out, cap, &o, "How this person talks:\n", 23);
        put_list(out, cap, &o, "Words they use: ", p->preferred_vocab, p->preferred_vocab_count);
        put_list(out, cap, &o, "Slang they use: ", p->slang, p->slang_count);
        put_list(out, cap, &o, "Words they avoid: ", p->avoided_vocab, p->avoided_vocab_count);
        put_list(out, cap, &o, "Never: ", p->anti_patterns, p->anti_patterns_count);
    }
    return o;
}

/* Models sometimes wrap the answer in quotes or add a trailing newline. */
static void unwrap(const char **s, size_t *n) {
    while (*n > 0 && ((*s)[0] == ' ' || (*s)[0] == '\n')) {
        (*s)++;
        (*n)--;
    }
    while (*n > 0 && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\n' || (*s)[*n - 1] == '\r'))
        (*n)--;
    if (*n >= 2 && (*s)[0] == '"' && (*s)[*n - 1] == '"') {
        (*s)++;
        *n -= 2;
    }
}

hu_error_t hu_speech_prepare(hu_allocator_t *alloc, const hu_provider_t *provider,
                             const char *model, size_t model_len, const struct hu_persona *persona,
                             hu_speech_rewrite_mode_t mode, const char *reply, size_t reply_len,
                             const char *inbound, size_t inbound_len, hu_speech_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    bool cue = false;
    out->spoken_len = hu_speech_cleanup(reply, reply_len, out->spoken, sizeof(out->spoken), &cue);
    out->laughter_cue = cue;
    if (out->spoken_len == 0) {
        out->reason = "nothing_to_say";
        return HU_OK;
    }
    if (mode == HU_SPEECH_REWRITE_OFF) {
        out->reason = "off";
        return HU_OK;
    }
    if (!provider || !provider->vtable || !provider->vtable->chat_with_system) {
        out->reason = "no_provider";
        return HU_OK;
    }

    char sys[4096];
    size_t sys_len = hu_speech_rewrite_system_prompt(persona, sys, sizeof(sys));
    char msg[3072];
    size_t in_n = inbound ? (inbound_len > 800 ? 800 : inbound_len) : 0;
    int mn = snprintf(msg, sizeof(msg), "Message they're replying to: %.*s\n\nTheir text reply: %s",
                      (int)in_n, inbound ? inbound : "", out->spoken);
    if (mn <= 0 || (size_t)mn >= sizeof(msg)) {
        out->reason = "provider_error";
        return HU_OK;
    }

    char *raw = NULL;
    size_t raw_len = 0;
    hu_error_t err = provider->vtable->chat_with_system(
        provider->ctx, alloc, sys, sys_len, msg, (size_t)mn, model, model_len, 0.7, &raw, &raw_len);
    size_t alloc_len = raw_len;
    bool rcue = false;
    size_t rw_len = 0;
    if (err == HU_OK && raw && raw_len > 0) {
        const char *b = raw;
        size_t n = raw_len;
        unwrap(&b, &n);
        rw_len = hu_speech_cleanup(b, n, out->rewritten, sizeof(out->rewritten), &rcue);
    }
    if (raw)
        alloc->free(alloc->ctx, raw, alloc_len + 1);
    if (err != HU_OK) {
        out->reason = "provider_error";
        return HU_OK;
    }
    if (rw_len == 0) {
        out->reason = "empty";
        return HU_OK;
    }

    hu_speech_drift_t drift =
        hu_speech_drift_check(out->spoken, out->spoken_len, out->rewritten, rw_len);
    if (drift != HU_SPEECH_DRIFT_OK) {
        out->reason = hu_speech_drift_name(drift);
        return HU_OK;
    }
    if (mode == HU_SPEECH_REWRITE_SHADOW) {
        out->reason = "shadow";
        return HU_OK;
    }
    memcpy(out->spoken, out->rewritten, rw_len + 1);
    out->spoken_len = rw_len;
    out->used_rewrite = true;
    out->laughter_cue = cue || rcue;
    out->reason = "live";
    return HU_OK;
}

hu_error_t hu_speech_shadow_line(hu_allocator_t *alloc, const hu_speech_result_t *r, int64_t ts,
                                 char **out, size_t *out_len) {
    if (!alloc || !r || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    hu_json_buf_t b;
    hu_error_t err = hu_json_buf_init(&b, alloc);
    if (err != HU_OK)
        return err;
    const char *reason = r->reason ? r->reason : "";
    if ((err = hu_json_buf_append_raw(&b, "{", 1)) == HU_OK &&
        (err = hu_json_append_key_int(&b, "ts", 2, (long long)ts)) == HU_OK &&
        (err = hu_json_buf_append_raw(&b, ",", 1)) == HU_OK &&
        (err = hu_json_append_key_bool(&b, "used", 4, r->used_rewrite)) == HU_OK &&
        (err = hu_json_buf_append_raw(&b, ",", 1)) == HU_OK &&
        (err = hu_json_append_key_value(&b, "reason", 6, reason, strlen(reason))) == HU_OK &&
        (err = hu_json_buf_append_raw(&b, ",", 1)) == HU_OK &&
        (err = hu_json_append_key_value(&b, "spoken", 6, r->spoken, strlen(r->spoken))) == HU_OK &&
        (err = hu_json_buf_append_raw(&b, ",", 1)) == HU_OK &&
        (err = hu_json_append_key_value(&b, "rewritten", 9, r->rewritten, strlen(r->rewritten))) ==
            HU_OK)
        err = hu_json_buf_append_raw(&b, "}", 1);
    if (err != HU_OK) {
        hu_json_buf_free(&b);
        return err;
    }
    char *line = alloc->alloc(alloc->ctx, b.len + 1);
    if (!line) {
        hu_json_buf_free(&b);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memcpy(line, b.ptr, b.len);
    line[b.len] = '\0';
    *out = line;
    *out_len = b.len;
    hu_json_buf_free(&b);
    return HU_OK;
}

void hu_speech_shadow_record(hu_allocator_t *alloc, const hu_speech_result_t *r) {
#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)alloc;
    (void)r;
#else
    if (!alloc || !r)
        return;
    char dir[512];
    if (hu_paths_state(dir, sizeof(dir), "voice") <= 0)
        return;
    (void)mkdir(dir, 0700);
    if (hu_paths_state(dir, sizeof(dir), "voice/shadow") <= 0)
        return;
    (void)mkdir(dir, 0700);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char day[16];
    strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
    char path[640];
    if (hu_paths_state(path, sizeof(path), "voice/shadow/%s.jsonl", day) <= 0)
        return;
    char *line = NULL;
    size_t len = 0;
    if (hu_speech_shadow_line(alloc, r, (int64_t)now, &line, &len) != HU_OK)
        return;
    FILE *f = fopen(path, "a");
    if (f) {
        (void)fwrite(line, 1, len, f);
        (void)fputc('\n', f);
        fclose(f);
    }
    alloc->free(alloc->ctx, line, len + 1);
#endif
}
