#include "seaclaw/core/allocator.h"
#include "synthetic_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SC_SYNTH_CLI_BUF 65536

sc_error_t sc_synth_run_cli(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                            sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics) {
    (void)gemini;
    SC_SYNTH_LOG("=== CLI Tests ===");
    sc_synth_metrics_init(metrics);
    int n = cfg->tests_per_category > 0 ? cfg->tests_per_category : 5;
    for (int i = 0; i < n; i++) {
        int pipefd[2];
        if (pipe(pipefd) != 0)
            return SC_ERR_IO;
        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return SC_ERR_IO;
        }
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            execl(cfg->binary_path, cfg->binary_path, "version", (char *)NULL);
            _exit(127);
        }
        close(pipefd[1]);
        char buf[SC_SYNTH_CLI_BUF];
        size_t total = 0;
        ssize_t rd;
        while (total < sizeof(buf) - 1 &&
               (rd = read(pipefd[0], buf + total, sizeof(buf) - 1 - total)) > 0)
            total += (size_t)rd;
        close(pipefd[0]);
        buf[total] = '\0';
        int st = 0;
        waitpid(pid, &st, 0);
        double t0 = sc_synth_now_ms();
        (void)t0;
        sc_synth_verdict_t v =
            (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? SC_SYNTH_PASS : SC_SYNTH_FAIL;
        sc_synth_metrics_record(alloc, metrics, 0.0, v);
        SC_SYNTH_VERBOSE(cfg, "CLI run %d: %s", i + 1, v == SC_SYNTH_PASS ? "PASS" : "FAIL");
    }
    return SC_OK;
}
