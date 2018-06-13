
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

void fiberCopyToBundle(Memory *bundle, ptrdiff_t offset, const void *pointer, size_t size) {
	struct Closure {
		static void copied(CopyToBundleNode *base) {
			auto closure = frg::container_of(base, &Closure::copy);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		CopyToBundleNode copy;
	} closure;

	closure.blocker.setup();
	copyToBundle(bundle, offset, pointer, size, &closure.copy, &Closure::copied);
	KernelFiber::blockCurrent(&closure.blocker);
}

void fiberCopyFromBundle(Memory *bundle, ptrdiff_t offset, void *pointer, size_t size) {
	struct Closure {
		static void copied(CopyFromBundleNode *base) {
			auto closure = frg::container_of(base, &Closure::copy);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		CopyFromBundleNode copy;
	} closure;

	closure.blocker.setup();
	copyFromBundle(bundle, offset, pointer, size, &closure.copy, &Closure::copied);
	KernelFiber::blockCurrent(&closure.blocker);
}

void fiberSleep(uint64_t nanos) {
	struct Closure {
		static void elapsed(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);
			KernelFiber::unblockOther(&closure->blocker);
		}

		FiberBlocker blocker;
		Worklet worklet;
		PrecisionTimerNode timer;
	} closure;

	closure.blocker.setup();
	closure.worklet.setup(&Closure::elapsed, WorkQueue::localQueue());
	closure.timer.setup(systemClockSource()->currentNanos() + nanos, &closure.worklet);
	generalTimerEngine()->installTimer(&closure.timer);
	KernelFiber::blockCurrent(&closure.blocker);
}

LaneHandle fiberOffer(LaneHandle lane) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	auto callback = [&] (Error error) {
		assert(!error);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	auto branch = lane.getStream()->submitOffer(lane.getLane(), wrap<void(Error)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);

	return branch;
}

LaneHandle fiberAccept(LaneHandle lane) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	Error error;
	LaneDescriptor handle;
	auto callback = [&] (Error the_error, frigg::WeakPtr<Universe>, LaneDescriptor the_handle) {
		error = the_error;
		handle = std::move(the_handle);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	lane.getStream()->submitAccept(lane.getLane(), frigg::WeakPtr<Universe>{},
			wrap<void(Error, frigg::WeakPtr<Universe>, LaneDescriptor)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);

	if(error == kErrEndOfLane)
		return LaneHandle{};
	assert(!error);
	return handle.handle;
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
	lane.getStream()->submitSendBuffer(lane.getLane(),
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
	lane.getStream()->submitRecvInline(lane.getLane(),
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
	lane.getStream()->submitPushDescriptor(lane.getLane(), frigg::move(descriptor),
			wrap<void(Error)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);
}

AnyDescriptor fiberPullDescriptor(LaneHandle lane) {
	struct Closure {
		FiberBlocker blocker;
	} closure;

	AnyDescriptor descriptor;
	auto callback = [&] (Error error, frigg::WeakPtr<Universe>, AnyDescriptor the_descriptor) {
		assert(!error);
		descriptor = std::move(the_descriptor);
		KernelFiber::unblockOther(&closure.blocker);
	};

	closure.blocker.setup();
	lane.getStream()->submitPullDescriptor(lane.getLane(), frigg::WeakPtr<Universe>{},
			wrap<void(Error, frigg::WeakPtr<Universe>, AnyDescriptor)>(callback));
	KernelFiber::blockCurrent(&closure.blocker);

	return frigg::move(descriptor);
}

} // namespace thor

