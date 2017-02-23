
#include <frigg/initializer.hpp>
#include <mbus.frigg_pb.hpp>
#include <hwctrl.frigg_pb.hpp>
#include "../../generic/fiber.hpp"
#include "../../generic/service_helpers.hpp"
#include "hwctrl_service.hpp"
#include "pic.hpp"

namespace thor {

// TODO: Move this to a header file.
extern frigg::LazyInitializer<LaneHandle> mbusClient;

namespace arch_x86 {

namespace {
	// ------------------------------------------------------------------------
	// Protocol handling.
	// ------------------------------------------------------------------------

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

	// ------------------------------------------------------------------------
	// mbus object creation and management.
	// ------------------------------------------------------------------------

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
		KernelFiber::run([lane = stream.get<0>()] () {
			while(true)
				handleReqs(std::move(lane));
		});
	}
}

void runHwctrlService() {
	KernelFiber::run([] {
		auto object_lane = createObject(*mbusClient);
		while(true)
			handleBind(object_lane);
	});
}

} } // namespace thor::arch_x86

