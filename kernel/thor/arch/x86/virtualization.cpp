#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/virtualization.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace x86_virt {

bool isCanonical(uint64_t address) {
	return static_cast<uint64_t>(static_cast<int64_t>(address << 16) >> 16) == address;
}

bool isValidXcr0(uint64_t value) {
	auto features = getGlobalCpuFeatures();
	uint64_t supported = features->haveXsave ? features->xcr0Mask : XCR0_X87;
	if(value & ~supported)
		return false;
	// x87 is always enabled.
	if(!(value & XCR0_X87))
		return false;
	// AVX requires SSE.
	if((value & XCR0_AVX) && !(value & XCR0_SSE))
		return false;
	// The AVX-512 components can only be enabled together and they require AVX.
	if(value & XCR0_AVX512) {
		if((value & XCR0_AVX512) != XCR0_AVX512 || !(value & XCR0_AVX))
			return false;
	}
	return true;
}

} // namespace x86_virt

using namespace x86_virt;

bool X86VirtualizedCpu::assertInterrupt(uint64_t number, bool level) {
	if(number > 0xFF || !lapic_.post(number, level))
		return false;
	notify();
	return true;
}

void X86VirtualizedCpu::queueException(uint8_t vector, bool hasErrorCode, uint32_t errorCode) {
	// Exceptions raised by emulated instructions are delivered on the next entry.
	reinjectValid_ = true;
	reinject_ = {};
	reinject_.type = kEventHardwareException;
	reinject_.vector = vector;
	reinject_.hasErrorCode = hasErrorCode;
	reinject_.errorCode = errorCode;
}

frg::optional<Error> X86VirtualizedCpu::applyCompletion(const HelVmexitCompletion *completion,
		uint64_t &rax, uint64_t &rbx, uint64_t &rcx, uint64_t &rdx, bool &skipInstruction) {
	skipInstruction = false;

	// Exits that need a completion must get one, and no other exit may get one.
	if(completionPending_ != (completion != nullptr))
		return Error::illegalState;
	if(completion && completion->status != kHelVmexitCompletionDone
			&& completion->status != kHelVmexitCompletionFault)
		return Error::illegalArgs;

	if(!completion)
		return frg::null_opt;

	completionPending_ = false;
	auto &pending = pendingCompletion_;
	if(completion->status == kHelVmexitCompletionFault) {
		queueException(GP_VECTOR, true, 0);
		return frg::null_opt;
	}

	const uint64_t *data = completion->data;
	switch(pending.exitReason) {
	case kHelVmexitIo:
		if(pending.ioIn && !pending.ioString) {
			if(pending.ioSize == 4) {
				// Like other 32-bit operations, IN EAX zero-extends RAX.
				rax = data[0] & 0xFFFF'FFFF;
			}else{
				uint64_t mask = (UINT64_C(1) << (pending.ioSize * 8)) - 1;
				rax = (rax & ~mask) | (data[0] & mask);
			}
		}
		break;
	case kHelVmexitCpuid:
		rax = data[0] & 0xFFFF'FFFF;
		rbx = data[1] & 0xFFFF'FFFF;
		rcx = data[2] & 0xFFFF'FFFF;
		rdx = data[3] & 0xFFFF'FFFF;
		break;
	case kHelVmexitRdmsr:
		rax = data[0] & 0xFFFF'FFFF;
		rdx = data[0] >> 32;
		break;
	case kHelVmexitHyperCall:
		rax = data[0];
		break;
	default:
		break;
	}
	skipInstruction = true;
	return frg::null_opt;
}

