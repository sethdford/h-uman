#include "seaclaw/core/allocator.h"
#include "synthetic_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sc_error_t sc_synth_run_agent(sc_allocator_t *alloc, const sc_synth_config_t *cfg,
                              sc_synth_gemini_ctx_t *gemini, sc_synth_metrics_t *metrics) {
    (void)alloc;
    (void)cfg;
    (void)gemini;
    SC_SYNTH_LOG("=== Agent Tests ===");
    sc_synth_metrics_init(metrics);
    sc_synth_metrics_record(alloc, metrics, 0.0, SC_SYNTH_SKIP);
    SC_SYNTH_LOG("Agent tests skipped (not implemented)");
    return SC_OK;
}
