/*
 * ChaCha20 — x86_64 SSE2 implementation.
 * System V AMD64 ABI: rdi=key, rsi=nonce, rdx=counter, rcx=in, r8=out, r9=len
 */
.text

#if defined(__APPLE__)
#define sc_chacha20_encrypt_x86_64 _sc_chacha20_encrypt_x86_64
#define sc_chacha20_decrypt_x86_64 _sc_chacha20_decrypt_x86_64
#endif

.globl sc_chacha20_encrypt_x86_64
.globl sc_chacha20_decrypt_x86_64

.section .note.GNU-stack,"",@progbits

/* Quarter round macro: (a,b,c,d) in memory or regs. Uses rax, r10, r11. */
#define QR_SCALAR(a_off, b_off, c_off, d_off) \
    movl a_off(%rsp), %eax; \
    addl b_off(%rsp), %eax; \
    movl %eax, a_off(%rsp); \
    movl d_off(%rsp), %r10d; \
    xorl %eax, %r10d; \
    roll $16, %r10d; \
    movl %r10d, d_off(%rsp); \
    movl c_off(%rsp), %eax; \
    addl %r10d, %eax; \
    movl %eax, c_off(%rsp); \
    movl b_off(%rsp), %r11d; \
    xorl %eax, %r11d; \
    roll $12, %r11d; \
    movl %r11d, b_off(%rsp); \
    movl a_off(%rsp), %eax; \
    addl %r11d, %eax; \
    movl %eax, a_off(%rsp); \
    movl d_off(%rsp), %r10d; \
    xorl %eax, %r10d; \
    roll $8, %r10d; \
    movl %r10d, d_off(%rsp); \
    movl c_off(%rsp), %eax; \
    addl %r10d, %eax; \
    movl %eax, c_off(%rsp); \
    movl b_off(%rsp), %r11d; \
    xorl %eax, %r11d; \
    roll $7, %r11d; \
    movl %r11d, b_off(%rsp)

.align 16
sc_chacha20_encrypt_x86_64:
    pushq %rbp
    movq %rsp, %rbp
    subq $128, %rsp
    andq $-16, %rsp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Save args (r9=len can get overwritten) */
    movq %rdi, %r12
    movq %rsi, %r13
    movl %edx, %r14d
    movq %rcx, %r15
    movq %r8, %rbx
    movq %r9, %r10

.Lchacha_outer:
    testq %r10, %r10
    jz .Lchacha_done

    /* Build state on stack: 16 x uint32_t */
    movl $0x61707865, 0(%rsp)
    movl $0x3320646e, 4(%rsp)
    movl $0x79622d32, 8(%rsp)
    movl $0x6b206574, 12(%rsp)
    movl 0(%r12), %eax
    movl %eax, 16(%rsp)
    movl 4(%r12), %eax
    movl %eax, 20(%rsp)
    movl 8(%r12), %eax
    movl %eax, 24(%rsp)
    movl 12(%r12), %eax
    movl %eax, 28(%rsp)
    movl 16(%r12), %eax
    movl %eax, 32(%rsp)
    movl 20(%r12), %eax
    movl %eax, 36(%rsp)
    movl 24(%r12), %eax
    movl %eax, 40(%rsp)
    movl 28(%r12), %eax
    movl %eax, 44(%rsp)
    movl %r14d, 48(%rsp)
    movl 0(%r13), %eax
    movl %eax, 52(%rsp)
    movl 4(%r13), %eax
    movl %eax, 56(%rsp)
    movl 8(%r13), %eax
    movl %eax, 60(%rsp)

    /* Save initial state at 64-127 for final add; working state at 0-63 */
    movdqa 0(%rsp), %xmm0
    movdqa 16(%rsp), %xmm1
    movdqa 32(%rsp), %xmm2
    movdqa 48(%rsp), %xmm3
    movdqa %xmm0, 64(%rsp)
    movdqa %xmm1, 80(%rsp)
    movdqa %xmm2, 96(%rsp)
    movdqa %xmm3, 112(%rsp)

    /* 10 double-rounds on working state 0-63 */
    movq $10, %r11
.Lchacha_round:
    /* Column round: (0,4,8,12), (1,5,9,13), (2,6,10,14), (3,7,11,15) */
    QR_SCALAR(0, 16, 32, 48)
    QR_SCALAR(4, 20, 36, 52)
    QR_SCALAR(8, 24, 40, 56)
    QR_SCALAR(12, 28, 44, 60)
    /* Diagonal round: (0,5,10,15), (1,6,11,12), (2,7,8,13), (3,4,9,14) */
    QR_SCALAR(0, 20, 40, 60)
    QR_SCALAR(4, 24, 44, 48)
    QR_SCALAR(8, 28, 32, 52)
    QR_SCALAR(12, 16, 36, 56)
    decq %r11
    jnz .Lchacha_round

    /* Add initial state to working state -> output block */
    movdqa 0(%rsp), %xmm0
    movdqa 16(%rsp), %xmm1
    movdqa 32(%rsp), %xmm2
    movdqa 48(%rsp), %xmm3
    paddd 64(%rsp), %xmm0
    paddd 80(%rsp), %xmm1
    paddd 96(%rsp), %xmm2
    paddd 112(%rsp), %xmm3

    /* Store keystream at 0-63 */
    movdqa %xmm0, 0(%rsp)
    movdqa %xmm1, 16(%rsp)
    movdqa %xmm2, 32(%rsp)
    movdqa %xmm3, 48(%rsp)
    /* Output is LE, x86 is LE - no swap needed, store as-is */

    movq %r10, %rcx
    cmpq $64, %rcx
    jb .Lchacha_partial
    movq $64, %rcx
.Lchacha_partial:
    /* XOR in with out, 16 bytes at a time with SSE2 */
    xorq %rax, %rax
.Lchacha_xor_loop:
    cmpq %rax, %rcx
    jbe .Lchacha_xor_done
    movq %rcx, %r8
    subq %rax, %r8
    cmpq $16, %r8
    jae .Lchacha_xor_16
    /* Scalar for last partial block */
    leaq (%rsp,%rax), %rdi
    leaq (%r15,%rax), %rsi
    leaq (%rbx,%rax), %rdx
    movq %r8, %r9
.Lchacha_xor_byte:
    movzbl (%rsi), %r10d
    xorb (%rdi), %r10b
    movb %r10b, (%rdx)
    incq %rsi
    incq %rdx
    incq %rdi
    decq %r9
    jnz .Lchacha_xor_byte
    addq %r8, %rax
    jmp .Lchacha_xor_done
.Lchacha_xor_16:
    movdqu (%r15,%rax), %xmm0
    movdqu (%rsp,%rax), %xmm1
    pxor %xmm1, %xmm0
    movdqu %xmm0, (%rbx,%rax)
    addq $16, %rax
    jmp .Lchacha_xor_loop
.Lchacha_xor_done:

    addq %rcx, %r15
    addq %rcx, %rbx
    subq %rcx, %r10
    incl %r14d
    jmp .Lchacha_outer

.Lchacha_done:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    leave
    ret

sc_chacha20_decrypt_x86_64:
    /* ChaCha20 decrypt = encrypt (XOR stream cipher) */
    jmp sc_chacha20_encrypt_x86_64
