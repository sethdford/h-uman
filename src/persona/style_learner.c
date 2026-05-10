/* Style learning loop: re-analyze conversations to update persona */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/persona.h"
#include "human/provider.h"
#include <stdio.h>
#include <string.h>

#define HU_STYLE_REANALYZE_PROMPT_CAP 16384
#define HU_STYLE_REANALYZE_MSG_LIMIT  20
#define HU_STYLE_REANALYZE_MIN_MSGS   2

#define HU_STYLE_ANALYST_SYS                                                          \
    "You are a communication style analyst. Analyze the following "                   \
    "messages and extract personality traits, vocabulary preferences, communication " \
    "patterns, and style rules in JSON format."

hu_error_t hu_persona_style_reanalyze(hu_allocator_t *alloc, hu_provider_t *provider,
                                      const char *model, size_t model_len, hu_legacy_memory_t *memory,
                                      const char *persona_name, size_t persona_name_len,
                                      const char *channel, size_t channel_len,
                                      const char *contact_id, size_t contact_id_len) {
    if (!alloc || !persona_name || persona_name_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Apply pending feedback regardless of memory/recall */
    hu_error_t fb_err = hu_persona_feedback_apply(alloc, persona_name, persona_name_len);
    if (fb_err != HU_OK)
        return fb_err;

    if (!memory || !memory->vtable || !memory->vtable->recall)
        return HU_OK;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)provider;
    (void)model;
    (void)model_len;
    (void)memory;
    (void)channel;
    (void)channel_len;
    (void)contact_id;
    (void)contact_id_len;
    return HU_OK;
#else
    /* Recall recent conversation entries from memory */
    static const char query[] = "conversation";
    const char *sess = contact_id && contact_id_len > 0 ? contact_id : "";
    size_t sess_len = contact_id && contact_id_len > 0 ? contact_id_len : 0;

    hu_memory_entry_t *entries = NULL;
    size_t entry_count = 0;
    hu_error_t err = memory->vtable->recall(memory->ctx, alloc, query, sizeof(query) - 1,
                                            HU_STYLE_REANALYZE_MSG_LIMIT, sess, sess_len, &entries,
                                            &entry_count);
    if (err != HU_OK || !entries || entry_count < HU_STYLE_REANALYZE_MIN_MSGS) {
        if (entries) {
            for (size_t i = 0; i < entry_count; i++)
                hu_memory_entry_free_fields(alloc, &entries[i]);
            alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_memory_entry_t));
        }
        return HU_OK;
    }

    /* Extract message texts (null-terminated copies for analyzer) */
    char **messages = (char **)alloc->alloc(alloc->ctx, entry_count * sizeof(char *));
    if (!messages) {
        for (size_t i = 0; i < entry_count; i++)
            hu_memory_entry_free_fields(alloc, &entries[i]);
        alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_memory_entry_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(messages, 0, entry_count * sizeof(char *));
    size_t msg_count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (!entries[i].content || entries[i].content_len == 0)
            continue;
        char *dup = hu_strndup(alloc, entries[i].content, entries[i].content_len);
        if (!dup)
            break;
        messages[msg_count++] = dup;
    }
    for (size_t i = 0; i < entry_count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, entry_count * sizeof(hu_memory_entry_t));

    if (msg_count < HU_STYLE_REANALYZE_MIN_MSGS) {
        for (size_t i = 0; i < msg_count; i++)
            alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
        alloc->free(alloc->ctx, messages, entry_count * sizeof(char *));
        return HU_OK;
    }

    /* Build extraction prompt (for future provider call; not invoked here) */
    char prompt_buf[HU_STYLE_REANALYZE_PROMPT_CAP];
    size_t prompt_len = 0;
    char ch_buf[64];
    const char *ch = "cli";
    if (channel && channel_len > 0 && channel_len < sizeof(ch_buf)) {
        memcpy(ch_buf, channel, channel_len);
        ch_buf[channel_len] = '\0';
        ch = ch_buf;
    }
    err = hu_persona_analyzer_build_prompt((const char **)messages, msg_count, ch, prompt_buf,
                                           sizeof(prompt_buf), &prompt_len);
    for (size_t i = 0; i < msg_count; i++)
        alloc->free(alloc->ctx, messages[i], strlen(messages[i]) + 1);
    alloc->free(alloc->ctx, messages, entry_count * sizeof(char *));

    if (err != HU_OK)
        return err;

    if (!provider || !provider->vtable || !provider->vtable->chat_with_system)
        return HU_OK;

    /* Call LLM to analyze conversation style */
    char *response = NULL;
    size_t response_len = 0;
    hu_error_t llm_err = provider->vtable->chat_with_system(
        provider->ctx, alloc, HU_STYLE_ANALYST_SYS, (sizeof(HU_STYLE_ANALYST_SYS) - 1), prompt_buf,
        prompt_len, model && model_len > 0 ? model : "gpt-4o-mini",
        model && model_len > 0 ? model_len : (size_t)11, 0.0, &response, &response_len);
    if (llm_err != HU_OK || !response)
        return HU_OK;

    /* Parse response into partial persona */
    hu_persona_t partial;
    memset(&partial, 0, sizeof(partial));
    size_t ch_len = channel && channel_len > 0 ? channel_len : 3;
    const char *ch_ptr = channel && channel_len > 0 ? channel : "cli";
    hu_error_t parse_err =
        hu_persona_analyzer_parse_response(alloc, response, response_len, ch_ptr, ch_len, &partial);
    alloc->free(alloc->ctx, response, response_len + 1);

    if (parse_err == HU_OK) {
        hu_persona_t current;
        memset(&current, 0, sizeof(current));
        if (hu_persona_load(alloc, persona_name, persona_name_len, &current) == HU_OK) {
            hu_persona_t merged;
            memset(&merged, 0, sizeof(merged));
            if (hu_persona_creator_synthesize(alloc, (const hu_persona_t[]){current, partial}, 2,
                                              persona_name, persona_name_len, &merged) == HU_OK) {
                hu_error_t write_err = hu_persona_creator_write(alloc, &merged);
                if (write_err != HU_OK)
                    hu_log_error("style_learner", NULL, "persona write failed: %s",
                                 hu_error_string(write_err));
                hu_persona_deinit(alloc, &merged);
            }
            hu_persona_deinit(alloc, &current);
        }
    }
    hu_persona_deinit(alloc, &partial);
    return HU_OK;
#endif
}
