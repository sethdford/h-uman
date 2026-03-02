#include "seaclaw/providers/codex_cli.h"
#include "seaclaw/provider.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>

#ifdef SC_GATEWAY_POSIX
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

#define SC_CODEX_CLI_NAME "codex"
#define SC_CODEX_DEFAULT_MODEL "codex-mini-latest"

typedef struct sc_codex_cli_ctx {
    char *model;
    size_t model_len;
} sc_codex_cli_ctx_t;

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

#if defined(SC_GATEWAY_POSIX) && !SC_IS_TEST
static sc_error_t run_codex(sc_allocator_t *alloc,
    const char *prompt, size_t prompt_len,
    char **out, size_t *out_len)
{
    char prompt_buf[65536];
    if (prompt_len >= sizeof(prompt_buf) - 1) return SC_ERR_INVALID_ARGUMENT;
    memcpy(prompt_buf, prompt, prompt_len);
    prompt_buf[prompt_len] = '\0';

    char *argv[] = {
        (char *)SC_CODEX_CLI_NAME,
        "--quiet",
        prompt_buf,
        NULL
    };

    int fds[2];
    if (pipe(fds) != 0) return SC_ERR_IO;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return SC_ERR_IO;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execvp(SC_CODEX_CLI_NAME, argv);
        _exit(127);
    }

    close(fds[1]);
    size_t cap = 4 * 1024 * 1024;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf) {
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return SC_ERR_OUT_OF_MEMORY;
    }
    size_t len = 0;
    for (;;) {
        if (len >= cap - 1) break;
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    close(fds[0]);

    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        alloc->free(alloc->ctx, buf, cap);
        return SC_ERR_PROVIDER_RESPONSE;
    }

    /* Trim trailing whitespace */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t' || buf[len - 1] == '\r' || buf[len - 1] == '\n'))
        len--;
    buf[len] = '\0';

    *out = sc_strndup(alloc, buf, len);
    alloc->free(alloc->ctx, buf, cap);
    if (!*out) return SC_ERR_OUT_OF_MEMORY;
    *out_len = len;
    return SC_OK;
}
#endif /* SC_GATEWAY_POSIX && !SC_IS_TEST */

static sc_error_t codex_cli_chat_with_system(void *ctx, sc_allocator_t *alloc,
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
    const char *mock = "Hello from mock Codex CLI";
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
            if (system_prompt_len + message_len + 4 >= sizeof(combined)) return SC_ERR_INVALID_ARGUMENT;
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
        return run_codex(alloc, combined, combined_len, out, out_len);
    }
#else
    (void)alloc; (void)system_prompt; (void)system_prompt_len; (void)message; (void)message_len;
    (void)out; (void)out_len;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

static sc_error_t codex_cli_chat(void *ctx, sc_allocator_t *alloc,
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
    const char *content = "Hello from mock Codex CLI";
    size_t len = strlen(content);
    out->content = sc_strndup(alloc, content, len);
    out->content_len = len;
    out->model = sc_strndup(alloc, "codex-cli", 9);
    out->model_len = 9;
    return SC_OK;
#else
#ifdef SC_GATEWAY_POSIX
    {
        size_t prompt_len = 0;
        const char *prompt = extract_last_user_message(request->messages, request->messages_count, &prompt_len);
        if (!prompt) return SC_ERR_INVALID_ARGUMENT;

        char *text = NULL;
        size_t text_len = 0;
        sc_error_t err = run_codex(alloc, prompt, prompt_len, &text, &text_len);
        if (err != SC_OK) return err;

        memset(out, 0, sizeof(*out));
        out->content = text;
        out->content_len = text_len;
        out->model = sc_strndup(alloc, "codex-cli", 9);
        out->model_len = 9;
        return SC_OK;
    }
#else
    (void)alloc; (void)request; (void)out;
    return SC_ERR_NOT_SUPPORTED;
#endif
#endif
}

/* CLI wrapper shells out to `codex` binary; the CLI does not expose tool-calling or
 * streaming via its API, so these capabilities are inherently unsupported. */
static bool codex_cli_supports_native_tools(void *ctx) { (void)ctx; return false; }
static const char *codex_cli_get_name(void *ctx) { (void)ctx; return "codex-cli"; }
static void codex_cli_deinit(void *ctx, sc_allocator_t *alloc) {
    sc_codex_cli_ctx_t *c = (sc_codex_cli_ctx_t *)ctx;
    if (c && c->model) alloc->free(alloc->ctx, c->model, c->model_len + 1);
    if (c) alloc->free(alloc->ctx, c, sizeof(*c));
}

static const sc_provider_vtable_t codex_cli_vtable = {
    .chat_with_system = codex_cli_chat_with_system,
    .chat = codex_cli_chat,
    .supports_native_tools = codex_cli_supports_native_tools,
    .get_name = codex_cli_get_name,
    .deinit = codex_cli_deinit,
    .warmup = NULL, .chat_with_tools = NULL,
    /* stream_chat NULL: CLI wrapper cannot stream; codex CLI returns complete output only. */
    .supports_streaming = NULL, .supports_vision = NULL,
    .supports_vision_for_model = NULL, .stream_chat = NULL,
};

sc_error_t sc_codex_cli_create(sc_allocator_t *alloc,
    const char *api_key, size_t api_key_len,
    const char *base_url, size_t base_url_len,
    sc_provider_t *out)
{
    (void)api_key;
    (void)api_key_len;
    (void)base_url;
    (void)base_url_len;
    sc_codex_cli_ctx_t *c = (sc_codex_cli_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->model = sc_strndup(alloc, SC_CODEX_DEFAULT_MODEL, sizeof(SC_CODEX_DEFAULT_MODEL) - 1);
    c->model_len = sizeof(SC_CODEX_DEFAULT_MODEL) - 1;
    out->ctx = c;
    out->vtable = &codex_cli_vtable;
    return SC_OK;
}
