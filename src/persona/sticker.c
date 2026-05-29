/* Feature-test macros must precede the first include so libc's <features.h>
 * exposes the right symbols. dirent's d_type and the BSD/XOPEN surface need a
 * non-strict-ANSI source level; this is the same proven combination used by
 * the rest of the codebase. Uniform random goes through hu_rand_uniform
 * (human/core/rand.h) so we never touch arc4random directly — musl does not
 * implement the arc4random family at all. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "human/persona/sticker.h"
#include "human/core/log.h"
#include "human/core/rand.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Path to override the LRU file location (for testing). */
/* Test-only LRU path override. Stored in a static buffer (NOT just a pointer
 * to caller memory) so the path survives after the caller's stack frame is
 * destroyed. Earlier draft stored the pointer directly — caused a dangling
 * pointer and an LRU file written to a garbage filename in the worktree
 * root when the test's local `lru_path[512]` buffer went out of scope. */
static char test_lru_path_buf[512];
static const char *test_lru_path = NULL;

/* Parse a filename of the form <context>-<mood>-<tone>_<seq>.<ext>
 * Returns true if all parts are recognized, false otherwise. */
static bool parse_filename_tags(const char *filename, const char **out_context,
                                const char **out_mood, const char **out_tone,
                                unsigned int *out_seq) {
    if (!filename)
        return false;

    /* Make a mutable copy since we'll modify it with strtok. */
    char *buf = malloc(strlen(filename) + 1);
    if (!buf)
        return false;
    strcpy(buf, filename);

    /* Remove extension. */
    char *dot = strrchr(buf, '.');
    if (!dot) {
        free(buf);
        return false;
    }
    *dot = '\0';

    /* Split on underscore to separate tags from seq. */
    char *tags_part = buf;
    char *seq_part = strchr(buf, '_');
    if (!seq_part) {
        free(buf);
        return false;
    }
    *seq_part = '\0';
    seq_part++;

    /* Parse seq number (3 digits). */
    unsigned int seq = 0;
    if (sscanf(seq_part, "%u", &seq) != 1 || seq < 1 || seq > 999) {
        free(buf);
        return false;
    }

    /* Split tags on '-' to get context, mood, tone. */
    char *saveptr = NULL;
    const char *context = strtok_r(tags_part, "-", &saveptr);
    const char *mood = strtok_r(NULL, "-", &saveptr);
    const char *tone = strtok_r(NULL, "-", &saveptr);

    if (!context || !mood || !tone) {
        free(buf);
        return false;
    }

    /* Validate vocabulary. */
    bool context_valid = (strcmp(context, "casual") == 0 || strcmp(context, "formal") == 0 ||
                          strcmp(context, "intimate") == 0 || strcmp(context, "playful") == 0);
    bool mood_valid = (strcmp(mood, "happy") == 0 || strcmp(mood, "acknowledgment") == 0 ||
                       strcmp(mood, "laugh") == 0 || strcmp(mood, "support") == 0 ||
                       strcmp(mood, "apology") == 0 || strcmp(mood, "gratitude") == 0);
    bool tone_valid =
        (strcmp(tone, "warm") == 0 || strcmp(tone, "dry") == 0 || strcmp(tone, "earnest") == 0);

    if (!context_valid || !mood_valid || !tone_valid) {
        free(buf);
        return false;
    }

    /* Return pointers into a new allocation (the old buf is freed). */
    *out_context = malloc(strlen(context) + 1);
    *out_mood = malloc(strlen(mood) + 1);
    *out_tone = malloc(strlen(tone) + 1);

    if (!*out_context || !*out_mood || !*out_tone) {
        free((void *)*out_context);
        free((void *)*out_mood);
        free((void *)*out_tone);
        free(buf);
        return false;
    }

    strcpy((char *)*out_context, context);
    strcpy((char *)*out_mood, mood);
    strcpy((char *)*out_tone, tone);
    *out_seq = seq;

    free(buf);
    return true;
}

/* Load LRU state from disk. Returns an array of strings (malloced),
 * count in *out_count. Returns NULL on error or if file doesn't exist. */
