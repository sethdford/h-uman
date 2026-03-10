#ifndef HU_MEMORY_VECTOR_CIRCUIT_BREAKER_H
#define HU_MEMORY_VECTOR_CIRCUIT_BREAKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_cb_state {
    HU_CB_CLOSED,
    HU_CB_OPEN,
    HU_CB_HALF_OPEN,
} hu_cb_state_t;

typedef struct hu_circuit_breaker {
    hu_cb_state_t state;
    uint32_t failure_count;
    uint32_t threshold;
    uint64_t cooldown_ns;
    int64_t last_failure_ns;
    bool half_open_probe_sent;
} hu_circuit_breaker_t;

/* Initialize with threshold (failures before open) and cooldown (ms). */
void hu_circuit_breaker_init(hu_circuit_breaker_t *cb, uint32_t threshold, uint32_t cooldown_ms);

/* Can we attempt an operation? */
bool hu_circuit_breaker_allow(hu_circuit_breaker_t *cb);

/* Record success: reset to closed. */
void hu_circuit_breaker_record_success(hu_circuit_breaker_t *cb);

/* Record failure: increment, trip at threshold. */
void hu_circuit_breaker_record_failure(hu_circuit_breaker_t *cb);

/* Is the breaker open? */
bool hu_circuit_breaker_is_open(const hu_circuit_breaker_t *cb);

#endif /* HU_MEMORY_VECTOR_CIRCUIT_BREAKER_H */
