#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/stream.hpp>
#include <hw.frigg_bragi.hpp>
#include <mbus.frigg_pb.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

namespace thor {
	extern frg::manual_box<LaneHandle> mbusClient;
}

namespace thor::legacy_pc {

namespace {
	// TODO: Distinguish protocol errors and end-of-lane.
	//       Print a log message on protocol errors.

	coroutine<Error> handleRequest(LaneHandle boundLane) {
		auto sendResponse = [] (LaneHandle &conversation,
				managarm::hw::SvrResponse<KernelAlloc> &&resp) -> coroutine<frg::tuple<Error, Error>> {
			frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc,
				resp.head_size};

			frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc,
				resp.size_of_tail()};

			bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

			auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};
			auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};

			co_return {respHeadError, respTailError};
		};

		auto [acceptError, lane] = co_await AcceptSender{boundLane};
		if(acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		assert(!preamble.error());

		if(preamble.id() == bragi::message_id<managarm::hw::GetPciInfoRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::GetPciInfoRequest>(reqBuffer, *kernelAlloc);

			if (!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);


			managarm::hw::PciBar<KernelAlloc> mainBar{*kernelAlloc};
			mainBar.set_io_type(managarm::hw::IoType::PORT);
			mainBar.set_address(0x1F0);
			mainBar.set_length(8);
			resp.add_bars(std::move(mainBar));

			managarm::hw::PciBar<KernelAlloc> altBar{*kernelAlloc};
			altBar.set_io_type(managarm::hw::IoType::PORT);
			altBar.set_address(0x3F6);
			altBar.set_length(1);
			resp.add_bars(std::move(altBar));

			for(size_t k = 2; k < 6; k++) {
				managarm::hw::PciBar<KernelAlloc> noBar{*kernelAlloc};
				noBar.set_io_type(managarm::hw::IoType::NO_BAR);
				resp.add_bars(std::move(noBar));
			}

			auto [headError, tailError] = co_await sendResponse(lane, std::move(resp));

			if(headError != Error::success)
				co_return headError;
			if(tailError != Error::success)
				co_return tailError;
		}else if(preamble.id() == bragi::message_id<managarm::hw::AccessBarRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::AccessBarRequest>(reqBuffer, *kernelAlloc);

			if (!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

			auto space = smarter::allocate_shared<IoSpace>(*kernelAlloc);
			if(req->index() == 0) {
				for(size_t p = 0; p < 8; ++p)
					space->addPort(0x1F0 + p);
				resp.set_error(managarm::hw::Errors::SUCCESS);
			}else if(req->index() == 1) {
				space->addPort(0x3F6);
				resp.set_error(managarm::hw::Errors::SUCCESS);
			}else{
				resp.set_error(managarm::hw::Errors::OUT_OF_BOUNDS);
			}

			auto [headError, tailError] = co_await sendResponse(lane, std::move(resp));

			if(headError != Error::success)
				co_return headError;
			if(tailError != Error::success)
				co_return tailError;

			auto ioError = co_await PushDescriptorSender{lane, IoDescriptor{space}};
			if(ioError != Error::success)
				co_return ioError;
		}else if(preamble.id() == bragi::message_id<managarm::hw::AccessIrqRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::AccessIrqRequest>(reqBuffer, *kernelAlloc);

			if (!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			auto object = smarter::allocate_shared<IrqObject>(*kernelAlloc,
					frg::string<KernelAlloc>{*kernelAlloc, "isa-irq.ata"});
#ifdef __x86_64__
			auto irqOverride = resolveIsaIrq(14);
			IrqPin::attachSink(getGlobalSystemIrq(irqOverride.gsi), object.get());
#endif

			auto [headError, tailError] = co_await sendResponse(lane, std::move(resp));

			if(headError != Error::success)
				co_return headError;
			if(tailError != Error::success)
				co_return tailError;

			auto irqError = co_await PushDescriptorSender{lane, IrqDescriptor{object}};
			if(irqError != Error::success)
				co_return irqError;
		}else{
			infoLogger() << "thor: Dismissing conversation due to illegal HW request." << frg::endlog;
			co_await DismissSender{lane};
			co_return Error::protocolViolation;
		}

		co_return Error::success;
	}

	coroutine<void> handleBind(LaneHandle objectLane) {
		auto [acceptError, lane] = co_await AcceptSender{objectLane};
		assert(acceptError == Error::success && "Unexpected mbus transaction");

		auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
		assert(reqError == Error::success && "Unexpected mbus transaction");
		managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
		assert(req.req_type() == managarm::mbus::SvrReqType::BIND
				&& "Unexpected mbus transaction");

		auto stream = createStream();
		managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::mbus::Error::SUCCESS);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		assert(respError == Error::success && "Unexpected mbus transaction");
		auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
		assert(boundError == Error::success && "Unexpected mbus transaction");

		auto boundLane = stream.get<0>();
		while(true) {
			// Terminate the connection on protocol errors.
			auto error = co_await handleRequest(boundLane);
			if(error == Error::endOfLane)
				break;
			if(isRemoteIpcError(error))
				infoLogger() << "thor: Aborting legacy-pc.ata request"
						" after remote violated the protocol" << frg::endlog;
			assert(error == Error::success);
		}
	}
}

coroutine<void> initializeAtaDevice() {
	managarm::mbus::Property<KernelAlloc> legacy_prop(*kernelAlloc);
	legacy_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "legacy"));
	auto &legacy_item = legacy_prop.mutable_item().mutable_string_item();
	legacy_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "ata"));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(legacy_prop));

	frg::string<KernelAlloc> ser{*kernelAlloc};
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto [offerError, lane] = co_await OfferSender{*mbusClient};
	assert(offerError == Error::success && "Unexpected mbus transaction");
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(reqError == Error::success && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(respError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS && "Unexpected mbus transaction");

	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(objectError == Error::success && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>() && "Unexpected mbus transaction");
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;

	while(true)
		co_await handleBind(objectLane);
}

static initgraph::Task initAtaTask{&globalInitEngine, "legacy_pc.init-ata",
	initgraph::Requires{getFibersAvailableStage()},
	[] {
		// For now, we only need the kernel fiber to make sure mbusClient is already initialized.
		KernelFiber::run([=] {
			async::detach_with_allocator(*kernelAlloc, initializeAtaDevice());
		});
	}
};

} // namespace thor::legacy_pc
