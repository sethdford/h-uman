#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/lifecycle.h"
#include "human/provider.h"
#include <string.h>
#include <time.h>

#ifdef HU_IS_TEST
#define HU_SUMMARIZER_USE_LLM 0
#else
#define HU_SUMMARIZER_USE_LLM 1
#endif

static const char HU_SUMMARIZER_SYSTEM[] =
    "Summarize the following memories into one concise paragraph. Preserve key facts.";
static const char HU_SUMMARIZER_MODEL[] = "gpt-4o-mini";

hu_error_t hu_memory_summarize(hu_allocator_t *alloc, hu_memory_t *memory,
                               const hu_summarizer_config_t *config, hu_summarizer_stats_t *stats) {
    if (!alloc || !memory || !memory->vtable || !config || !stats)
        return HU_ERR_INVALID_ARGUMENT;

    memset(stats, 0, sizeof(*stats));

    hu_memory_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = memory->vtable->list(memory->ctx, alloc, NULL, NULL, 0, &entries, &count);
    if (err != HU_OK)
        return err;
    if (!entries || count == 0)
        return HU_OK;

    size_t max_len = config->max_summary_len > 0 ? config->max_summary_len : 512;
    bool use_llm = HU_SUMMARIZER_USE_LLM && config->provider && config->provider->vtable;

    if (!use_llm) {
        for (size_t i = 0; i < count; i++) {
            hu_memory_entry_t *e = &entries[i];
            if (!e->content || e->content_len <= max_len)
                continue;

            char *truncated = (char *)alloc->alloc(alloc->ctx, max_len + 1);
            if (!truncated) {
                for (size_t j = 0; j < count; j++)
                    hu_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            memcpy(truncated, e->content, max_len);
            truncated[max_len] = '\0';

            {
                bool discarded;
                memory->vtable->forget(memory->ctx, e->key, e->key_len, &discarded);
            }
            err = memory->vtable->store(memory->ctx, e->key, e->key_len, truncated, max_len,
                                        &e->category, e->session_id, e->session_id_len);
            alloc->free(alloc->ctx, truncated, max_len + 1);
            if (err != HU_OK) {
                for (size_t j = 0; j < count; j++)
                    hu_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
                return err;
            }
            stats->entries_summarized++;
            stats->tokens_saved += (e->content_len - max_len) / 4;
        }
    } else {
        size_t batch_size = config->batch_size > 0 ? config->batch_size : 5;
        for (size_t i = 0; i < count; i++) {
            if (!entries[i].content || entries[i].content_len <= max_len)
                continue;

            size_t n = 0;
            size_t total_len = 0;
            for (size_t j = i; j < count && n < batch_size; j++) {
                if (entries[j].content && entries[j].content_len > max_len) {
                    n++;
                    total_len += entries[j].content_len + 2;
                }
            }
            if (n == 0)
                break;

            char *batch_msg = (char *)alloc->alloc(alloc->ctx, total_len + 1);
            if (!batch_msg) {
                for (size_t j = 0; j < count; j++)
                    hu_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t pos = 0;
            size_t processed = 0;
            for (size_t j = i; j < count && processed < n; j++) {
                if (!entries[j].content || entries[j].content_len <= max_len)
                    continue;
                memcpy(batch_msg + pos, entries[j].content, entries[j].content_len);
                pos += entries[j].content_len;
                batch_msg[pos++] = '\n';
                batch_msg[pos++] = '\n';
                processed++;
            }
            batch_msg[pos] = '\0';

            char *summary = NULL;
            size_t summary_len = 0;
            err = config->provider->vtable->chat_with_system(
                config->provider->ctx, alloc, HU_SUMMARIZER_SYSTEM,
                sizeof(HU_SUMMARIZER_SYSTEM) - 1, batch_msg, pos, HU_SUMMARIZER_MODEL,
                sizeof(HU_SUMMARIZER_MODEL) - 1, 0.3, &summary, &summary_len);
            alloc->free(alloc->ctx, batch_msg, total_len + 1);
            if (err != HU_OK || !summary) {
                for (size_t j = 0; j < count; j++)
                    hu_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
                return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
            }

            if (summary_len > max_len)
                summary_len = max_len;
            char *key_buf =
                hu_sprintf(alloc, "summary_%ld_%zu", (long)time(NULL), stats->entries_summarized);
            if (!key_buf) {
                alloc->free(alloc->ctx, summary, summary_len + 1);
                for (size_t j = 0; j < count; j++)
                    hu_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
                return HU_ERR_OUT_OF_MEMORY;
            }
            size_t key_len = strlen(key_buf);
            hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
            err = memory->vtable->store(memory->ctx, key_buf, key_len, summary, summary_len, &cat,
                                        NULL, 0);
            hu_str_free(alloc, key_buf);
            alloc->free(alloc->ctx, summary, summary_len + 1);
            if (err != HU_OK) {
                for (size_t j = 0; j < count; j++)
                    hu_memory_entry_free_fields(alloc, &entries[j]);
                alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
                return err;
            }

            processed = 0;
            for (size_t j = i; j < count && processed < n; j++) {
                if (!entries[j].content || entries[j].content_len <= max_len)
                    continue;
                {
                    bool discarded;
                    memory->vtable->forget(memory->ctx, entries[j].key, entries[j].key_len,
                                           &discarded);
                }
                stats->entries_summarized++;
                stats->tokens_saved += (entries[j].content_len - max_len) / 4;
                processed++;
                i = j;
            }
        }
    }

    for (size_t i = 0; i < count; i++)
        hu_memory_entry_free_fields(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_memory_entry_t));
    return HU_OK;
}
