.section .stivalehdr
.extern eirStackTop
stivale_header:
    .quad eirStackTop
    .word 1
    .word 0
    .word 0
    .word 32

.section .text
.global eirEntry

eirEntry:
    cld

    .extern eirStivaleMain
    call eirStivaleMain
halt_kernel:
    hlt
    jmp halt_kernel
