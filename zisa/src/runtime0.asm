
.global zisaRtEntry
zisaRtEntry:
	# allocate memory for the stack
	mov $2, %rdi
	mov $0x1000, %rsi
	int $0x80
	# now %rsi contains the memory handle
	
	# map stack memory into this address space
	mov $6, %rdi
	# %rsi still contains the memory handle
	mov $0x2000000, %rdx
	mov $0x1000, %rcx
	int $0x80
	
	mov $0x2001000, %rsp

	.extern main
	call main

zisaIdle:
	jmp zisaIdle

.global syscall0
.global syscall1
.global syscall2
.global syscall3
syscall0:
syscall1:
syscall2:
syscall3:
	int $0x80
	ret

