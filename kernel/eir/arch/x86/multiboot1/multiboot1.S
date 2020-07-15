.section .header
	.int 0x1BADB002 # Magic
	.int 0x4 # Enable graphic fields in the header
	.int -0x1BADB006 # Checksum
	.int 0 # File format: header_addr
	.int 0 # File format: load_addr
	.int 0 # File format: load_end_addr
	.int 0 # File format: bss_end_addr
	.int 0 # File format: entry_addr
	.int 0 # Graphics: Linear mode
	.int 0 # Graphics: width
	.int 0 # Graphics: height
	.int 32 # Graphics: depth

.section .text
.global eirRtEntry
eirRtEntry:
    cld

	.extern eirRtStackTop
	mov $eirRtStackTop, %esp

	push %eax
	push %ebx
	
	.extern eirMultiboot1Main
	call eirMultiboot1Main

halt_kernel:
	hlt
	jmp halt_kernel