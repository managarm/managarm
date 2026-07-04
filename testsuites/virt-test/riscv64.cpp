#include <hel.h>
#include <hel-syscalls.h>
#include <iostream>
#include <assert.h>

asm(R"(
.pushsection .data
.global testCode
.global testCodeEnd
testCode:
	wfi
	ecall
	li t1, 0x2000
	ld t0, (t1)
	sd t0, (t1)
	// Interrupt injected here.
	ecall
testCodeEnd:
.popsection
)");

extern "C" char testCode[];
extern "C" char testCodeEnd[];

int main() {
	HelHandle vspace, vcpu, mem;
	HEL_CHECK(helCreateVirtualizedSpace(&vspace));

	HEL_CHECK(helAllocateMemory(0x10000, 0, nullptr, &mem));

	void *fake_ptr;
	HEL_CHECK(helMapMemory(mem, vspace, nullptr, 0x0, 0x1000, kHelMapFixed | kHelMapProtRead | kHelMapProtWrite | kHelMapProtExecute, &fake_ptr));
	assert(fake_ptr == nullptr);

	void *actual_ptr;
	HEL_CHECK(helMapMemory(mem, kHelNullHandle, nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &actual_ptr));

	memcpy(actual_ptr, &testCode, reinterpret_cast<uintptr_t>(&testCodeEnd) - reinterpret_cast<uintptr_t>(&testCode));

	HEL_CHECK(helCreateVirtualizedCpu(vspace, &vcpu));

	HelRiscv64VirtualizationRegs regs{};
	HEL_CHECK(helLoadRegisters(vcpu, kHelRegsVirtualization, (void *)&regs));

	assert(regs.kernelMode);

	HelVmexitReason reason;

	auto runStep = [&] {
		HEL_CHECK(helRunVirtualizedCpu(vcpu, &reason));

		HEL_CHECK(helLoadRegisters(vcpu, kHelRegsVirtualization, (void *)&regs));
		regs.pc += 4;
		HEL_CHECK(helStoreRegisters(vcpu, kHelRegsVirtualization, (void *)&regs));
	};

	runStep();
	assert(reason.exitReason == kHelVmexitInstructionTrap);
	// wfi
	assert(reason.instruction == 0x10500073);

	runStep();
	assert(reason.exitReason == kHelVmexitHyperCall);

	runStep();
	assert(reason.exitReason == kHelVmexitTranslationFault);
	assert(reason.address == 0x2000);
	assert(reason.flags == kHelVmFaultRead);
	assert(reason.instruction == 0x3283);

	runStep();
	assert(reason.exitReason == kHelVmexitTranslationFault);
	assert(reason.address == 0x2000);
	assert(reason.flags == kHelVmFaultWrite);
	assert(reason.instruction == 0x503023);

	// Assert VSEI (external interrupt).
	HEL_CHECK(helAssertVirtualizedIrq(vcpu, 10, true));

	runStep();
	assert(reason.exitReason == kHelVmexitHyperCall);
	assert(regs.sip & (1 << 9));

	helCloseDescriptor(kHelThisUniverse, vcpu);
	helCloseDescriptor(kHelThisUniverse, vspace);
	helCloseDescriptor(kHelThisUniverse, mem);

	return 0;
}
