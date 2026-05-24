/* src/memory/identity_continuity.c
 *
 * Pure scan over personal_model handles + identity_graph contacts.
 * Surfaces "this new handle might be Alice" candidates without
 * mutating either side. Sprint B Story 8. */

#include "human/memory/identity_continuity.h"

#include "human/memory/fact_extract.h"
#include "human/memory/identity_resolver.h"
#include "human/memory/personal_model.h"
#include "human/memory/trust.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

size_t hu_identity_continuity_first_token_lower(const char *s, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (!s)
        return 0;
    /* Skip leading non-alpha. */
    while (*s && !isalpha((unsigned char)*s))
        s++;
    size_t i = 0;
    while (*s && isalpha((unsigned char)*s) && i + 1 < cap) {
        out[i++] = (char)tolower((unsigned char)*s);
        s++;
    }
    out[i] = '\0';
    return i;
}

/* Is `handle` already represented in the graph? Walks all contacts'
 * alias arrays. Case-insensitive exact match. */
static bool handle_in_graph(const hu_identity_graph_t *g, const char *handle) {
    if (!g || !handle || !*handle)
        return false;
    for (size_t c = 0; c < g->contact_count; c++) {
        for (size_t a = 0; a < g->contacts[c].alias_count; a++) {
            if (strncasecmp(g->contacts[c].aliases[a], handle, HU_IDENTITY_HANDLE_CAP) == 0)
                return true;
        }
    }
    return false;
}

/* Find a contact whose canonical_name's first-token matches the
 * first-token of `handle`. Returns NULL when no match (or graph empty). */
static const hu_identity_contact_t *find_name_match(const hu_identity_graph_t *g,
                                                    const char *handle) {
    if (!g || !handle || !*handle)
        return NULL;
    char handle_tok[64];
    if (hu_identity_continuity_first_token_lower(handle, handle_tok, sizeof(handle_tok)) < 2)
        return NULL; /* too short to be a useful match */
    for (size_t c = 0; c < g->contact_count; c++) {
        const hu_identity_contact_t *contact = &g->contacts[c];
        char name_tok[64];
        if (hu_identity_continuity_first_token_lower(contact->canonical_name, name_tok,
                                                     sizeof(name_tok)) < 2)
            continue;
        if (strcmp(handle_tok, name_tok) == 0)
            return contact;
    }
    return NULL;
}

size_t hu_identity_continuity_suggest(const struct hu_personal_model *model_opaque,
                                      const hu_identity_graph_t *graph_opaque, char *out,
                                      size_t cap) {
    if (!out || cap < 16)
        return 0;
    out[0] = '\0';
    if (!model_opaque || !graph_opaque)
        return 0;

    const hu_personal_model_t *model = (const hu_personal_model_t *)model_opaque;
    const hu_identity_graph_t *graph = graph_opaque;
    if (graph->contact_count == 0 || model->fact_count == 0)
        return 0;

    /* Walk facts; for each unique provenance.contact_handle that is
     * NOT already in the graph, look for a first-token name match. */
    char seen[16][HU_IDENTITY_HANDLE_CAP] = {{0}};
    size_t seen_count = 0;

    for (size_t i = 0; i < model->fact_count && seen_count < 16; i++) {
        const char *h = model->facts[i].provenance.contact_handle;
        if (!h || !h[0])
            continue;
        bool dup = false;
        for (size_t s = 0; s < seen_count; s++) {
            if (strncasecmp(seen[s], h, HU_IDENTITY_HANDLE_CAP) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        snprintf(seen[seen_count++], HU_IDENTITY_HANDLE_CAP, "%s", h);

        if (handle_in_graph(graph, h))
            continue; /* already merged — nothing to suggest */

        const hu_identity_contact_t *match = find_name_match(graph, h);
        if (!match)
            continue;

        /* Surface the first plausible candidate and stop. The user can
         * commit the merge with hu_identity_save; we don't loop because
         * flooding the prompt with merge candidates is noise. */
        int n = snprintf(out, cap,
                         "IDENTITY: \"%s\" may be same person as %s (shared first-name token).", h,
                         match->canonical_name);
        if (n < 0)
            return 0;
        return (size_t)((size_t)n < cap ? (size_t)n : cap - 1);
    }
    return 0;
}
