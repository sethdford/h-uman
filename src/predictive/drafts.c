/* src/predictive/drafts.c — Predictive draft suggestions (Sprint 1 Story 1).
 *
 * See include/human/predictive_drafts.h for the contract. */

#include "human/predictive_drafts.h"

#include "human/calibration.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/memory/personal_model.h"
#include "human/provider.h"
#include "human/providers/factory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── provider override (CLI --provider <name>) ───────────────────────── */
/* See header. File-scope static, NOT thread-local — the daemon runs as a
 * single-threaded event loop and CLI invocations are short-lived
 * single-threaded processes, so a plain static is sufficient. */
#define HU_DRAFTS_PROVIDER_OVERRIDE_CAP 64
static char s_drafts_provider_override[HU_DRAFTS_PROVIDER_OVERRIDE_CAP] = {0};

void hu_predictive_drafts_set_provider_override(const char *provider_name) {
    if (!provider_name || !*provider_name) {
        s_drafts_provider_override[0] = '\0';
        return;
    }
    /* snprintf truncates safely; the cap is generous for real provider
     * names ("gemini", "openai", "mlx_local", "ollama", "anthropic", …). */
    snprintf(s_drafts_provider_override, sizeof(s_drafts_provider_override), "%s", provider_name);
}

/* ── safe-string helpers ─────────────────────────────────────────────── */

/* Append `s` (up to s_len, or strlen(s) when s_len == SIZE_MAX) to the
 * snprintf-style write head. NUL-terminates `dst[*off]` after each
 * append. NULL-safe on `s`. */
static void sb_append(char *dst, size_t cap, size_t *off, const char *s, size_t s_len) {
    if (!dst || cap == 0 || *off + 1 >= cap || !s)
        return;
    size_t avail = cap - 1 - *off;
    size_t want = s_len;
    if (want == SIZE_MAX)
        want = strlen(s);
    if (want > avail)
        want = avail;
    memcpy(dst + *off, s, want);
    *off += want;
    dst[*off] = '\0';
}

static void sb_appendz(char *dst, size_t cap, size_t *off, const char *s) {
    sb_append(dst, cap, off, s, SIZE_MAX);
}

/* Append `s` as a JSON-escaped string fragment (no surrounding quotes).
 * Escapes the minimal set: \ " \n \r \t and other ASCII control chars.
 * Truncates safely on buffer exhaustion. */
static void sb_append_json_escaped(char *dst, size_t cap, size_t *off, const char *s,
                                   size_t s_len) {
    if (!dst || cap == 0 || !s)
        return;
    if (s_len == SIZE_MAX)
        s_len = strlen(s);
    for (size_t i = 0; i < s_len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (*off + 1 >= cap)
            return;
        const char *esc = NULL;
        char small[8];
        switch (c) {
        case '"':
            esc = "\\\"";
            break;
        case '\\':
            esc = "\\\\";
            break;
        case '\n':
            esc = "\\n";
            break;
        case '\r':
            esc = "\\r";
            break;
        case '\t':
            esc = "\\t";
            break;
        case '\b':
            esc = "\\b";
            break;
        case '\f':
            esc = "\\f";
            break;
        default:
            if (c < 0x20) {
                snprintf(small, sizeof(small), "\\u%04x", c);
                esc = small;
            }
            break;
        }
        if (esc) {
            sb_appendz(dst, cap, off, esc);
        } else {
            if (*off + 1 < cap) {
                dst[*off] = (char)c;
                (*off)++;
                dst[*off] = '\0';
            }
        }
    }
}

/* ── prompt builder ──────────────────────────────────────────────────── */

