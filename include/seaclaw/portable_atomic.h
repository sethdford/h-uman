#ifndef SC_PORTABLE_ATOMIC_H
#define SC_PORTABLE_ATOMIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sc_atomic_u64 sc_atomic_u64_t;
typedef struct sc_atomic_i64 sc_atomic_i64_t;
typedef struct sc_atomic_bool sc_atomic_bool_t;

sc_atomic_u64_t *sc_atomic_u64_create(uint64_t value);
void sc_atomic_u64_destroy(sc_atomic_u64_t *a);
uint64_t sc_atomic_u64_load(const sc_atomic_u64_t *a);
void sc_atomic_u64_store(sc_atomic_u64_t *a, uint64_t value);
uint64_t sc_atomic_u64_fetch_add(sc_atomic_u64_t *a, uint64_t operand);

sc_atomic_i64_t *sc_atomic_i64_create(int64_t value);
void sc_atomic_i64_destroy(sc_atomic_i64_t *a);
int64_t sc_atomic_i64_load(const sc_atomic_i64_t *a);
void sc_atomic_i64_store(sc_atomic_i64_t *a, int64_t value);

sc_atomic_bool_t *sc_atomic_bool_create(int value);
void sc_atomic_bool_destroy(sc_atomic_bool_t *a);
int sc_atomic_bool_load(const sc_atomic_bool_t *a);
void sc_atomic_bool_store(sc_atomic_bool_t *a, int value);

#ifdef __cplusplus
}
#endif

#endif /* SC_PORTABLE_ATOMIC_H */
