#ifndef HU_PORTABLE_ATOMIC_H
#define HU_PORTABLE_ATOMIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_atomic_u64 hu_atomic_u64_t;
typedef struct hu_atomic_i64 hu_atomic_i64_t;
typedef struct hu_atomic_bool hu_atomic_bool_t;

hu_atomic_u64_t *hu_atomic_u64_create(uint64_t value);
void hu_atomic_u64_destroy(hu_atomic_u64_t *a);
uint64_t hu_atomic_u64_load(const hu_atomic_u64_t *a);
void hu_atomic_u64_store(hu_atomic_u64_t *a, uint64_t value);
uint64_t hu_atomic_u64_fetch_add(hu_atomic_u64_t *a, uint64_t operand);

hu_atomic_i64_t *hu_atomic_i64_create(int64_t value);
void hu_atomic_i64_destroy(hu_atomic_i64_t *a);
int64_t hu_atomic_i64_load(const hu_atomic_i64_t *a);
void hu_atomic_i64_store(hu_atomic_i64_t *a, int64_t value);

hu_atomic_bool_t *hu_atomic_bool_create(int value);
void hu_atomic_bool_destroy(hu_atomic_bool_t *a);
int hu_atomic_bool_load(const hu_atomic_bool_t *a);
void hu_atomic_bool_store(hu_atomic_bool_t *a, int value);

#ifdef __cplusplus
}
#endif

#endif /* HU_PORTABLE_ATOMIC_H */
