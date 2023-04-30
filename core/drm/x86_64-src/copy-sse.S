
.text
.global fastCopy16
.type fastCopy16, @function
fastCopy16:
        leaq    .L4(%rip), %rcx
        shrq    $4, %rdx
        movq    %rdx, %rax
        andl    $7, %eax
        movl    (%rcx, %rax, 4), %eax
        addq    %rcx, %rax
        jmp     *%rax
.L4:
        .long .L13 - .L4
        .long .L10 - .L4
        .long .L9  - .L4
        .long .L8  - .L4
        .long .L7  - .L4
        .long .L6  - .L4
        .long .L5  - .L4
        .long .L3  - .L4
.L10:
        movdqa  (%rsi), %xmm6
        movaps  %xmm6, (%rdi)
.L9:
        movdqa  16(%rsi), %xmm5
        movaps  %xmm5, 16(%rdi)
.L8:
        movdqa  32(%rsi), %xmm4
        movaps  %xmm4, 32(%rdi)
.L7:
        movdqa  48(%rsi), %xmm3
        movaps  %xmm3, 48(%rdi)
.L6:
        movdqa  64(%rsi), %xmm2
        movaps  %xmm2, 64(%rdi)
.L5:
        movdqa  80(%rsi), %xmm1
        movaps  %xmm1, 80(%rdi)
.L3:
        movdqa  96(%rsi), %xmm0
        movaps  %xmm0, 96(%rdi)
.L2:
        cmpq    %rdx, %rax
        jnb     .L1
        movq    %rax, %rcx
        salq    $4, %rcx
        addq    %rcx, %rsi
        addq    %rcx, %rdi
.L12:
        movdqa  (%rsi), %xmm0
        addq    $8, %rax
        subq    $-128, %rsi
        subq    $-128, %rdi
        movaps  %xmm0, -128(%rdi)
        movdqa  -112(%rsi), %xmm1
        movaps  %xmm1, -112(%rdi)
        movdqa  -96(%rsi), %xmm2
        movaps  %xmm2, -96(%rdi)
        movdqa  -80(%rsi), %xmm3
        movaps  %xmm3, -80(%rdi)
        movdqa  -64(%rsi), %xmm4
        movaps  %xmm4, -64(%rdi)
        movdqa  -48(%rsi), %xmm5
        movaps  %xmm5, -48(%rdi)
        movdqa  -32(%rsi), %xmm6
        movaps  %xmm6, -32(%rdi)
        movdqa  -16(%rsi), %xmm7
        movaps  %xmm7, -16(%rdi)
        cmpq    %rax, %rdx
        ja      .L12
.L1:
        ret
.L13:
        xorl    %eax, %eax
        jmp     .L2

	.section .note.GNU-stack,"",%progbits