size_t hu_predictive_drafts_build_prompt(const char *contact_handle, const char *channel,
                                         const char *persona_summary,
                                         const char *recent_messages_summary,
                                         const char *reaction_signature_summary, size_t n,
                                         char *out, size_t out_cap) {
    if (!out || out_cap < 2)
        return 0;
    out[0] = '\0';
    if (!contact_handle || !*contact_handle)
        contact_handle = "(unknown)";

    /* Clamp N to documented range. */
    if (n < 1)
        n = HU_PREDICTIVE_DRAFT_DEFAULT_N;
    if (n > HU_PREDICTIVE_DRAFT_MAX_N)
        n = HU_PREDICTIVE_DRAFT_MAX_N;

    size_t off = 0;
    char tmp[64];

    sb_appendz(out, out_cap, &off,
               "You are drafting candidate messages on behalf of the user. "
               "Suggest the ");
    snprintf(tmp, sizeof(tmp), "%zu", n);
    sb_appendz(out, out_cap, &off, tmp);
    sb_appendz(out, out_cap, &off,
               " most likely next messages the user would send. "
               "Each draft must be at most 60 tokens (~240 characters), "
               "natural in the user's voice, and immediately sendable.\n\n");

    sb_appendz(out, out_cap, &off, "Recipient handle: ");
    sb_appendz(out, out_cap, &off, contact_handle);
    sb_appendz(out, out_cap, &off, "\n");

    if (channel && *channel) {
        sb_appendz(out, out_cap, &off, "Channel: ");
        sb_appendz(out, out_cap, &off, channel);
        sb_appendz(out, out_cap, &off, "\n");
    }
    sb_appendz(out, out_cap, &off, "\n");

    sb_appendz(out, out_cap, &off, "## Persona\n");
    if (persona_summary && *persona_summary) {
        sb_appendz(out, out_cap, &off, persona_summary);
        sb_appendz(out, out_cap, &off, "\n");
    } else {
        sb_appendz(out, out_cap, &off, "(no persona signal yet)\n");
    }

    sb_appendz(out, out_cap, &off, "\n## Recent messages with ");
    sb_appendz(out, out_cap, &off, contact_handle);
    sb_appendz(out, out_cap, &off, "\n");
    if (recent_messages_summary && *recent_messages_summary) {
        sb_appendz(out, out_cap, &off, recent_messages_summary);
        sb_appendz(out, out_cap, &off, "\n");
    } else {
        sb_appendz(out, out_cap, &off, "(no recent history available)\n");
    }

    if (reaction_signature_summary && *reaction_signature_summary) {
        sb_appendz(out, out_cap, &off, "\n## Reaction signature\n");
        sb_appendz(out, out_cap, &off, reaction_signature_summary);
        sb_appendz(out, out_cap, &off, "\n");
    }

    sb_appendz(out, out_cap, &off,
               "\n## Output format\n"
               "Respond with STRICT JSON of the form:\n"
               "{\"drafts\":[{\"text\":\"...\",\"confidence\":0.7,\"rationale\":\"...\"}]}\n"
               "The array MUST contain exactly ");
    snprintf(tmp, sizeof(tmp), "%zu", n);
    sb_appendz(out, out_cap, &off, tmp);
    sb_appendz(out, out_cap, &off,
               " entries. Confidence is a number in [0,1]. "
               "Rationale is a short phrase (≤ 80 chars) like "
               "\"matches recent hiking topic\". "
               "Output ONLY the JSON object. No prose, no markdown.");
    return off;
}

/* ── reaction signature rendering ────────────────────────────────────── */

static bool ieq(const char *a, const char *b) {
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

size_t hu_predictive_drafts_render_signature(const struct hu_personal_model *model,
                                             const char *contact_handle, char *buf, size_t cap) {
    if (!buf || cap < 2)
        return 0;
    buf[0] = '\0';
    if (!model || !contact_handle || !*contact_handle)
        return 0;

    hu_calib_reaction_signature_t sig;
    memset(&sig, 0, sizeof(sig));
    size_t reactors = hu_calib_reaction_signature_from_model(model, &sig);
    if (reactors == 0)
        return 0;

    /* Find the reactor matching contact_handle (case-insensitive). */
    const hu_calib_top_reactor_t *match = NULL;
    for (size_t i = 0; i < sig.reactor_count; i++) {
        if (ieq(sig.top_reactors[i].handle, contact_handle)) {
            match = &sig.top_reactors[i];
            break;
        }
    }
    if (!match)
        return 0;

    size_t off = 0;
    char num[32];
    sb_appendz(buf, cap, &off, contact_handle);
    if (match->positive_count > match->negative_count) {
        sb_appendz(buf, cap, &off, " has reacted positively most often");
    } else if (match->negative_count > match->positive_count) {
        sb_appendz(buf, cap, &off, " has reacted negatively most often");
    } else {
        sb_appendz(buf, cap, &off, " has mixed reactions");
    }
    if (sig.salient_topic_count > 0) {
        sb_appendz(buf, cap, &off, " on topics: ");
        size_t shown = 0;
        for (size_t i = 0; i < sig.salient_topic_count && shown < 5; i++) {
            if (!sig.salient_topics[i][0])
                continue;
            if (shown > 0)
                sb_appendz(buf, cap, &off, ", ");
            sb_appendz(buf, cap, &off, sig.salient_topics[i]);
            shown++;
        }
    }
    sb_appendz(buf, cap, &off, " (");
    snprintf(num, sizeof(num), "%u", match->positive_count);
    sb_appendz(buf, cap, &off, num);
    sb_appendz(buf, cap, &off, " positive, ");
    snprintf(num, sizeof(num), "%u", match->negative_count);
    sb_appendz(buf, cap, &off, num);
    sb_appendz(buf, cap, &off, " negative).");
    return off;
}

/* ── response parsing ────────────────────────────────────────────────── */

static size_t copy_truncated(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    if (!dst || dst_cap == 0)
        return 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    size_t want = src_len;
    if (want > dst_cap - 1)
        want = dst_cap - 1;
    memcpy(dst, src, want);
    dst[want] = '\0';
    return want;
}

/* Find the first '{' and the matching '}' in `s` and return the
 * enclosing slice. Returns false when no balanced object is found
 * (the response was a numbered-list, free prose, etc.). */
static bool find_json_object(const char *s, size_t len, size_t *out_start, size_t *out_end) {
    if (!s || len == 0)
        return false;
    size_t start = 0;
    bool found = false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '{') {
            start = i;
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (size_t i = start; i < len; i++) {
        char c = s[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                *out_start = start;
                *out_end = i + 1;
                return true;
            }
        }
    }
    return false;
}