HelVmexitReason X86VirtualizedCpu::requestMsrCompletion(bool write, uint64_t rax, uint64_t rcx,
		uint64_t rdx, uint32_t instructionLength) {
	HelVmexitReason exitInfo{};
	completionPending_ = true;
	pendingCompletion_ = {};
	pendingCompletion_.instructionLength = instructionLength;
	pendingCompletion_.exitReason = write ? kHelVmexitWrmsr : kHelVmexitRdmsr;
	exitInfo.address = rcx & 0xFFFF'FFFF;
	if(write)
		exitInfo.data = (rdx << 32) | (rax & 0xFFFF'FFFF);
	exitInfo.exitReason = pendingCompletion_.exitReason;
	return exitInfo;
}

HelVmexitReason X86VirtualizedCpu::requestCpuidCompletion(uint64_t rax, uint64_t rcx,
		uint32_t instructionLength) {
	HelVmexitReason exitInfo{};
	completionPending_ = true;
	pendingCompletion_ = {};
	pendingCompletion_.instructionLength = instructionLength;
	pendingCompletion_.exitReason = kHelVmexitCpuid;
	exitInfo.address = rax & 0xFFFF'FFFF;
	exitInfo.data = rcx & 0xFFFF'FFFF;
	exitInfo.exitReason = pendingCompletion_.exitReason;
	return exitInfo;
}

HelVmexitReason X86VirtualizedCpu::requestHyperCallCompletion(uint32_t instructionLength) {
	HelVmexitReason exitInfo{};
	completionPending_ = true;
	pendingCompletion_ = {};
	pendingCompletion_.instructionLength = instructionLength;
	pendingCompletion_.exitReason = kHelVmexitHyperCall;
	exitInfo.exitReason = pendingCompletion_.exitReason;
	return exitInfo;
}

HelVmexitReason X86VirtualizedCpu::requestIoCompletion(uint16_t port, uint8_t size, bool in,
		bool string, uint64_t rax, uint32_t instructionLength) {
	HelVmexitReason exitInfo{};
	completionPending_ = true;
	pendingCompletion_ = {};
	pendingCompletion_.instructionLength = instructionLength;
	pendingCompletion_.exitReason = kHelVmexitIo;
	pendingCompletion_.ioSize = size;
	pendingCompletion_.ioIn = in;
	pendingCompletion_.ioString = string;
	exitInfo.address = port;
	exitInfo.code = size;
	exitInfo.flags = 0;
	if(in)
		exitInfo.flags |= kHelVmexitIoIn;
	if(string) {
		exitInfo.flags |= kHelVmexitIoString;
	}else if(!in) {
		exitInfo.data = size == 4 ? (rax & 0xFFFF'FFFF)
				: (rax & ((UINT64_C(1) << (size * 8)) - 1));
	}
	exitInfo.exitReason = pendingCompletion_.exitReason;
	return exitInfo;
}

bool X86VirtualizedCpu::emulateLapicMsr(uint32_t index, bool write, uint64_t now,
		VirtualLapic::WriteEffects &effects, uint64_t &rax, uint64_t &rdx) {
	bool ok;
	if(write) {
		uint64_t value = (rdx << 32) | (rax & 0xFFFF'FFFF);
		ok = lapic_.writeMsr(index, value, now, effects);
	}else{
		uint64_t value;
		ok = lapic_.readMsr(index, now, value);
		if(ok) {
			rax = value & 0xFFFF'FFFF;
			rdx = value >> 32;
		}
	}
	if(!ok)
		queueException(GP_VECTOR, true, 0);
	return ok;
}

void X86VirtualizedCpu::waitForGuestInterrupt(frg::optional<uint64_t> deadline,
		bool interruptsEnabled) {
	// notify() raises wakeEvent_ after changing the state that cond checks;
	// the condition is checked under the event's lock, so we cannot miss a wakeup.
	auto cond = [&] {
		if(kickPending())
			return false;
		if(deadline && getClockNanos() >= *deadline)
			return false;
		return !(interruptsEnabled && lapic_.deliverableVector() >= 0);
	};
	auto wq = getCurrentThread()->mainWorkQueue().get();
	// The loop in run() reevaluates the state after waking up; the outcome does not matter.
	if(!deadline) {
		(void)Thread::asyncBlockCurrentInterruptible(
			[&](async::cancellation_token ct) {
				return wakeEvent_.async_wait_if(cond, ct);
			},
			wq
		);
		return;
	}

	Thread::asyncBlockCurrentInterruptible([&](async::cancellation_token ct) {
		return async::race_and_cancel(
			[&](async::cancellation_token cancellation) {
				return async::transform(wakeEvent_.async_wait_if(cond, cancellation),
						[](auto) {});
			},
			[&](async::cancellation_token cancellation) {
				return async::transform(generalTimerEngine()->sleep(*deadline, cancellation),
						[](bool) {});
			},
			[ct](async::cancellation_token cancellation) {
				return async::suspend_indefinitely(ct, cancellation);
			}
		);
	}, wq);
}

} // namespace thor
