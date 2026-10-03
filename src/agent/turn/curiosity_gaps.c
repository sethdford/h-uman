/* src/agent/turn/curiosity_gaps.c — see include/human/agent/curiosity_gaps.h. */
#include "human/agent/curiosity_gaps.h"

#include "human/core/string.h" /* hu_str_contains_word_ci_n */

#include <pthread.h>
#include <string.h>

typedef struct gap_topic {
    hu_curiosity_topic_t topic;
    const char *const *words;
    const char *line;
} gap_topic_t;

/* Whole words that mark a note as being about the topic. Both singular and
 * plural are listed; matching is whole-word, so "networking" is not "work". */
static const char *const k_plans[] = {"plan",     "plans",   "planning", "trip",        "vacation",
                                      "holiday",  "weekend", "birthday", "wedding",     "party",
                                      "concert",  "visit",   "visiting", "appointment", "flight",
                                      "upcoming", NULL};
static const char *const k_work[] = {
    "work",    "working",  "job",       "jobs",      "boss",     "office",   "shift",
    "shifts",  "career",   "company",   "client",    "clients",  "coworker", "coworkers",
    "meeting", "meetings", "promotion", "interview", "school",   "class",    "classes",
    "college", "exam",     "exams",     "project",   "projects", NULL};
static const char *const k_people[] = {
    "mom",       "dad",        "mother",  "father",  "sister",   "brother", "siblings",
    "son",       "daughter",   "kid",     "kids",    "baby",     "husband", "wife",
    "boyfriend", "girlfriend", "partner", "family",  "grandma",  "grandpa", "aunt",
    "uncle",     "cousin",     "friend",  "friends", "roommate", NULL};
static const char *const k_interests[] = {
    "loves",   "likes",  "favorite", "hobby",   "music",    "song",   "songs",   "show",
    "shows",   "movie",  "movies",   "game",    "games",    "book",   "books",   "reading",
    "cooking", "hiking", "gym",      "workout", "running",  "art",    "pottery", "travel",
    "dog",     "cat",    "pet",      "team",    "football", "soccer", NULL};

static const gap_topic_t k_topics[] = {
    {HU_CURIOSITY_PLANS, k_plans,
     "- nothing recent on what they have coming up; if there's a natural opening, ask"
     " (one short question, not an interview)\n"},
    {HU_CURIOSITY_WORK, k_work,
     "- nothing recent on their work or school; if there's a natural opening, ask how it's"
     " going (one short question, not an interview)\n"},
    {HU_CURIOSITY_PEOPLE, k_people,
     "- nothing recent on the people in their life; if there's a natural opening, ask after"
     " them (one short question, not an interview)\n"},
    {HU_CURIOSITY_INTERESTS, k_interests,
     "- nothing recent on what they've been into; if there's a natural opening, ask"
     " (one short question, not an interview)\n"},
};

#define TOPIC_COUNT (sizeof(k_topics) / sizeof(k_topics[0]))

static bool mentions_any(const char *text, size_t len, const char *const *words) {
    for (size_t i = 0; words[i]; i++)
        if (hu_str_contains_word_ci_n(text, len, words[i]))
            return true;
    return false;
}

hu_curiosity_topic_t hu_curiosity_gap_pick(const char *recent, size_t len,
                                           hu_curiosity_topic_t after) {
    size_t start = 0; /* index after `after`; NONE (or unknown) starts at the first */
    for (size_t i = 0; i < TOPIC_COUNT; i++)
        if (k_topics[i].topic == after)
            start = i + 1;
    for (size_t k = 0; k < TOPIC_COUNT; k++) {
        const gap_topic_t *t = &k_topics[(start + k) % TOPIC_COUNT];
        if (!recent || len == 0 || !mentions_any(recent, len, t->words))
            return t->topic;
    }
    return HU_CURIOSITY_NONE;
}

const char *hu_curiosity_gap_line(hu_curiosity_topic_t t) {
    for (size_t i = 0; i < TOPIC_COUNT; i++)
        if (k_topics[i].topic == t)
            return k_topics[i].line;
    return NULL;
}

/* Per-process cooldown and last topic, keyed by contact. A restart forgets
 * them, which at worst offers one gap a little early. */
#define COOLDOWN_SLOTS 64
static struct {
    char contact[64];
    int64_t at;
    hu_curiosity_topic_t last;
} s_offered[COOLDOWN_SLOTS];
static pthread_mutex_t s_offered_mu = PTHREAD_MUTEX_INITIALIZER;

hu_curiosity_topic_t hu_curiosity_gap_offer(const char *contact, size_t contact_len,
                                            const char *inbound, size_t inbound_len,
                                            const char *recent, size_t recent_len, int64_t now_s,
                                            bool commit) {
    if (!contact || contact_len == 0 || contact_len >= sizeof(s_offered[0].contact))
        return HU_CURIOSITY_NONE;
    if (inbound && memchr(inbound, '?', inbound_len))
        return HU_CURIOSITY_NONE; /* they asked us something: answer it first */
    const int64_t cooldown = (int64_t)HU_CURIOSITY_COOLDOWN_HOURS * 3600;
    pthread_mutex_lock(&s_offered_mu);
    size_t slot = COOLDOWN_SLOTS, oldest = 0;
    for (size_t i = 0; i < COOLDOWN_SLOTS; i++) {
        if (strlen(s_offered[i].contact) == contact_len &&
            memcmp(s_offered[i].contact, contact, contact_len) == 0) {
            slot = i;
            break;
        }
        if (s_offered[i].at < s_offered[oldest].at)
            oldest = i;
    }
    hu_curiosity_topic_t t = HU_CURIOSITY_NONE;
    if (slot == COOLDOWN_SLOTS || now_s - s_offered[slot].at >= cooldown)
        t = hu_curiosity_gap_pick(
            recent, recent_len, slot == COOLDOWN_SLOTS ? HU_CURIOSITY_NONE : s_offered[slot].last);
    if (t != HU_CURIOSITY_NONE && commit) {
        if (slot == COOLDOWN_SLOTS) {
            slot = oldest;
            memcpy(s_offered[slot].contact, contact, contact_len);
            s_offered[slot].contact[contact_len] = '\0';
        }
        s_offered[slot].at = now_s;
        s_offered[slot].last = t;
    }
    pthread_mutex_unlock(&s_offered_mu);
    return t;
}

hu_gate_mode_t hu_curiosity_gaps_mode(void) {
    return hu_gate_mode_from_env("HU_CURIOSITY_GAPS", HU_GATE_OFF);
}

#ifdef HU_IS_TEST
void hu_curiosity_gaps_reset_for_test(void) {
    pthread_mutex_lock(&s_offered_mu);
    memset(s_offered, 0, sizeof(s_offered));
    pthread_mutex_unlock(&s_offered_mu);
}
#endif
