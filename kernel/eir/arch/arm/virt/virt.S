.section .text.init
.global eirEntry
eirEntry:
	.extern eirStackTop
	ldr x1, =eirStackTop
	mov sp, x1

	// Load vector table
	.extern eirExcVectors
	ldr x1, =eirExcVectors
	msr vbar_el1, x1

	// Zero out BSS
	.extern eirBssStart
	.extern eirBssEnd
	ldr x5, =eirBssStart
	ldr x6, =eirBssEnd
.loop:
	cmp x5, x6
	b.eq .enter
	str xzr, [x5]
	add x5, x5, #8
	b .loop

.enter:
	.extern eirVirtMain
	bl eirVirtMain

.halt:
	wfe
	b .halt
