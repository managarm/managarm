.set .L_uarRead, 1
.set .L_uarWrite, 2

.set .L_currentUarOff, 32

.global doCopyFromUser
.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarRead

.text
doCopyFromUser:
	ldr x3, =1b
	mrs x4, tpidr_el1
	str x3, [x4, .L_currentUarOff]
	cbz x2, 3f
2:
	ldrb w8, [x1], #1
	strb w8, [x0], #1
	subs x2, x2, #1
	b.ne 2b
3:
	mov x0, xzr
	str x0, [x4, .L_currentUarOff]
	ret
4:
	mov x0, xzr
	str x0, [x4, .L_currentUarOff]
	mov x0, #1
	ret

.global doCopyToUser

.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarWrite

.text
doCopyToUser:
	ldr x3, =1b
	mrs x4, tpidr_el1
	str x3, [x4, .L_currentUarOff]
	cbz x2, 3f
2:
	ldrb w8, [x1], #1
	subs x2, x2, #1
	strb w8, [x0], #1
	b.ne 2b
3:
	mov x0, xzr
	str x0, [x4, .L_currentUarOff]
	ret
4:
	mov x0, xzr
	str x0, [x4, .L_currentUarOff]
	mov x0, #1
	ret

.global doAtomicUserLoad

.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarRead | .L_uarWrite

.text
doAtomicUserLoad:
	ldr x3, =1b
	mrs x4, tpidr_el1
	str x3, [x4, .L_currentUarOff]
2:
	ldar x2, [x1]
3:
	stlr x2, [x0]
	mov x0, xzr
	str x0, [x4, .L_currentUarOff]
	ret
4:
	mov x0, xzr
	str x0, [x4, .L_currentUarOff]
	mov x0, #1
	ret

	.section .note.GNU-stack,"",%progbits
