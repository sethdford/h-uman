#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/json.h"
#include "seaclaw/config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef SC_GATEWAY_POSIX
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

#define SC_CLAUDE_CLI_NAME "claude"
#define SC_CLAUDE_DEFAULT_MODEL "claude-sonnet-4"

typedef struct sc_claude_cli_ctx {
    char *claude_path;
    size_t claude_path_len;
} sc_claude_cli_ctx_t;

#if defined(SC_GATEWAY_POSIX) && !SC_IS_TEST
static sc_error_t run_claude_cli(sc_allocator_t *alloc,
    const char *prompt, size_t prompt_len,
    const char *model, size_t model_len,
    char **out, size_t *out_len)
{
    const char *cli = SC_CLAUDE_CLI_NAME;

    char *argv_with_model[] = {
        (char *)cli,
        "-p",
        "--output-format", "json",
        "--model", NULL,
        NULL
    };
    char *argv_default[] = {
        (char *)cli,
        "-p",
        "--output-format", "json",
        NULL
    };

    char model_buf[128];
    char **argv;
    if (model && model_len > 0) {
        if (model_len >= sizeof(model_buf)) model_len = sizeof(model_buf) - 1;
        memcpy(model_buf, model, model_len);
        model_buf[model_len] = '\0';
        argv_with_model[5] = model_buf;
        argv = argv_with_model;
    } else {
        argv = argv_default;
    }

    int stdout_fds[2];
    int stdin_fds[2];
    if (pipe(stdout_fds) != 0) return SC_ERR_IO;
    if (pipe(stdin_fds) != 0) {
        close(stdout_fds[0]);
        close(stdout_fds[1]);
        return SC_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_fds[0]); close(stdout_fds[1]);
        close(stdin_fds[0]); close(stdin_fds[1]);
        return SC_ERR_IO;
    }

    if (pid == 0) {
        close(stdout_fds[0]);
        close(stdin_fds[1]);
        dup2(stdin_fds[0], STDIN_FILENO);
        dup2(stdout_fds[1], STDOUT_FILENO);
        dup2(stdout_fds[1], STDERR_FILENO);
        close(stdin_fds[0]);
        close(stdout_fds[1]);
        execvp(cli, argv);
        _exit(127);
    }

    close(stdout_fds[1]);
    close(stdin_fds[0]);

    /* Write prompt to child's stdin, then close to signal EOF */
    size_t written = 0;
    while (written < prompt_len) {
        ssize_t w = write(stdin_fds[1], prompt + written, prompt_len - written);
        if (w <= 0) break;
        written += (size_t)w;
    }
    close(stdin_fds[1]);

    size_t cap = 256 * 1024;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        close(stdout_fds[0]);
        waitpid(pid, NULL, 0);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1) break;
        ssize_t n = read(stdout_fds[0], buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(stdout_fds[0]);

    int status;
    waitpid(pid, &status, 0);

    /* Parse JSON output: {"type":"result","result":"..."} */
    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, buf, len, &parsed);
    alloc->free(alloc->ctx, buf, cap);

    if (err != SC_OK || !parsed) return SC_ERR_PROVIDER_RESPONSE;

    const char *result = sc_json_get_string(parsed, "result");
    if (result && strlen(result) > 0) {
        size_t rlen = strlen(result);
        *out = sc_strndup(alloc, result, rlen);
        *out_len = rlen;
        sc_json_free(alloc, parsed);
        if (!*out) return SC_ERR_OUT_OF_MEMORY;
        return SC_OK;
    }
    sc_json_free(alloc, parsed);
    return SC_ERR_PROVIDER_RESPONSE;
}
#endif /* SC_GATEWAY_POSIX && !SC_IS_TEST */

