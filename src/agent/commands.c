#include "seaclaw/agent/commands.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <ctype.h>

static sc_slash_cmd_t g_parsed;
static char g_name_buf[64];
static char g_arg_buf[512];

static int ci_equal(const char *a, size_t na, const char *b, size_t nb) {
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

const sc_slash_cmd_t *sc_agent_commands_parse(const char *msg, size_t len) {
    if (!msg || len < 2 || msg[0] != '/') return NULL;
    while (len > 0 && (msg[len - 1] == ' ' || msg[len - 1] == '\r' || msg[len - 1] == '\n'))
        len--;
    const char *body = msg + 1;
    size_t body_len = len - 1;
    if (body_len == 0) return NULL;

    size_t split = 0;
    while (split < body_len && body[split] != ':' && body[split] != ' ' && body[split] != '\t')
        split++;
    if (split == 0) return NULL;

    const char *raw_name = body;
    size_t name_len = split;
    const char *mention = memchr(raw_name, '@', name_len);
    if (mention) name_len = (size_t)(mention - raw_name);

    size_t rest_off = split;
    if (rest_off < body_len && body[rest_off] == ':') rest_off++;
    const char *rest = body + rest_off;
    size_t rest_len = body_len - rest_off;
    while (rest_len > 0 && (rest[0] == ' ' || rest[0] == '\t')) { rest++; rest_len--; }

    size_t cn = name_len < sizeof(g_name_buf) - 1 ? name_len : sizeof(g_name_buf) - 1;
    memcpy(g_name_buf, raw_name, cn);
    g_name_buf[cn] = '\0';
    size_t ca = rest_len < sizeof(g_arg_buf) - 1 ? rest_len : sizeof(g_arg_buf) - 1;
    memcpy(g_arg_buf, rest, ca);
    g_arg_buf[ca] = '\0';

    g_parsed.name = g_name_buf;
    g_parsed.name_len = strlen(g_name_buf);
    g_parsed.arg = g_arg_buf;
    g_parsed.arg_len = strlen(g_arg_buf);
    return &g_parsed;
}

static const char BARE_PROMPT[] =
    "A new session was started via /new or /reset. Execute your Session Startup sequence now - read the required files before responding to the user. Then greet the user in your configured persona, if one is provided. Be yourself - use your defined voice, mannerisms, and mood. Keep it to 1-3 sentences and ask what they want to do. If the runtime model differs from default_model in the system prompt, mention the default model. Do not mention internal steps, files, tools, or reasoning.";

sc_error_t sc_agent_commands_bare_session_reset_prompt(sc_allocator_t *alloc,
    const char *msg, size_t len, char **out_prompt) {
    if (!alloc || !out_prompt) return SC_ERR_INVALID_ARGUMENT;
    const sc_slash_cmd_t *cmd = sc_agent_commands_parse(msg, len);
    if (!cmd) { *out_prompt = NULL; return SC_OK; }
    if (!ci_equal(cmd->name, cmd->name_len, "new", 3) &&
        !ci_equal(cmd->name, cmd->name_len, "reset", 5)) {
        *out_prompt = NULL;
        return SC_OK;
    }
    if (cmd->arg_len != 0) { *out_prompt = NULL; return SC_OK; }
    *out_prompt = sc_strdup(alloc, BARE_PROMPT);
    return *out_prompt ? SC_OK : SC_ERR_OUT_OF_MEMORY;
}
