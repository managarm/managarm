.section .header
.align 4
header_start:
	.long 0xE85250D6 # Magic
	.long 0 # Architecture: i386
	.long (header_end - header_start) # Length
	.long -(0xE85250D6 + 0 + (header_end - header_start)) # Checksum

	.align 8

	# Framebuffer
	.short 5 # Type
	.short 0 # Flags
	.long 20 # Size
	.long 0 # Width: No preference
	.long 0 # Height: No preference
	.long 32 # Depth

	.align 8

	# End tag
	.short 0 # Type
	.short 0 # Flags
	.long 8 # Size
header_end:

.section .text
.global eirEntry

eirEntry:
	cld
	
	.extern eirStackTop
	mov $eirStackTop, %esp
	
	push %eax
	push %ebx
	
	.extern eirMultiboot2Main
	call eirMultiboot2Main
halt_kernel:
	hlt
	jmp halt_kernel
