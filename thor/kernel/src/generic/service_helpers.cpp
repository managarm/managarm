
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

void fiberSleep(uint64_t nanos) {
	auto this_fiber = thisFiber();

	struct Blocker : PrecisionTimerNode {
		Blocker(uint64_t ticks, KernelFiber *fiber)
		: PrecisionTimerNode{ticks}, complete{false}, fiber{fiber} { }

		void onElapse() override {
			complete.store(true, std::memory_order_release);
			fiber->unblock();
		}

		std::atomic<bool> complete;
		KernelFiber *fiber;
	};

	Blocker blocker{currentTicks() + durationToTicks(0, 0, 0, nanos), this_fiber};
	installTimer(&blocker);

	while(!blocker.complete.load(std::memory_order_acquire)) {
		auto check = [&] {
			return !blocker.complete.load(std::memory_order_relaxed);
		};
		KernelFiber::blockCurrent(wrap<bool()>(check));
	}
}

LaneHandle fiberOffer(LaneHandle lane) {
	auto this_fiber = thisFiber();
	std::atomic<bool> complete{false};

	auto callback = [&] (Error error) {
		assert(!error);
		complete.store(true, std::memory_order_release);
		this_fiber->unblock();
	};

	auto branch = lane.getStream()->submitOffer(lane.getLane(), wrap<void(Error)>(callback));

	while(!complete.load(std::memory_order_acquire)) {
		auto check = [&] {
			return !complete.load(std::memory_order_relaxed);
		};
		KernelFiber::blockCurrent(wrap<bool()>(check));
	}
	return branch;
}

LaneHandle fiberAccept(LaneHandle lane) {
	auto this_fiber = thisFiber();
	std::atomic<bool> complete{false};

	Error error;
	LaneDescriptor handle;
	auto callback = [&] (Error the_error, frigg::WeakPtr<Universe>, LaneDescriptor the_handle) {
		error = the_error;
		handle = std::move(the_handle);
		complete.store(true, std::memory_order_release);
		this_fiber->unblock();
	};

	lane.getStream()->submitAccept(lane.getLane(), frigg::WeakPtr<Universe>{},
			wrap<void(Error, frigg::WeakPtr<Universe>, LaneDescriptor)>(callback));

	while(!complete.load(std::memory_order_acquire)) {
		auto check = [&] {
			return !complete.load(std::memory_order_relaxed);
		};
		KernelFiber::blockCurrent(wrap<bool()>(check));
	}

	if(error == kErrEndOfLane)
		return LaneHandle{};
	assert(!error);
	return handle.handle;
}

void fiberSend(LaneHandle lane, const void *buffer, size_t length) {
	auto this_fiber = thisFiber();
	std::atomic<bool> complete{false};

	auto callback = [&] (Error error) {
		assert(!error);
		complete.store(true, std::memory_order_release);
		this_fiber->unblock();
	};

	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);
	lane.getStream()->submitSendBuffer(lane.getLane(),
			frigg::move(kernel_buffer), wrap<void(Error)>(callback));

	while(!complete.load(std::memory_order_acquire)) {
		auto check = [&] {
			return !complete.load(std::memory_order_relaxed);
		};
		KernelFiber::blockCurrent(wrap<bool()>(check));
	}
}

frigg::UniqueMemory<KernelAlloc> fiberRecv(LaneHandle lane) {
	auto this_fiber = thisFiber();
	std::atomic<bool> complete{false};

	frigg::UniqueMemory<KernelAlloc> buffer;
	auto callback = [&] (Error error, frigg::UniqueMemory<KernelAlloc> the_buffer) {
		assert(!error);
		buffer = std::move(the_buffer);
		complete.store(true, std::memory_order_release);
		this_fiber->unblock();
	};

	lane.getStream()->submitRecvInline(lane.getLane(),
			wrap<void(Error, frigg::UniqueMemory<KernelAlloc>)>(callback));

	while(!complete.load(std::memory_order_acquire)) {
		auto check = [&] {
			return !complete.load(std::memory_order_relaxed);
		};
		KernelFiber::blockCurrent(wrap<bool()>(check));
	}
	return frigg::move(buffer);
}

void fiberPushDescriptor(LaneHandle lane, AnyDescriptor descriptor) {
	auto this_fiber = thisFiber();
	std::atomic<bool> complete{false};

	auto callback = [&] (Error error) {
		assert(!error);
		complete.store(true, std::memory_order_release);
		this_fiber->unblock();
	};

	lane.getStream()->submitPushDescriptor(lane.getLane(), frigg::move(descriptor),
			wrap<void(Error)>(callback));

	while(!complete.load(std::memory_order_acquire)) {
		auto check = [&] {
			return !complete.load(std::memory_order_relaxed);
		};
		KernelFiber::blockCurrent(wrap<bool()>(check));
	}
}

AnyDescriptor fiberPullDescriptor(LaneHandle lane) {
	auto this_fiber = thisFiber();
	std::atomic<bool> complete{false};

	AnyDescriptor descriptor;
	auto callback = [&] (Error error, frigg::WeakPtr<Universe>, AnyDescriptor the_descriptor) {
		assert(!error);
		descriptor = std::move(the_descriptor);
		complete.store(true, std::memory_order_release);
		this_fiber->unblock();
	};

	lane.getStream()->submitPullDescriptor(lane.getLane(), frigg::WeakPtr<Universe>{},
			wrap<void(Error, frigg::WeakPtr<Universe>, AnyDescriptor)>(callback));

	while(!complete.load(std::memory_order_acquire)) {
		auto check = [&] {
			return !complete.load(std::memory_order_relaxed);
		};
		KernelFiber::blockCurrent(wrap<bool()>(check));
	}
	return frigg::move(descriptor);
}

} // namespace thor

