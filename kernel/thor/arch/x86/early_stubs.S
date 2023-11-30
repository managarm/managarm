
.global earlyStubDivideByZero
earlyStubDivideByZero:
	popq %rdi # faulting ip
	call handleEarlyDivideByZeroFault
	ud2

.global earlyStubOpcode
earlyStubOpcode:
	popq %rdi # faulting ip
	call handleEarlyOpcodeFault
	ud2

.global earlyStubDouble
earlyStubDouble:
	popq %rdi # error code
	popq %rsi # faulting ip
	call handleEarlyDoubleFault
	ud2

.global earlyStubProtection
earlyStubProtection:
	popq %rdi # error code
	popq %rsi # faulting ip
	call handleEarlyProtectionFault
	ud2

.global earlyStubPage
earlyStubPage:
	popq %rdi # error code
	popq %rsi # faulting ip
	call handleEarlyPageFault
	ud2

	.section .note.GNU-stack,"",%progbits
