.section .stivalehdr
.extern eirRtStackTop
stivale_header:
    .quad eirRtStackTop
    .word 1
    .word 0
    .word 0
    .word 32

.section .text
.global eirRtEntry

eirRtEntry:
    cld

    .extern eirStivaleMain
    call eirStivaleMain
halt_kernel:
    hlt
    jmp halt_kernel