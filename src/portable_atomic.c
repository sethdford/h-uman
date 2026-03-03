#include "seaclaw/portable_atomic.h"
#include "seaclaw/core/allocator.h"
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

struct sc_atomic_u64 {
    _Atomic uint64_t value;
};

struct sc_atomic_i64 {
    _Atomic int64_t value;
};

struct sc_atomic_bool {
    _Atomic int value;
};

sc_atomic_u64_t *sc_atomic_u64_create(uint64_t value) {
    sc_atomic_u64_t *a = (sc_atomic_u64_t *)malloc(sizeof(sc_atomic_u64_t));
    if (a)
        atomic_init(&a->value, value);
    return a;
}

void sc_atomic_u64_destroy(sc_atomic_u64_t *a) {
    if (a)
        free(a);
}

uint64_t sc_atomic_u64_load(const sc_atomic_u64_t *a) {
    return (a) ? atomic_load_explicit((_Atomic uint64_t *)&a->value, memory_order_acquire) : 0;
}

void sc_atomic_u64_store(sc_atomic_u64_t *a, uint64_t value) {
    if (a)
        atomic_store_explicit((_Atomic uint64_t *)&a->value, value, memory_order_release);
}

uint64_t sc_atomic_u64_fetch_add(sc_atomic_u64_t *a, uint64_t operand) {
    return (a) ? atomic_fetch_add_explicit((_Atomic uint64_t *)&a->value, operand,
                                           memory_order_acq_rel)
               : 0;
}

sc_atomic_i64_t *sc_atomic_i64_create(int64_t value) {
    sc_atomic_i64_t *a = (sc_atomic_i64_t *)malloc(sizeof(sc_atomic_i64_t));
    if (a)
        atomic_init(&a->value, value);
    return a;
}

void sc_atomic_i64_destroy(sc_atomic_i64_t *a) {
    if (a)
        free(a);
}

int64_t sc_atomic_i64_load(const sc_atomic_i64_t *a) {
    return (a) ? atomic_load_explicit((_Atomic int64_t *)&a->value, memory_order_acquire) : 0;
}

void sc_atomic_i64_store(sc_atomic_i64_t *a, int64_t value) {
    if (a)
        atomic_store_explicit((_Atomic int64_t *)&a->value, value, memory_order_release);
}

sc_atomic_bool_t *sc_atomic_bool_create(int value) {
    sc_atomic_bool_t *a = (sc_atomic_bool_t *)malloc(sizeof(sc_atomic_bool_t));
    if (a)
        atomic_init(&a->value, value ? 1 : 0);
    return a;
}

void sc_atomic_bool_destroy(sc_atomic_bool_t *a) {
    if (a)
        free(a);
}

int sc_atomic_bool_load(const sc_atomic_bool_t *a) {
    return (a) ? (atomic_load_explicit((_Atomic int *)&a->value, memory_order_acquire) != 0) : 0;
}

void sc_atomic_bool_store(sc_atomic_bool_t *a, int value) {
    if (a)
        atomic_store_explicit((_Atomic int *)&a->value, value ? 1 : 0, memory_order_release);
}
