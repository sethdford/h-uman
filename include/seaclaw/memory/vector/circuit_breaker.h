#ifndef SC_MEMORY_VECTOR_CIRCUIT_BREAKER_H
#define SC_MEMORY_VECTOR_CIRCUIT_BREAKER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum sc_cb_state {
    SC_CB_CLOSED,
    SC_CB_OPEN,
    SC_CB_HALF_OPEN,
} sc_cb_state_t;

typedef struct sc_circuit_breaker {
    sc_cb_state_t state;
    uint32_t failure_count;
    uint32_t threshold;
    uint64_t cooldown_ns;
    int64_t last_failure_ns;
    bool half_open_probe_sent;
} sc_circuit_breaker_t;

/* Initialize with threshold (failures before open) and cooldown (ms). */
void sc_circuit_breaker_init(sc_circuit_breaker_t *cb, uint32_t threshold, uint32_t cooldown_ms);

/* Can we attempt an operation? */
bool sc_circuit_breaker_allow(sc_circuit_breaker_t *cb);

/* Record success: reset to closed. */
void sc_circuit_breaker_record_success(sc_circuit_breaker_t *cb);

/* Record failure: increment, trip at threshold. */
void sc_circuit_breaker_record_failure(sc_circuit_breaker_t *cb);

/* Is the breaker open? */
bool sc_circuit_breaker_is_open(const sc_circuit_breaker_t *cb);

#endif /* SC_MEMORY_VECTOR_CIRCUIT_BREAKER_H */
