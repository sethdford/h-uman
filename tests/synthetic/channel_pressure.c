#include "channel_harness.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct pressure_result {
    int total;
    int passed;
    int failed;
    double total_latency_ms;
    double max_latency_ms;
} pressure_result_t;

static void worker_loop(hu_allocator_t *alloc, const hu_channel_test_entry_t *entry,
                        int duration_secs, int pipe_fd) {
    pressure_result_t res = {0};
    time_t start = time(NULL);

    hu_channel_t ch = {0};
    if (entry->create(alloc, &ch) != HU_OK) {
        res.failed = 1;
        (void)write(pipe_fd, &res, sizeof(res));
        close(pipe_fd);
        return;
    }

    while ((time(NULL) - start) < duration_secs) {
        double t0 = hu_synth_now_ms();
        const char *msg = "pressure test message";
        hu_error_t err = entry->inject(&ch, "pressure_user", 13, msg, 21);
        if (err != HU_OK) {
            res.failed++;
            res.total++;
            continue;
        }

        if (entry->poll) {
            hu_channel_loop_msg_t msgs[16];
            size_t count = 0;
            err = entry->poll(ch.ctx, alloc, msgs, 16, &count);
            if (err != HU_OK || count == 0) {
                res.failed++;
                res.total++;
                continue;
            }
        }

        if (ch.vtable && ch.vtable->send) {
            err = ch.vtable->send(ch.ctx, "pressure_user", 13, "reply", 5, NULL, 0);
            if (err != HU_OK) {
                res.failed++;
                res.total++;
                continue;
            }
        }

        double lat = hu_synth_now_ms() - t0;
        res.total++;
        res.passed++;
        res.total_latency_ms += lat;
        if (lat > res.max_latency_ms)
            res.max_latency_ms = lat;
    }

    entry->destroy(&ch);
    (void)write(pipe_fd, &res, sizeof(res));
    close(pipe_fd);
}

hu_error_t hu_channel_run_pressure(hu_allocator_t *alloc, const hu_channel_test_config_t *cfg,
                                   hu_synth_gemini_ctx_t *gemini, hu_synth_metrics_t *metrics) {
    (void)gemini;
    hu_synth_metrics_init(metrics);

    size_t reg_count = 0;
    const hu_channel_test_entry_t *reg = hu_channel_test_registry(&reg_count);
    if (reg_count == 0)
        return HU_OK;

    int workers = cfg->concurrency > 0 ? cfg->concurrency : 4;
    int duration = cfg->duration_secs > 0 ? cfg->duration_secs : 10;
    HU_CH_LOG("launching %d pressure workers for %ds", workers, duration);

    int *pipes = (int *)malloc((size_t)workers * 2 * sizeof(int));
    pid_t *pids = (pid_t *)malloc((size_t)workers * sizeof(pid_t));
    if (!pipes || !pids) {
        free(pipes);
        free(pids);
        return HU_ERR_OUT_OF_MEMORY;
    }

    for (int w = 0; w < workers; w++) {
        int pfd[2];
        if (pipe(pfd) < 0) {
            pids[w] = -1;
            continue;
        }
        pipes[w * 2] = pfd[0];
        pipes[w * 2 + 1] = pfd[1];

        /* Pick channel round-robin from filtered list */
        size_t ci = 0;
        if (cfg->all_channels || cfg->channel_count == 0) {
            ci = (size_t)w % reg_count;
        } else {
            size_t idx = (size_t)(w % (int)cfg->channel_count);
            const char *want = cfg->channels[idx];
            for (size_t i = 0; i < reg_count; i++) {
                if (strcmp(reg[i].name, want) == 0) {
                    ci = i;
                    break;
                }
            }
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            hu_allocator_t child_alloc = hu_system_allocator();
            worker_loop(&child_alloc, &reg[ci], duration, pfd[1]);
            _exit(0);
        }
        close(pfd[1]);
        pids[w] = pid;
    }

    /* Collect results */
    int total_ops = 0, total_pass = 0, total_fail = 0;
    double total_lat = 0, max_lat = 0;

    for (int w = 0; w < workers; w++) {
        if (pids[w] <= 0)
            continue;
        pressure_result_t res = {0};
        (void)read(pipes[w * 2], &res, sizeof(res));
        close(pipes[w * 2]);
        int status = 0;
        waitpid(pids[w], &status, 0);
        total_ops += res.total;
        total_pass += res.passed;
        total_fail += res.failed;
        total_lat += res.total_latency_ms;
        if (res.max_latency_ms > max_lat)
            max_lat = res.max_latency_ms;
    }

    double avg_lat = total_ops > 0 ? total_lat / (double)total_ops : 0;
    double rate = duration > 0 ? (double)total_ops / (double)duration : 0;
    HU_CH_LOG(
        "Pressure: %d workers, %ds, %d ops, %.1f ops/s, %d ok, %d fail, avg %.1fms, max %.1fms",
        workers, duration, total_ops, rate, total_pass, total_fail, avg_lat, max_lat);

    for (int i = 0; i < total_pass; i++)
        hu_synth_metrics_record(alloc, metrics, avg_lat, HU_SYNTH_PASS);
    for (int i = 0; i < total_fail; i++)
        hu_synth_metrics_record(alloc, metrics, 0, HU_SYNTH_FAIL);

    free(pipes);
    free(pids);
    return HU_OK;
}
