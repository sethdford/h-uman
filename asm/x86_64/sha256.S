/*
 * SHA-256 — x86_64 with SHA-NI (Intel SHA Extensions).
 * Called only when CPUID indicates SHA-NI support; otherwise dispatch
 * uses sc_sha256_generic.
 * System V AMD64 ABI: rdi=data, rsi=len, rdx=out
 *
 * Uses sha256rnds2, sha256msg1, sha256msg2 for full block processing.
 * SHA256RNDS2: XMM0 = schedule (W+K), first op = CDGH (gets new ABEF),
 *              second op = ABEF (becomes new CDGH). Operands alternate each pair.
 */
.text

#if defined(__APPLE__)
#define sc_sha256_x86_64 _sc_sha256_x86_64
#define sc_sha256_generic _sc_sha256_generic
#endif

.globl sc_sha256_x86_64

/* pshufb mask: byteswap each dword (LE -> BE for SHA-256) */
.Lbswap_mask:
    .byte 3, 2, 1, 0, 7, 6, 5, 4
    .byte 11, 10, 9, 8, 15, 14, 13, 12

/* Initial hash state H0-H7 (big-endian 32-bit) */
.LH0: .long 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a
.LH1: .long 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19

/* Round constants K[0..63] as (K_i, K_{i+1}) pairs for sha256rnds2 */
.LK: .long 0x428a2f98, 0x71374491
     .long 0xb5c0fbcf, 0xe9b5dba5
     .long 0x3956c25b, 0x59f111f1
     .long 0x923f82a4, 0xab1c5ed5
     .long 0xd807aa98, 0x12835b01
     .long 0x243185be, 0x550c7dc3
     .long 0x72be5d74, 0x80deb1fe
     .long 0x9bdc06a7, 0xc19bf174
     .long 0xe49b69c1, 0xefbe4786
     .long 0x0fc19dc6, 0x240ca1cc
     .long 0x2de92c6f, 0x4a7484aa
     .long 0x5cb0a9dc, 0x76f988da
     .long 0x983e5152, 0xa831c66d
     .long 0xb00327c8, 0xbf597fc7
     .long 0xc6e00bf3, 0xd5a79147
     .long 0x06ca6351, 0x14292967
     .long 0x27b70a85, 0x2e1b2138
     .long 0x4d2c6dfc, 0x53380d13
     .long 0x650a7354, 0x766a0abb
     .long 0x81c2c92e, 0x92722c85
     .long 0xa2bfe8a1, 0xa81a664b
     .long 0xc24b8b70, 0xc76c51a3
     .long 0xd192e819, 0xd6990624
     .long 0xf40e3585, 0x106aa070
     .long 0x19a4c116, 0x1e376c08
     .long 0x2748774c, 0x34b0bcb5
     .long 0x391c0cb3, 0x4ed8aa4a
     .long 0x5b9cca4f, 0x682e6ff3
     .long 0x748f82ee, 0x78a5636f
     .long 0x84c87814, 0x8cc70208
     .long 0x90befffa, 0xa4506ceb
     .long 0xbef9a3f7, 0xc67178f2

.align 16
sc_sha256_x86_64:
    pushq %rbp
    movq %rsp, %rbp
    subq $256, %rsp
    andq $-16, %rsp

    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rdi, %r12          /* r12 = data */
    movq %rsi, %r13         /* r13 = len */
    movq %rdx, %r14         /* r14 = out */
    movq %rsi, %rbx         /* rbx = total len (for final block) */

    /* If len < 64, tail to generic (handles all padding) */
    cmpq $64, %r13
    jb .Lsha_tail_generic

    /* Load initial state: ABEF = [H0,H1,H4,H5], CDGH = [H2,H3,H6,H7] */
    movdqu .LH0(%rip), %xmm2     /* [H0,H1,H2,H3] */
    movdqu .LH1(%rip), %xmm3     /* [H4,H5,H6,H7] */
    movdqa %xmm2, %xmm4
    punpckldq %xmm3, %xmm2   /* [H0,H4,H1,H5] */
    pshufd $0xD8, %xmm2, %xmm2   /* [H0,H1,H4,H5] = ABEF */
    punpckhdq %xmm3, %xmm4   /* [H2,H6,H3,H7] */
    pshufd $0xD8, %xmm4, %xmm4   /* [H2,H3,H6,H7] = CDGH */

    leaq .Lbswap_mask(%rip), %r15
    movdqa (%r15), %xmm8

