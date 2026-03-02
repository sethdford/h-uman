/*
 * ChaCha20 — AArch64 implementation (scalar core, NEON for XOR).
 * ARM64 calling convention: x0=key, x1=nonce, x2=counter, x3=in, x4=out, x5=len
 */
.text

#if defined(__APPLE__)
#define sc_chacha20_encrypt_aarch64 _sc_chacha20_encrypt_aarch64
#define sc_chacha20_decrypt_aarch64 _sc_chacha20_decrypt_aarch64
#endif

.globl sc_chacha20_encrypt_aarch64
.globl sc_chacha20_decrypt_aarch64

.align 4

/* Quarter round: a+=b, d^=a, d=rotl(d,16), c+=d, b^=c, b=rotl(b,12),
   a+=b, d^=a, d=rotl(d,8), c+=d, b^=c, b=rotl(b,7).
   rotl32(x,n) = rotr32(x, 32-n). So rotr #16, #20, #24, #25. */
.macro qr_offs a, b, c, d
    ldr     w9, [x8, #\a]
    ldr     w10, [x8, #\b]
    add     w9, w9, w10
    str     w9, [x8, #\a]
    ldr     w11, [x8, #\d]
    eor     w11, w11, w9
    ror     w11, w11, #16
    str     w11, [x8, #\d]
    ldr     w9, [x8, #\c]
    add     w9, w9, w11
    str     w9, [x8, #\c]
    ldr     w10, [x8, #\b]
    eor     w10, w10, w9
    ror     w10, w10, #20
    str     w10, [x8, #\b]
    ldr     w9, [x8, #\a]
    add     w9, w9, w10
    str     w9, [x8, #\a]
    ldr     w11, [x8, #\d]
    eor     w11, w11, w9
    ror     w11, w11, #24
    str     w11, [x8, #\d]
    ldr     w9, [x8, #\c]
    add     w9, w9, w11
    str     w9, [x8, #\c]
    ldr     w10, [x8, #\b]
    eor     w10, w10, w9
    ror     w10, w10, #25
    str     w10, [x8, #\b]
.endm

sc_chacha20_encrypt_aarch64:
    stp     x19, x20, [sp, #-64]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    str     x25, [sp, #48]

    mov     x19, x0
    mov     x20, x1
    mov     w21, w2
    mov     x22, x3
    mov     x23, x4
    mov     x24, x5

.Lchacha_outer:
    cbz     x24, .Lchacha_done

    sub     sp, sp, #128
    mov     x8, sp

    /* State: constants "expa" "nd 3" "2-by" "te k" */
    movz    w9, #0x7865
    movk    w9, #0x6170, lsl #16
    str     w9, [x8, #0]
    movz    w9, #0x646e
    movk    w9, #0x3320, lsl #16
    str     w9, [x8, #4]
    movz    w9, #0x2d32
    movk    w9, #0x7962, lsl #16
    str     w9, [x8, #8]
    movz    w9, #0x6574
    movk    w9, #0x6b20, lsl #16
    str     w9, [x8, #12]
    ldp     q0, q1, [x19]
    stp     q0, q1, [x8, #16]
    str     w21, [x8, #48]
    ldr     x9, [x20]
    str     x9, [x8, #52]
    ldr     w9, [x20, #8]
    str     w9, [x8, #60]

    /* Copy initial to 64-127 for final add */
    ldp     q0, q1, [x8]
    ldp     q2, q3, [x8, #32]
    stp     q0, q1, [x8, #64]
    stp     q2, q3, [x8, #96]

    /* 10 double-rounds */
    mov     w25, #10
.Lchacha_round:
    qr_offs 0, 16, 32, 48
    qr_offs 4, 20, 36, 52
    qr_offs 8, 24, 40, 56
    qr_offs 12, 28, 44, 60
    qr_offs 0, 20, 40, 60
    qr_offs 4, 24, 44, 48
    qr_offs 8, 28, 32, 52
    qr_offs 12, 16, 36, 56
    subs    w25, w25, #1
    b.ne    .Lchacha_round

    /* Add initial state */
    ldp     q0, q1, [x8]
    ldp     q2, q3, [x8, #32]
    ldp     q4, q5, [x8, #64]
    ldp     q6, q7, [x8, #96]
    add     v0.4s, v0.4s, v4.4s
    add     v1.4s, v1.4s, v5.4s
    add     v2.4s, v2.4s, v6.4s
    add     v3.4s, v3.4s, v7.4s
    stp     q0, q1, [x8]
    stp     q2, q3, [x8, #32]

    /* Chunk size */
    cmp     x24, #64
    mov     x9, #64
    csel    x25, x24, x9, ls

    /* XOR: 16 bytes at a time with NEON */
    mov     x26, xzr
.Lchacha_xor:
    cmp     x26, x25
    b.hs    .Lchacha_xor_done
    sub     x27, x25, x26
    cmp     x27, #16
    b.lo    .Lchacha_xor_byte
    ldr     q16, [sp, x26]
    ldr     q17, [x22, x26]
    eor     v16.16b, v16.16b, v17.16b
    str     q16, [x23, x26]
    add     x26, x26, #16
    b       .Lchacha_xor
.Lchacha_xor_byte:
    ldrb    w9, [sp, x26]
    ldrb    w10, [x22, x26]
    eor     w9, w9, w10
    strb    w9, [x23, x26]
    add     x26, x26, #1
    b       .Lchacha_xor
.Lchacha_xor_done:
    add     sp, sp, #128

    add     x22, x22, x25
    add     x23, x23, x25
    sub     x24, x24, x25
    add     w21, w21, #1
    b       .Lchacha_outer

.Lchacha_done:
    ldp     x19, x20, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x23, x24, [sp], #16
    ldr     x25, [sp], #16
    ret

sc_chacha20_decrypt_aarch64:
    b       sc_chacha20_encrypt_aarch64