static sc_error_t claude_cli_chat_with_system(void *ctx, sc_allocator_t *alloc,
    const char *system_prompt, size_t system_prompt_len,
    const char *message, size_t message_len,
    const char *model, size_t model_len,
    double temperature,
    char **out, size_t *out_len)
{
    (void)ctx;
    (void)temperature;
    (void)model;
    (void)model_len;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;

#if SC_IS_TEST
    const char *mock = "Hello from mock Claude CLI";
    size_t n = strlen(mock);
    char *buf = (char *)alloc->alloc(alloc->ctx, n + 1);
    if (!buf) return SC_ERR_OUT_OF_MEMORY;
    memcpy(buf, mock, n + 1);
    *out = buf;
    *out_len = n;
    return SC_OK;
#else
#ifdef SC_GATEWAY_POSIX
    {
        char combined[65536];
        size_t combined_len;
        if (system_prompt && system_prompt_len > 0) {
            if (system_prompt_len + message_len + 4 >= sizeof(combined))
                return SC_ERR_INVALID_ARGUMENT;
            memcpy(combined, system_prompt, system_prompt_len);
            combined[system_prompt_len] = '\n';
            combined[system_prompt_len + 1] = '\n';
            memcpy(combined + system_prompt_len + 2, message, message_len);
            combined_len = system_prompt_len + 2 + message_len;
        } else {
            if (message_len >= sizeof(combined)) return SC_ERR_INVALID_ARGUMENT;
            memcpy(combined, message, message_len);
            combined_len = message_len;
        }
        return run_claude_cli(alloc, combined, combined_len, model, model_len, out, out_len);
    }
#else
    (void)alloc;
    (void)system_prompt;
    (void)system_prompt_len;
    (void)message;
    (void)message_len;
    (void)model;
    (void)model_len;
    (void)out;
    (void)out_len;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

#if !SC_IS_TEST
static const char *extract_last_user_message(const sc_chat_message_t *msgs, size_t count, size_t *out_len) {
    for (size_t i = count; i > 0; i--) {
        if (msgs[i - 1].role == SC_ROLE_USER && msgs[i - 1].content && msgs[i - 1].content_len > 0) {
            *out_len = msgs[i - 1].content_len;
            return msgs[i - 1].content;
        }
    }
    return NULL;
}
#endif /* !SC_IS_TEST */

static sc_error_t claude_cli_chat(void *ctx, sc_allocator_t *alloc,
    const sc_chat_request_t *request,
    const char *model, size_t model_len,
    double temperature,
    sc_chat_response_t *out)
{
    (void)ctx;
    (void)temperature;
    (void)model;
    (void)model_len;
    (void)request;

#if SC_IS_TEST
    memset(out, 0, sizeof(*out));
    const char *content = "Hello from mock Claude CLI";
    size_t len = strlen(content);
    out->content = sc_strndup(alloc, content, len);
    out->content_len = len;
    return SC_OK;
#else
#ifdef SC_GATEWAY_POSIX
    {
        size_t prompt_len = 0;
        const char *prompt = extract_last_user_message(request->messages, request->messages_count, &prompt_len);
        if (!prompt) return SC_ERR_INVALID_ARGUMENT;

        char *text = NULL;
        size_t text_len = 0;
        sc_error_t err = run_claude_cli(alloc, prompt, prompt_len, model, model_len, &text, &text_len);
        if (err != SC_OK) return err;

        memset(out, 0, sizeof(*out));
        out->content = text;
        out->content_len = text_len;
        const char *rmodel = (model_len > 0) ? model : SC_CLAUDE_DEFAULT_MODEL;
        size_t rmodel_len = (model_len > 0) ? model_len : (sizeof(SC_CLAUDE_DEFAULT_MODEL) - 1);
        out->model = sc_strndup(alloc, rmodel, rmodel_len);
        out->model_len = rmodel_len;
        return SC_OK;
    }
#else
    (void)alloc;
    (void)request;
    (void)model;
    (void)model_len;
    (void)out;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

/* CLI wrapper shells out to `claude` binary; the CLI does not expose tool-calling or
 * streaming via its API, so these capabilities are inherently unsupported. */
static bool claude_cli_supports_native_tools(void *ctx) { (void)ctx; return false; }
static const char *claude_cli_get_name(void *ctx) { (void)ctx; return "claude-cli"; }
static void claude_cli_deinit(void *ctx, sc_allocator_t *alloc) {
    (void)alloc;
    sc_claude_cli_ctx_t *cc = (sc_claude_cli_ctx_t *)ctx;
    if (cc && cc->claude_path) { free(cc->claude_path); free(cc); }
    else if (cc) free(cc);
}

static const sc_provider_vtable_t claude_cli_vtable = {
    .chat_with_system = claude_cli_chat_with_system,
    .chat = claude_cli_chat,
    .supports_native_tools = claude_cli_supports_native_tools,
    .get_name = claude_cli_get_name,
    .deinit = claude_cli_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    /* stream_chat NULL: CLI wrapper cannot stream; claude CLI returns complete output only. */
    .supports_streaming = NULL, .supports_vision = NULL,
    .supports_vision_for_model = NULL, .stream_chat = NULL,
};

sc_error_t sc_claude_cli_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out)
{
    (void)alloc;
    (void)api_key;
    (void)api_key_len;
    (void)base_url;
    (void)base_url_len;
    sc_claude_cli_ctx_t *cc = (sc_claude_cli_ctx_t *)calloc(1, sizeof(*cc));
    if (!cc) return SC_ERR_OUT_OF_MEMORY;
    cc->claude_path = NULL;
    cc->claude_path_len = 0;
    out->ctx = cc;
    out->vtable = &claude_cli_vtable;
    return SC_OK;
}
