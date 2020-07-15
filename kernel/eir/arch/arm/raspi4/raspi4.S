.section .text.init
.global eirEntry
eirEntry:
	.extern eirStackTop
	ldr x1, =eirStackTop
	mov sp, x1

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
	.extern eirRaspi4Main
	// x0 -> pointer to device tree binary
	bl eirRaspi4Main

.halt:
	wfe
	b .halt
