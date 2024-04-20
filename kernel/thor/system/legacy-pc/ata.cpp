#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/mbus.hpp>
#include <thor-internal/stream.hpp>
#include <hw.frigg_bragi.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

namespace thor::legacy_pc {

struct AtaBusObject : private KernelBusObject {
	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("legacy", frg::string<KernelAlloc>(*kernelAlloc, "ata"));

		// TODO(qookie): Better error handling here.
		(co_await createObject("legacy-pc/ata", std::move(properties))).unwrap();
	}

private:
	coroutine<frg::expected<Error>> handleRequest(LaneHandle boundLane) override {
		auto sendResponse = [] (LaneHandle &conversation,
				managarm::hw::SvrResponse<KernelAlloc> &&resp) -> coroutine<frg::expected<Error>> {
			frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc,
				resp.head_size};

			frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc,
				resp.size_of_tail()};

			bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

			auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};

			if (respHeadError != Error::success)
				co_return respHeadError;

			auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};

			if (respTailError != Error::success)
				co_return respTailError;

			co_return frg::success;
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

			FRG_CO_TRY(co_await sendResponse(lane, std::move(resp)));
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

			FRG_CO_TRY(co_await sendResponse(lane, std::move(resp)));

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

			auto object = smarter::allocate_shared<GenericIrqObject>(*kernelAlloc,
					frg::string<KernelAlloc>{*kernelAlloc, "isa-irq.ata"});
#ifdef __x86_64__
			auto irqOverride = resolveIsaIrq(14);
			IrqPin::attachSink(getGlobalSystemIrq(irqOverride.gsi), object.get());
#endif

			FRG_CO_TRY(co_await sendResponse(lane, std::move(resp)));

			auto irqError = co_await PushDescriptorSender{lane, IrqDescriptor{object}};
			if(irqError != Error::success)
				co_return irqError;
		}else{
			infoLogger() << "thor: Dismissing conversation due to illegal HW request." << frg::endlog;
			co_await DismissSender{lane};
			co_return Error::protocolViolation;
		}

		co_return frg::success;
	}
};

static initgraph::Task initAtaTask{&globalInitEngine, "legacy_pc.init-ata",
	initgraph::Requires{getFibersAvailableStage()},
	[] {
		// For now, we only need the kernel fiber to make sure mbusClient is already initialized.
		KernelFiber::run([=] {
			auto ata = frg::construct<AtaBusObject>(*kernelAlloc);
			async::detach_with_allocator(*kernelAlloc, ata->run());
		});
	}
};

} // namespace thor::legacy_pc
