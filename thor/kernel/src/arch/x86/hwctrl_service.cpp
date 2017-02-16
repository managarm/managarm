
#include <frigg/initializer.hpp>
#include <mbus.frigg_pb.hpp>
#include <hwctrl.frigg_pb.hpp>
#include "../../generic/fiber.hpp"
#include "../../generic/stream.hpp"
#include "hwctrl_service.hpp"
#include "pic.hpp"

namespace thor {

// TODO: Move this to a header file.
extern frigg::LazyInitializer<LaneHandle> mbusClient;

namespace arch_x86 {

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

		LaneDescriptor handle;
		auto callback = [&] (Error error, frigg::WeakPtr<Universe>, LaneDescriptor the_handle) {
			assert(!error);
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
}

namespace {
	void handleReqs(LaneHandle lane) {
		while(true) {
			auto branch = fiberAccept(lane);

			auto buffer = fiberRecv(branch);
			managarm::hwctrl::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(buffer.data(), buffer.size());
			assert(req.req_type() == managarm::hwctrl::CntReqType::CONFIGURE_IRQ);

			TriggerMode trigger;
			Polarity polarity;
			if(req.trigger_mode() == managarm::hwctrl::TriggerMode::EDGE_TRIGGERED) {
				trigger = TriggerMode::edge;
			}else{
				assert(req.trigger_mode() == managarm::hwctrl::TriggerMode::LEVEL_TRIGGERED);
				trigger = TriggerMode::level;
			}
			if(req.polarity() == managarm::hwctrl::Polarity::HIGH) {
				polarity = Polarity::high;
			}else{
				assert(req.polarity() == managarm::hwctrl::Polarity::LOW);
				polarity = Polarity::low;
			}
			
			auto pin = getGlobalSystemIrq(req.number());
			pin->configure(trigger, polarity);

			managarm::hwctrl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hwctrl::Error::SUCCESS);

			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
		}
	}

	LaneHandle createObject(LaneHandle mbus_lane) {
		auto branch = fiberOffer(mbus_lane);
		
		managarm::mbus::PropertyEntry<KernelAlloc> prop(*kernelAlloc);
		prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "what"));
		prop.set_value(frigg::String<KernelAlloc>(*kernelAlloc, "hwctrl"));
		
		managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
		req.set_parent_id(1);
		req.add_properties(prop);

		frigg::String<KernelAlloc> ser(*kernelAlloc);
		req.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());

		auto buffer = fiberRecv(branch);
		managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.ParseFromArray(buffer.data(), buffer.size());
		assert(resp.error() == managarm::mbus::Error::SUCCESS);
		
		auto descriptor = fiberPullDescriptor(branch);
		assert(descriptor.is<LaneDescriptor>());
		return descriptor.get<LaneDescriptor>().handle;
	}

	void handleBind(LaneHandle object_lane) {
		auto branch = fiberAccept(object_lane);

		auto buffer = fiberRecv(branch);
		managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(buffer.data(), buffer.size());
		assert(req.req_type() == managarm::mbus::SvrReqType::BIND);
		
		managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::mbus::Error::SUCCESS);

		frigg::String<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());

		auto stream = createStream();
		fiberPushDescriptor(branch, LaneDescriptor{stream.get<1>()});

		// TODO: Do this in an own fiber.
		handleReqs(stream.get<0>());
	}
}

void runHwctrlService() {
	KernelFiber::run([] {
		// TODO: This should not be necessary!
		disableInts();

		auto object_lane = createObject(*mbusClient);
		while(true) {
			handleBind(object_lane);
		}
	});
}

} } // namespace thor::arch_x86

