#ifndef HU_TTS_OPENER_GATE_H
#define HU_TTS_OPENER_GATE_H

/* Stop every voice memo to someone opening with the same reaction word.
 * Ported from voiceai's opener gate (2026-09-27): live replies opened 3-4 of
 * every 4 with "Oh"/"Ugh"/"Ha"/"Yeah" despite a prompt rule against it. A
 * reaction word now and then is human; on every memo it is a tic. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_OPENER_EVERY      3
#define HU_OPENER_GATE_SLOTS 32

/* Drop leading reaction words (oh, ugh, ha, yeah, yep, hmm, mm, ah, aw, wow,
 * whoa — not "well", which opens real content) that follow any leading
 * <tag/> or [bracket] markup, and capitalize what follows. Returns the length
 * written to out, or 0 when nothing was stripped (no opener, nothing
 * speakable after it, or out too small). */
size_t hu_opener_strip(const char *in, size_t len, char *out, size_t cap);

/* Per-recipient memory of when an opener was last kept. */
typedef struct {
    struct {
        char key[63];
        uint8_t key_len;
        uint8_t since_kept;
        uint32_t last_use;
    } slot[HU_OPENER_GATE_SLOTS];
    uint32_t tick;
    uint8_t every;
} hu_opener_gate_t;

void hu_opener_gate_init(hu_opener_gate_t *g, uint8_t every);

/* Call only for a memo that opens with a reaction word. True: keep it (and
 * start counting again); false: strip it. The first memo to a recipient may
 * keep one; after that, `every` are stripped before the next is kept. */
bool hu_opener_gate_keep(hu_opener_gate_t *g, const char *key, size_t key_len);

#endif
