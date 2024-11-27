.set .L_tpCurrentUar, 0x28

.set .L_uarRead, 1
.set .L_uarWrite, 2

.option norelax

#------------------------------------------------------------------------------
# doCopyFromUser.
#------------------------------------------------------------------------------

.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarRead

.text
.global doCopyFromUser
# a0: dst
# a1: src
# a2: size
.p2align 2
doCopyFromUser:
	la t0, 1b
	sd t0, .L_tpCurrentUar(tp) # Setup the UAR.

2:
	# TODO: It would be easy to improve this to not do a byte-wise copy.
	beqz a2, 3f
	lbu t0, (a1)
	sb t0, (a0)
	addi a0, a0, 1
	addi a1, a1, 1
	addi a2, a2, -1
	j 2b

3:
	sd zero, .L_tpCurrentUar(tp) # Clean up the UAR.
	li a0, 0
	ret

4:
	sd zero, .L_tpCurrentUar(tp) # Clean up the UAR.
	li a0, 1
	ret

#------------------------------------------------------------------------------
# doCopyToUser.
#------------------------------------------------------------------------------

.data
1:
	.quad 2f
	.quad 3f
	.quad 4f
	.int .L_uarWrite

.text
.global doCopyToUser
# a0: dst
# a1: src
# a2: size
.p2align 2
doCopyToUser:
	la t0, 1b
	sd t0, .L_tpCurrentUar(tp) # Setup the UAR.

2:
	# TODO: It would be easy to improve this to not do a byte-wise copy.
	beqz a2, 3f
	lbu t0, (a1)
	sb t0, (a0)
	addi a0, a0, 1
	addi a1, a1, 1
	addi a2, a2, -1
	j 2b

3:
	sd zero, .L_tpCurrentUar(tp) # Clean up the UAR.
	li a0, 0
	ret

4:
	sd zero, .L_tpCurrentUar(tp) # Clean up the UAR.
	li a0, 1
	ret

#------------------------------------------------------------------------------

.section .note.GNU-stack,"",%progbits