static hu_error_t parse_numbered_fallback(const char *s, size_t len,
                                          hu_predictive_draft_set_t *out) {
    size_t i = 0;
    while (i < len && out->draft_count < HU_PREDICTIVE_DRAFT_MAX_N) {
        /* skip leading whitespace */
        while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
            i++;
        if (i >= len)
            break;
        /* require a digit followed by . or ) */
        if (!isdigit((unsigned char)s[i])) {
            i++;
            continue;
        }
        size_t d = i;
        while (d < len && isdigit((unsigned char)s[d]))
            d++;
        if (d >= len || (s[d] != '.' && s[d] != ')'))
            continue;
        d++; /* skip . or ) */
        while (d < len && (s[d] == ' ' || s[d] == '\t'))
            d++;
        size_t start = d;
        while (d < len && s[d] != '\n' && s[d] != '\r')
            d++;
        size_t text_len = d - start;
        if (text_len > 0) {
            hu_predictive_draft_t *draft = &out->drafts[out->draft_count++];
            memset(draft, 0, sizeof(*draft));
            copy_truncated(draft->text, sizeof(draft->text), s + start, text_len);
            draft->confidence = 0.5f;
            draft->rationale[0] = '\0';
        }
        i = d;
    }
    return (out->draft_count > 0) ? HU_OK : HU_ERR_PARSE;
}

