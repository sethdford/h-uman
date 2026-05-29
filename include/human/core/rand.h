/* include/human/core/rand.h
 *
 * Portable non-cryptographic uniform random.
 *
 * macOS and the BSDs provide arc4random_uniform in <stdlib.h>. musl libc (the
 * alpine docker build) does NOT implement the arc4random family at all,
 * so a feature-test macro cannot expose it. This helper centralizes the
 * platform split: arc4random_uniform on BSD/macOS, getrandom/urandom +
 * rejection sampling on Linux and any other POSIX target.
 *
 * Use this for jitter, sampling, and selection — NOT for keys or nonces.
 * Cryptographic randomness must go through the security subsystem
 * (ks_random_bytes / secure_random_bytes), which fails closed rather than
 * returning low-entropy data.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * hu_rand_uniform() — Return a uniformly distributed value in [0, bound).
 *
 * Returns 0 when bound is 0 or 1 (the only value in range). The result is
 * free of modulo bias (rejection sampling on the Linux path; arc4random_uniform
 * is unbiased by construction on BSD/macOS).
 *
 * Thread-safe: the BSD/macOS path uses arc4random_uniform (thread-safe by
 * design); the Linux path is stateless (each call draws fresh OS entropy).
 *
 * Not for cryptographic use. If OS entropy is briefly unavailable on the
 * Linux path, the function degrades to a time/address-seeded PRNG rather
 * than blocking — acceptable for jitter/selection, never for secrets.
 */
uint32_t hu_rand_uniform(uint32_t bound);

#ifdef __cplusplus
}
#endif
