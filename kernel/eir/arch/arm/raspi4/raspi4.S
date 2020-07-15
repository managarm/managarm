.section .text.init
.global eirRtEntry
eirRtEntry:
	.extern eirRtStackTop
	ldr x1, =eirRtStackTop
	mov sp, x1

	// Zero out BSS
	.extern eirRtBssStart
	.extern eirRtBssEnd
	ldr x5, =eirRtBssStart
	ldr x6, =eirRtBssEnd
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