.Lsha_block_loop:
    cmpq $64, %r13
    jb .Lsha_final

    /* Save state, run transform on block at r12 */
    movdqa %xmm2, %xmm6
    movdqa %xmm4, %xmm7
    call .Lsha_transform_block
    addq $64, %r12
    subq $64, %r13
    jmp .Lsha_block_loop

.Lsha_transform_block:
    /* Block ptr in r12. XMM0=schedule (implicit), xmm2=ABEF, xmm4=CDGH. */
    /* Rounds 0-1: schedule in XMM0, sha256rnds2(CDGH, ABEF) */
    movdqu 0(%r12), %xmm0
    pshufb %xmm8, %xmm0
    movdqa %xmm0, %xmm9          /* MSG0 */
    paddd .LK+0*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2     /* CDGH gets new ABEF, xmm2 = new CDGH */
    pshufd $0x0E, %xmm0, %xmm0   /* next 2 rounds schedule */
    sha256rnds2 %xmm2, %xmm4     /* alternate */

    /* Rounds 4-7 */
    movdqu 16(%r12), %xmm1
    pshufb %xmm8, %xmm1
    movdqa %xmm1, %xmm10         /* MSG1 */
    paddd .LK+2*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm9, %xmm10

    /* Rounds 8-11 */
    movdqu 32(%r12), %xmm0
    pshufb %xmm8, %xmm0
    movdqa %xmm0, %xmm11         /* MSG2 */
    paddd .LK+4*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2
    pshufd $0x0E, %xmm0, %xmm0
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm10, %xmm11

    /* Rounds 12-15 */
    movdqu 48(%r12), %xmm1
    pshufb %xmm8, %xmm1
    movdqa %xmm1, %xmm12         /* MSG3 */
    paddd .LK+6*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm11, %xmm12
    paddd %xmm12, %xmm9
    sha256msg2 %xmm9, %xmm12
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm11, %xmm12

    /* Rounds 16-19 */
    movdqa %xmm9, %xmm0
    paddd .LK+8*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm12, %xmm9
    paddd %xmm9, %xmm10
    sha256msg2 %xmm10, %xmm9
    pshufd $0x0E, %xmm0, %xmm0
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm12, %xmm9

    /* Rounds 20-23 */
    movdqa %xmm10, %xmm1
    paddd .LK+10*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm9, %xmm10
    paddd %xmm10, %xmm11
    sha256msg2 %xmm11, %xmm10
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm9, %xmm10

    /* Rounds 24-27 */
    movdqa %xmm11, %xmm0
    paddd .LK+12*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm10, %xmm11
    paddd %xmm11, %xmm12
    sha256msg2 %xmm12, %xmm11
    pshufd $0x0E, %xmm0, %xmm0
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm10, %xmm11

    /* Rounds 28-31 */
    movdqa %xmm12, %xmm1
    paddd .LK+14*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm11, %xmm12
    paddd %xmm12, %xmm9
    sha256msg2 %xmm9, %xmm12
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm11, %xmm12

    /* Rounds 32-35 */
    movdqa %xmm9, %xmm0
    paddd .LK+16*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm12, %xmm9
    paddd %xmm9, %xmm10
    sha256msg2 %xmm10, %xmm9
    pshufd $0x0E, %xmm0, %xmm0
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm12, %xmm9

    /* Rounds 36-39 */
    movdqa %xmm10, %xmm1
    paddd .LK+18*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm9, %xmm10
    paddd %xmm10, %xmm11
    sha256msg2 %xmm11, %xmm10
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm9, %xmm10

    /* Rounds 40-43 */
    movdqa %xmm11, %xmm0
    paddd .LK+20*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm10, %xmm11
    paddd %xmm11, %xmm12
    sha256msg2 %xmm12, %xmm11
    pshufd $0x0E, %xmm0, %xmm0
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm10, %xmm11

    /* Rounds 44-47 */
    movdqa %xmm12, %xmm1
    paddd .LK+22*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm11, %xmm12
    paddd %xmm12, %xmm9
    sha256msg2 %xmm9, %xmm12
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm11, %xmm12

    /* Rounds 48-51 */
    movdqa %xmm9, %xmm0
    paddd .LK+24*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm12, %xmm9
    paddd %xmm9, %xmm10
    sha256msg2 %xmm10, %xmm9
    pshufd $0x0E, %xmm0, %xmm0
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm12, %xmm9

    /* Rounds 52-55 */
    movdqa %xmm10, %xmm1
    paddd .LK+26*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm9, %xmm10
    paddd %xmm10, %xmm11
    sha256msg2 %xmm11, %xmm10
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm9, %xmm10

    /* Rounds 56-59 */
    movdqa %xmm11, %xmm0
    paddd .LK+28*8(%rip), %xmm0
    sha256rnds2 %xmm4, %xmm2
    palignr $4, %xmm10, %xmm11
    paddd %xmm11, %xmm12
    sha256msg2 %xmm12, %xmm11
    pshufd $0x0E, %xmm0, %xmm0
    sha256rnds2 %xmm2, %xmm4
    sha256msg1 %xmm10, %xmm11

    /* Rounds 60-63 */
    movdqa %xmm12, %xmm1
    paddd .LK+30*8(%rip), %xmm1
    sha256rnds2 %xmm4, %xmm2
    pshufd $0x0E, %xmm1, %xmm1
    sha256rnds2 %xmm2, %xmm4

    /* Add saved state (xmm2=ABEF, xmm4=CDGH after last sha256rnds2) */
    paddd %xmm6, %xmm2
    paddd %xmm7, %xmm4
    ret

