#include "human/portable_atomic.h"
#include "human/core/allocator.h"
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

struct hu_atomic_u64 {
    _Atomic uint64_t value;
};

struct hu_atomic_i64 {
    _Atomic int64_t value;
};

struct hu_atomic_bool {
    _Atomic int value;
};

hu_atomic_u64_t *hu_atomic_u64_create(uint64_t value) {
    hu_atomic_u64_t *a = (hu_atomic_u64_t *)malloc(sizeof(hu_atomic_u64_t));
    if (a)
        atomic_init(&a->value, value);
    return a;
}

void hu_atomic_u64_destroy(hu_atomic_u64_t *a) {
    if (a)
        free(a);
}

uint64_t hu_atomic_u64_load(const hu_atomic_u64_t *a) {
    return (a) ? atomic_load_explicit((_Atomic uint64_t *)&a->value, memory_order_acquire) : 0;
}

void hu_atomic_u64_store(hu_atomic_u64_t *a, uint64_t value) {
    if (a)
        atomic_store_explicit((_Atomic uint64_t *)&a->value, value, memory_order_release);
}

uint64_t hu_atomic_u64_fetch_add(hu_atomic_u64_t *a, uint64_t operand) {
    return (a) ? atomic_fetch_add_explicit((_Atomic uint64_t *)&a->value, operand,
                                           memory_order_acq_rel)
               : 0;
}

hu_atomic_i64_t *hu_atomic_i64_create(int64_t value) {
    hu_atomic_i64_t *a = (hu_atomic_i64_t *)malloc(sizeof(hu_atomic_i64_t));
    if (a)
        atomic_init(&a->value, value);
    return a;
}

void hu_atomic_i64_destroy(hu_atomic_i64_t *a) {
    if (a)
        free(a);
}

int64_t hu_atomic_i64_load(const hu_atomic_i64_t *a) {
    return (a) ? atomic_load_explicit((_Atomic int64_t *)&a->value, memory_order_acquire) : 0;
}

void hu_atomic_i64_store(hu_atomic_i64_t *a, int64_t value) {
    if (a)
        atomic_store_explicit((_Atomic int64_t *)&a->value, value, memory_order_release);
}

hu_atomic_bool_t *hu_atomic_bool_create(int value) {
    hu_atomic_bool_t *a = (hu_atomic_bool_t *)malloc(sizeof(hu_atomic_bool_t));
    if (a)
        atomic_init(&a->value, value ? 1 : 0);
    return a;
}

void hu_atomic_bool_destroy(hu_atomic_bool_t *a) {
    if (a)
        free(a);
}

int hu_atomic_bool_load(const hu_atomic_bool_t *a) {
    return (a) ? (atomic_load_explicit((_Atomic int *)&a->value, memory_order_acquire) != 0) : 0;
}

void hu_atomic_bool_store(hu_atomic_bool_t *a, int value) {
    if (a)
        atomic_store_explicit((_Atomic int *)&a->value, value ? 1 : 0, memory_order_release);
}