hu_error_t hu_predictive_drafts_parse_response(const char *response, size_t response_len,
                                               hu_predictive_draft_set_t *out) {
    if (!response || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (response_len == 0)
        return HU_ERR_PARSE;

    size_t js, je;
    if (find_json_object(response, response_len, &js, &je)) {
        hu_allocator_t alloc = hu_system_allocator();
        hu_json_value_t *root = NULL;
        hu_error_t err = hu_json_parse(&alloc, response + js, je - js, &root);
        if (err == HU_OK && root && root->type == HU_JSON_OBJECT) {
            hu_json_value_t *drafts = hu_json_object_get(root, "drafts");
            if (drafts && drafts->type == HU_JSON_ARRAY) {
                size_t n = drafts->data.array.len;
                if (n > HU_PREDICTIVE_DRAFT_MAX_N)
                    n = HU_PREDICTIVE_DRAFT_MAX_N;
                for (size_t i = 0; i < n; i++) {
                    hu_json_value_t *item = drafts->data.array.items[i];
                    if (!item || item->type != HU_JSON_OBJECT)
                        continue;
                    const char *text = hu_json_get_string(item, "text");
                    if (!text || !*text)
                        continue;
                    hu_predictive_draft_t *d = &out->drafts[out->draft_count++];
                    memset(d, 0, sizeof(*d));
                    copy_truncated(d->text, sizeof(d->text), text, strlen(text));
                    double conf = hu_json_get_number(item, "confidence", 0.5);
                    if (conf < 0.0)
                        conf = 0.0;
                    if (conf > 1.0)
                        conf = 1.0;
                    d->confidence = (float)conf;
                    const char *rat = hu_json_get_string(item, "rationale");
                    if (rat)
                        copy_truncated(d->rationale, sizeof(d->rationale), rat, strlen(rat));
                }
            }
            hu_json_free(&alloc, root);
            if (out->draft_count > 0)
                return HU_OK;
        } else if (root) {
            hu_json_free(&alloc, root);
        }
    }

    /* JSON parse failed or no usable drafts — try numbered-list. */
    return parse_numbered_fallback(response, response_len, out);
}

/* ── end-to-end generator ────────────────────────────────────────────── */

hu_error_t hu_predictive_drafts_generate(hu_allocator_t *alloc,
                                         const struct hu_personal_model *model,
                                         const char *contact_handle, const char *channel,
                                         const char *recent_messages_summary, size_t n,
                                         hu_predictive_draft_set_t *out) {
    if (!alloc || !contact_handle || !*contact_handle || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Grounding check: refuse to hallucinate without any signal at all. */
    bool has_persona =
        model != NULL && hu_personal_model_has_content((const hu_personal_model_t *)model);
    bool has_history = recent_messages_summary && *recent_messages_summary;
    if (!has_persona && !has_history)
        return HU_ERR_NOT_FOUND;

    /* Build the persona summary slot if a model was supplied. */
    char persona_buf[2048];
    persona_buf[0] = '\0';
    if (model)
        hu_personal_model_build_prompt((const hu_personal_model_t *)model, persona_buf,
                                       sizeof(persona_buf));

    /* Reaction signature for THIS contact, if any. */
    char sig_buf[512];
    hu_predictive_drafts_render_signature(model, contact_handle, sig_buf, sizeof(sig_buf));

    /* Build the prompt. */
    char prompt[HU_PREDICTIVE_DRAFT_PROMPT_MAX];
    size_t prompt_len = hu_predictive_drafts_build_prompt(contact_handle, channel, persona_buf,
                                                          recent_messages_summary, sig_buf, n,
                                                          prompt, sizeof(prompt));
    if (prompt_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* Create the user's default provider. */
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_error_t cerr = hu_config_load(alloc, &cfg);
    if (cerr != HU_OK)
        return HU_ERR_NOT_SUPPORTED;

    /* Apply CLI provider override if set. The override BORROWS the cfg's
     * default_provider slot for this one invocation — we don't allocate,
     * just point at our static buffer. hu_config_deinit will see a
     * non-allocated default_provider via the swap-and-restore guard, so
     * we save the original pointer and restore before deinit. */
    char *saved_default_provider = NULL;
    bool override_applied = false;
    if (s_drafts_provider_override[0]) {
        saved_default_provider = cfg.default_provider;
        cfg.default_provider = s_drafts_provider_override;
        override_applied = true;
    }

    hu_provider_t provider;
    memset(&provider, 0, sizeof(provider));
    hu_error_t perr = hu_provider_create_default(alloc, &cfg, &provider);
    if (perr != HU_OK || !provider.vtable) {
        if (override_applied)
            cfg.default_provider = saved_default_provider;
        hu_config_deinit(&cfg);
        return HU_ERR_NOT_SUPPORTED;
    }
    if (!provider.vtable->chat_with_system) {
        if (provider.vtable->deinit)
            provider.vtable->deinit(provider.ctx, alloc);
        if (override_applied)
            cfg.default_provider = saved_default_provider;
        hu_config_deinit(&cfg);
        return HU_ERR_NOT_SUPPORTED;
    }

    static const char *kSystem =
        "You are a draft-suggestion helper. Respond ONLY with JSON as instructed.";
    char *resp = NULL;
    size_t resp_len = 0;
    const char *model_id = NULL;
    size_t model_id_len = 0;
    if (cfg.default_model) {
        model_id = cfg.default_model;
        model_id_len = strlen(cfg.default_model);
    }
    hu_error_t cerr2 = provider.vtable->chat_with_system(
        provider.ctx, alloc, kSystem, strlen(kSystem), prompt, prompt_len, model_id, model_id_len,
        0.6, &resp, &resp_len);

    hu_error_t parse_err = HU_OK;
    if (cerr2 == HU_OK && resp && resp_len > 0) {
        parse_err = hu_predictive_drafts_parse_response(resp, resp_len, out);
    } else {
        parse_err = (cerr2 == HU_OK) ? HU_ERR_PARSE : cerr2;
    }

    if (resp)
        alloc->free(alloc->ctx, resp, resp_len + 1);
    if (provider.vtable->deinit)
        provider.vtable->deinit(provider.ctx, alloc);
    /* Restore the cfg's owned pointer before deinit so the override slot
     * (file-scope static, NOT heap) doesn't get passed to free(). */
    if (override_applied)
        cfg.default_provider = saved_default_provider;
    hu_config_deinit(&cfg);
    return parse_err;
}

/* ── CLI subcommand ──────────────────────────────────────────────────── */

static const char *kDraftsUsage =
    "Usage: human drafts --contact <handle> [--channel <name>] [--n N] [--provider <name>]\n"
    "  --contact <handle>   Recipient handle (required)\n"
    "  --channel <name>     Channel name (optional; default \"imessage\")\n"
    "  --n N                Number of drafts (1..8; default 3)\n"
    "  --provider <name>    Override configured default_provider for this run\n"
    "                       (e.g. \"gemini\" when local MLX is down)\n";

hu_error_t cmd_drafts(hu_allocator_t *alloc, int argc, char **argv) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    const char *contact = NULL;
    const char *channel = NULL;
    const char *provider_override = NULL;
    size_t n = HU_PREDICTIVE_DRAFT_DEFAULT_N;
    bool want_help = false;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!a)
            continue;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            want_help = true;
        } else if (strcmp(a, "--contact") == 0 && i + 1 < argc) {
            contact = argv[++i];
        } else if (strcmp(a, "--channel") == 0 && i + 1 < argc) {
            channel = argv[++i];
        } else if (strcmp(a, "--provider") == 0 && i + 1 < argc) {
            provider_override = argv[++i];
        } else if (strcmp(a, "--n") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 1)
                v = 1;
            if (v > (long)HU_PREDICTIVE_DRAFT_MAX_N)
                v = HU_PREDICTIVE_DRAFT_MAX_N;
            n = (size_t)v;
        }
    }

    if (want_help || !contact) {
        printf("%s", kDraftsUsage);
        return want_help ? HU_OK : HU_ERR_INVALID_ARGUMENT;
    }
    if (!channel)
        channel = "imessage";

    /* Load the personal model from the resolved default path. */
    hu_personal_model_t model;
    hu_personal_model_init(&model);
    char path_buf[1024];
    const char *path = hu_personal_model_resolve_default_path(path_buf, sizeof(path_buf));
    if (path) {
        hu_error_t lerr = hu_personal_model_load(&model, path);
        if (lerr != HU_OK && lerr != HU_ERR_NOT_FOUND)
            hu_log_warn("drafts", NULL, "personal model load: %s", hu_error_string(lerr));
    }

    /* Apply CLI provider override (no-op when --provider was not passed). */
    hu_predictive_drafts_set_provider_override(provider_override);

    hu_predictive_draft_set_t set;
    hu_error_t err = hu_predictive_drafts_generate(alloc, &model, contact, channel, NULL, n, &set);

    /* Clear the override so subsequent generate calls (e.g. in long-running
     * daemon paths sharing this TU) revert to the configured default. */
    hu_predictive_drafts_set_provider_override(NULL);
    if (err == HU_ERR_NOT_SUPPORTED) {
        hu_log_error("drafts", NULL, "no LLM provider configured — see ~/.human/config.json");
        return err;
    }
    if (err == HU_ERR_NOT_FOUND) {
        hu_log_error("drafts", NULL,
                     "no grounding signal: personal_model is empty and no history was supplied");
        return err;
    }
    if (err != HU_OK) {
        hu_log_error("drafts", NULL, "generation failed: %s", hu_error_string(err));
        return err;
    }

    printf("%zu draft suggestion%s for %s on %s:\n\n", set.draft_count,
           set.draft_count == 1 ? "" : "s", contact, channel);
    for (size_t i = 0; i < set.draft_count; i++) {
        const hu_predictive_draft_t *d = &set.drafts[i];
        printf("  %zu. %s\n", i + 1, d->text);
        if (d->rationale[0])
            printf("     (%.0f%% confidence; %s)\n", d->confidence * 100.0f, d->rationale);
        else
            printf("     (%.0f%% confidence)\n", d->confidence * 100.0f);
    }
    /* Suppress unused-warning for sb_append_json_escaped — kept for future
     * use when the prompt builder embeds inputs into a JSON wire format. */
    (void)sb_append_json_escaped;
    return HU_OK;
}