.Lsha_final:
    leaq 64(%rsp), %r15          /* block buffer */
    leaq 128(%rsp), %r11         /* second block (two-block case) */

    testq %r13, %r13
    jz .Lsha_pad_block_only

    /* Copy remainder to block, append 0x80 */
    movq %r15, %rdi
    movq %r12, %rsi
    movq %r13, %rcx
    rep movsb
    movb $0x80, (%r15,%r13)
    addq $1, %r13

    cmpq $56, %r13
    jbe .Lsha_pad_single_block

    /* Two blocks: zero rest of first, transform both */
    movq %r15, %rdi
    addq %r13, %rdi
    movq $64, %rcx
    subq %r13, %rcx
    xorl %eax, %eax
    rep stosb

    movdqa %xmm2, %xmm6
    movdqa %xmm4, %xmm7
    movq %r15, %r12
    call .Lsha_transform_block

    movq %r11, %rdi
    xorl %eax, %eax
    movq $56, %rcx
    rep stosb
    movq %rbx, %rax
    shlq $3, %rax
    bswapq %rax
    movq %rax, 56(%r11)
    movdqa %xmm2, %xmm6
    movdqa %xmm4, %xmm7
    movq %r11, %r12
    call .Lsha_transform_block
    jmp .Lsha_output

.Lsha_pad_single_block:
    movq %r15, %rdi
    addq %r13, %rdi
    movq $56, %rcx
    subq %r13, %rcx
    xorl %eax, %eax
    rep stosb
    movq %rbx, %rax
    shlq $3, %rax
    bswapq %rax
    movq %rax, 56(%r15)
    movdqa %xmm2, %xmm6
    movdqa %xmm4, %xmm7
    movq %r15, %r12
    call .Lsha_transform_block
    jmp .Lsha_output

.Lsha_pad_block_only:
    movb $0x80, (%r15)
    leaq 1(%r15), %rdi
    movq $55, %rcx
    xorl %eax, %eax
    rep stosb
    xorq %rax, %rax
    movq %rax, 56(%r15)
    movdqa %xmm2, %xmm6
    movdqa %xmm4, %xmm7
    movq %r15, %r12
    call .Lsha_transform_block

.Lsha_output:
    /* xmm2=[A,B,E,F], xmm4=[C,D,G,H]. Output [A,B,C,D,E,F,G,H] big-endian. */
    movdqa %xmm2, %xmm0
    punpcklqdq %xmm4, %xmm0
    punpckhqdq %xmm4, %xmm2
    pshufb %xmm8, %xmm0
    pshufb %xmm8, %xmm2
    movdqu %xmm0, 0(%r14)
    movdqu %xmm2, 16(%r14)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    leave
    ret

.Lsha_tail_generic:
    movq %r12, %rdi
    movq %rbx, %rsi
    movq %r14, %rdx
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    leave
    jmp sc_sha256_generic

#if !defined(__APPLE__)
.section .note.GNU-stack,"",@progbits
#endif