static char **load_lru(const char *lru_path, size_t *out_count, size_t max_entries) {
    if (!lru_path) {
        *out_count = 0;
        return NULL;
    }

    FILE *f = fopen(lru_path, "r");
    if (!f) {
        *out_count = 0;
        return NULL;
    }

    char **entries = malloc(max_entries * sizeof(char *));
    if (!entries) {
        fclose(f);
        return NULL;
    }

    size_t count = 0;
    char line[1024];
    while (count < max_entries && fgets(line, sizeof(line), f)) {
        /* Strip newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }
        if (len == 0)
            continue;

        entries[count] = malloc(len + 1);
        if (!entries[count]) {
            for (size_t i = 0; i < count; i++) {
                free(entries[i]);
            }
            free(entries);
            fclose(f);
            return NULL;
        }
        strcpy(entries[count], line);
        count++;
    }

    fclose(f);
    *out_count = count;
    return entries;
}

/* Save LRU state to disk. */
static bool save_lru(const char *lru_path, const char **entries, size_t count) {
    if (!lru_path)
        return true;

    FILE *f = fopen(lru_path, "w");
    if (!f)
        return false;

    for (size_t i = 0; i < count; i++) {
        fprintf(f, "%s\n", entries[i]);
    }

    fclose(f);
    return true;
}

/* Check if filename is in the first recent_head entries of LRU. */
static bool is_in_recent_lru(const char *filename, const char **lru, size_t lru_count,
                             size_t recent_head) {
    if (!filename || !lru || lru_count == 0)
        return false;

    size_t check_count = (recent_head < lru_count) ? recent_head : lru_count;
    for (size_t i = 0; i < check_count; i++) {
        if (strcmp(filename, lru[i]) == 0)
            return true;
    }
    return false;
}

bool hu_persona_pick_sticker(const char *sticker_dir, const hu_sticker_query_t *q, char *out_path,
                             size_t out_cap) {
    if (!sticker_dir || !q || !out_path || out_cap == 0) {
        return false;
    }

    /* Open and scan directory. */
    DIR *dir = opendir(sticker_dir);
    if (!dir) {
        return false;
    }

    /* Collect matching files. */
    char **matches = malloc(1000 * sizeof(char *)); /* Max 1000 sticker files. */
    if (!matches) {
        closedir(dir);
        return false;
    }
    size_t match_count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Check file extension. */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || (strcmp(ext, ".png") != 0 && strcmp(ext, ".heic") != 0 &&
                     strcmp(ext, ".jpg") != 0 && strcmp(ext, ".jpeg") != 0)) {
            continue;
        }

        /* Parse tags from filename. */
        const char *file_context = NULL;
        const char *file_mood = NULL;
        const char *file_tone = NULL;
        unsigned int file_seq = 0;

        if (!parse_filename_tags(entry->d_name, &file_context, &file_mood, &file_tone, &file_seq)) {
            /* Invalid filename, skip. */
            continue;
        }

        /* Check if file matches query (NULL = match any). */
        bool match = true;
        if (q->context_tag && strcmp(file_context, q->context_tag) != 0) {
            match = false;
        }
        if (q->mood_tag && strcmp(file_mood, q->mood_tag) != 0) {
            match = false;
        }
        if (q->tone_tag && strcmp(file_tone, q->tone_tag) != 0) {
            match = false;
        }

        free((void *)file_context);
        free((void *)file_mood);
        free((void *)file_tone);

        if (!match)
            continue;

        /* Add to matches. */
        if (match_count < 1000) {
            matches[match_count] = malloc(strlen(entry->d_name) + 1);
            if (matches[match_count]) {
                strcpy(matches[match_count], entry->d_name);
                match_count++;
            }
        }
    }

    closedir(dir);

    if (match_count == 0) {
        free(matches);
        return false;
    }

    /* Load current LRU. */
    const char *lru_path = test_lru_path;
    if (!lru_path) {
        static char default_lru[1024];
        snprintf(default_lru, sizeof(default_lru), "%s/../state/sticker_lru.txt", sticker_dir);
        lru_path = default_lru;
    }

    size_t lru_count = 0;
    char **lru = load_lru(lru_path, &lru_count, 100);

    /* Partition matches: preferred set (NOT in recent LRU) and fallback set. */
    char **preferred = malloc(match_count * sizeof(char *));
    char **fallback = malloc(match_count * sizeof(char *));
    size_t pref_count = 0;
    size_t fall_count = 0;

    if (!preferred || !fallback) {
        free(preferred);
        free(fallback);
        for (size_t i = 0; i < match_count; i++) {
            free(matches[i]);
        }
        free(matches);
        for (size_t i = 0; i < lru_count; i++) {
            free(lru[i]);
        }
        free(lru);
        return false;
    }

    const size_t recent_head = 10; /* Consider first 10 as "recent". */
    for (size_t i = 0; i < match_count; i++) {
        if (is_in_recent_lru(matches[i], (const char **)lru, lru_count, recent_head)) {
            fallback[fall_count++] = matches[i];
        } else {
            preferred[pref_count++] = matches[i];
        }
    }

    /* Pick from preferred set if available, else fallback. */
    char **pick_set = (pref_count > 0) ? preferred : fallback;
    size_t pick_count = (pref_count > 0) ? pref_count : fall_count;

    if (pick_count == 0) {
        free(preferred);
        free(fallback);
        for (size_t i = 0; i < match_count; i++) {
            free(matches[i]);
        }
        free(matches);
        for (size_t i = 0; i < lru_count; i++) {
            free(lru[i]);
        }
        free(lru);
        return false;
    }

    /* Uniform random pick from the set. */
    unsigned int idx = hu_rand_uniform((unsigned int)pick_count);
    const char *picked = pick_set[idx];

    /* Build absolute path. */
    int ret = snprintf(out_path, out_cap, "%s/%s", sticker_dir, picked);
    if (ret < 0 || (size_t)ret >= out_cap) {
        /* Path too long. */
        free(preferred);
        free(fallback);
        for (size_t i = 0; i < match_count; i++) {
            free(matches[i]);
        }
        free(matches);
        for (size_t i = 0; i < lru_count; i++) {
            free(lru[i]);
        }
        free(lru);
        return false;
    }

    /* Update LRU: remove picked from anywhere, insert at front, cap at 100. */
    char **new_lru = malloc(101 * sizeof(char *));
    if (!new_lru) {
        free(preferred);
        free(fallback);
        for (size_t i = 0; i < match_count; i++) {
            free(matches[i]);
        }
        free(matches);
        for (size_t i = 0; i < lru_count; i++) {
            free(lru[i]);
        }
        free(lru);
        return false;
    }

    /* New entry (just the filename, not full path). */
    new_lru[0] = malloc(strlen(picked) + 1);
    if (!new_lru[0]) {
        free(new_lru);
        free(preferred);
        free(fallback);
        for (size_t i = 0; i < match_count; i++) {
            free(matches[i]);
        }
        free(matches);
        for (size_t i = 0; i < lru_count; i++) {
            free(lru[i]);
        }
        free(lru);
        return false;
    }
    strcpy(new_lru[0], picked);
    size_t new_lru_count = 1;

    /* Copy old LRU entries, skipping duplicates of the picked file. */
    for (size_t i = 0; i < lru_count && new_lru_count < 100; i++) {
        if (strcmp(lru[i], picked) != 0) {
            new_lru[new_lru_count] = lru[i];
            new_lru_count++;
        } else {
            free(lru[i]);
        }
    }

    /* Save updated LRU. */
    save_lru(lru_path, (const char **)new_lru, new_lru_count);

    /* Cleanup. */
    free(preferred);
    free(fallback);
    for (size_t i = 0; i < match_count; i++) {
        free(matches[i]);
    }
    free(matches);
    for (size_t i = 0; i < new_lru_count; i++) {
        free(new_lru[i]);
    }
    free(new_lru);
    free(lru);

    return true;
}

void hu_persona_sticker_set_test_lru_path(const char *path) {
    if (path) {
        strncpy(test_lru_path_buf, path, sizeof(test_lru_path_buf) - 1);
        test_lru_path_buf[sizeof(test_lru_path_buf) - 1] = '\0';
        test_lru_path = test_lru_path_buf;
    } else {
        test_lru_path = NULL;
    }
}
