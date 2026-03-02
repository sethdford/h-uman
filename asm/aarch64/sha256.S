/*
 * SHA-256 — AArch64 with crypto extensions (SHA256H, SHA256H2, SHA256SU0, SHA256SU1).
 * ARM64 calling convention: x0=data, x1=len, x2=out
 * Falls back to sc_sha256_generic when crypto extensions not available.
 * Note: ID_AA64ISAR0_EL1 is privileged; use getauxval(AT_HWCAP2) in C for feature detection.
 */
.text

#if defined(__APPLE__)
#define sc_sha256_aarch64 _sc_sha256_aarch64
#define sc_sha256_generic _sc_sha256_generic
#endif

.globl sc_sha256_aarch64

.align 4

sc_sha256_aarch64:
    /* Crypto extensions (SHA256H, SHA256H2, SHA256SU0, SHA256SU1) would go here.
     * For now tail-call generic C. Args already in x0,x1,x2. */
    b       sc_sha256_generic
