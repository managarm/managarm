
#include "service_helpers.hpp"
#include "fiber.hpp"
#include "../arch/x86/hpet.hpp"

namespace thor {

namespace {
	template<typename S, typename F>
	struct LambdaInvoker;
	
	template<typename R, typename... Args, typename F>
	struct LambdaInvoker<R(Args...), F> {
		static R invoke(void *object, Args... args) {
			return (*static_cast<F *>(object))(frigg::move(args)...);
		}
	};

	template<typename S, typename F>
	frigg::CallbackPtr<S> wrap(F &functor) {
		return frigg::CallbackPtr<S>(&functor, &LambdaInvoker<S, F>::invoke);
	}
}

void fiberCopyToBundle(MemoryView *bundle, ptrdiff_t offset, const void *pointer, size_t size) {
	struct Closure {
		static void copied(CopyToBundleNode *base) {
			auto closure = frg::container_of(base, &Closure::copy);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		CopyToBundleNode copy;
	} closure;

	closure.blocker.setup();
	if(!copyToBundle(bundle, offset, pointer, size, &closure.copy, &Closure::copied))
		KernelFiber::blockCurrent(&closure.blocker);
}

void fiberCopyFromBundle(MemoryView *bundle, ptrdiff_t offset, void *pointer, size_t size) {
	struct Closure {
		static void copied(CopyFromBundleNode *base) {
			auto closure = frg::container_of(base, &Closure::copy);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		CopyFromBundleNode copy;
	} closure;

	closure.blocker.setup();
	if(!copyFromBundle(bundle, offset, pointer, size, &closure.copy, &Closure::copied))
		KernelFiber::blockCurrent(&closure.blocker);
}

void fiberSleep(uint64_t nanos) {
	struct Closure {
		static void elapsed(Worklet *worklet) {
//			frigg::infoLogger() << "Timer is elapsed" << frigg::endLog;
			auto closure = frg::container_of(worklet, &Closure::worklet);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		Worklet worklet;
		PrecisionTimerNode timer;
	} closure;

	closure.blocker.setup();
	closure.worklet.setup(&Closure::elapsed);
	closure.timer.setup(systemClockSource()->currentNanos() + nanos, &closure.worklet);
	generalTimerEngine()->installTimer(&closure.timer);
	KernelFiber::blockCurrent(&closure.blocker);
}

LaneHandle fiberOffer(LaneHandle lane) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	LaneHandle handle;
	auto callback = [&] (Error error, LaneHandle the_handle) {
		assert(!error);
		handle = std::move(the_handle);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	submitOffer(lane, wrap<void(Error, LaneHandle)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);

	return handle;
}

LaneHandle fiberAccept(LaneHandle lane) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	Error error;
	LaneHandle handle;
	auto callback = [&] (Error the_error, LaneHandle the_handle) {
		error = the_error;
		handle = std::move(the_handle);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	submitAccept(lane, wrap<void(Error, LaneHandle)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);

	if(error == kErrEndOfLane)
		return LaneHandle{};
	assert(!error);
	return handle;
}

void fiberSend(LaneHandle lane, const void *buffer, size_t length) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	auto callback = [&] (Error error) {
		assert(!error);
		KernelFiber::unblockOther(&closure.blocker);
	};

	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);

	closure.blocker.setup();
	submitSendBuffer(lane,
			frigg::move(kernel_buffer), wrap<void(Error)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);
}

frigg::UniqueMemory<KernelAlloc> fiberRecv(LaneHandle lane) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	frigg::UniqueMemory<KernelAlloc> buffer;
	auto callback = [&] (Error error, frigg::UniqueMemory<KernelAlloc> the_buffer) {
		assert(!error);
		buffer = std::move(the_buffer);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	submitRecvInline(lane,
			wrap<void(Error, frigg::UniqueMemory<KernelAlloc>)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);

	return frigg::move(buffer);
}

void fiberPushDescriptor(LaneHandle lane, AnyDescriptor descriptor) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	auto callback = [&] (Error error) {
		assert(!error);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	submitPushDescriptor(lane, frigg::move(descriptor),
			wrap<void(Error)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);
}

AnyDescriptor fiberPullDescriptor(LaneHandle lane) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	AnyDescriptor descriptor;
	auto callback = [&] (Error error, AnyDescriptor the_descriptor) {
		assert(!error);
		descriptor = std::move(the_descriptor);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	submitPullDescriptor(lane,
			wrap<void(Error, AnyDescriptor)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);

	return frigg::move(descriptor);
}

} // namespace thor

