#include <algorithm>
#include <frg/algorithm.hpp>
#include <hw.frigg_bragi.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/framebuffer/boot-screen.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/stream.hpp>
#include <arch/mem_space.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

namespace thor {

// TODO: Move this to a header file.
extern frg::manual_box<LaneHandle> mbusClient;

namespace pci {

frg::manual_box<
	frg::vector<
		smarter::shared_ptr<PciDevice>,
		KernelAlloc
	>
> allDevices;

frg::manual_box<
	frg::vector<
		PciBus *,
		KernelAlloc
	>
> allRootBuses;

frg::manual_box<
	frg::hash_map<
		uint32_t,
		PciConfigIo *,
		frg::hash<uint32_t>,
		KernelAlloc
	>
> allConfigSpaces;

initgraph::Stage *getBus0AvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "pci.bus0-available"};
	return &s;
}

initgraph::Stage *getDevicesEnumeratedStage() {
	static initgraph::Stage s{&globalInitEngine, "pci.devices-enumerated"};
	return &s;
}

// --------------------------------------------------------
// PciBus implementation.
// --------------------------------------------------------

void PciBus::runRootBus() {
	KernelFiber::run([=, this] {
		async::detach_with_allocator(*kernelAlloc, [] (PciBus *device)
				-> coroutine<void> {
			Properties properties;

			properties.stringProperty("unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "pci"));
			properties.stringProperty("pci-type", frg::string<KernelAlloc>(*kernelAlloc, "pci-root-bus"));
			properties.hexStringProperty("pci-segment", device->segId, 4);
			properties.hexStringProperty("pci-bus", device->busId, 2);

			auto ret = co_await device->createObject("pci-root-bus", std::move(properties));
			assert(ret);
			device->mbusId = ret.value();
			device->mbusPublished.raise();
		}(this));
	});
}

coroutine<frg::expected<Error>> PciBus::handleRequest(LaneHandle lane) {
	// TODO(qookie): Implement this properly :^)
	auto dismissError = co_await DismissSender{lane};
	if (dismissError != Error::success)
		co_return dismissError;

	co_return frg::success;
}

// --------------------------------------------------------
// PciBridge implementation.
// --------------------------------------------------------

void PciBridge::runBridge() {
	KernelFiber::run([=, this] {
		async::detach_with_allocator(*kernelAlloc, [] (PciBridge *device)
				-> coroutine<void> {
			Properties properties;

			properties.stringProperty("unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "pci"));
			properties.stringProperty("pci-type", frg::string<KernelAlloc>(*kernelAlloc, "pci-bridge"));
			properties.hexStringProperty("pci-segment", device->seg, 4);
			properties.hexStringProperty("pci-bus", device->bus, 2);
			properties.hexStringProperty("pci-slot", device->slot, 2);
			properties.hexStringProperty("pci-function", device->function, 1);
			properties.hexStringProperty("pci-vendor", device->vendor, 4);
			properties.hexStringProperty("pci-device", device->deviceId, 4);
			properties.hexStringProperty("pci-revision", device->revision, 2);
			properties.hexStringProperty("pci-class", device->classCode, 2);
			properties.hexStringProperty("pci-subclass", device->subClass, 2);
			properties.hexStringProperty("pci-interface", device->interface, 2);

			if(device->parentBus->associatedBridge) {
				co_await device->parentBus->associatedBridge->mbusPublished.wait();
				properties.decStringProperty("drvcore.mbus-parent", device->parentBus->associatedBridge->mbusId, 1);
			} else {
				co_await device->parentBus->mbusPublished.wait();
				properties.decStringProperty("drvcore.mbus-parent", device->parentBus->mbusId, 1);
			}

			auto ret = co_await device->createObject("pci-bridge", std::move(properties));
			assert(ret);
			device->mbusId = ret.value();
			device->mbusPublished.raise();
		}(this));
	});
}

// --------------------------------------------------------
// PciDevice implementation.
// --------------------------------------------------------

void PciDevice::runDevice() {
	KernelFiber::run([=, this] {
		async::detach_with_allocator(*kernelAlloc, [] (PciDevice *device)
				-> coroutine<void> {
			Properties properties;

			properties.stringProperty("unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "pci"));
			properties.stringProperty("pci-type", frg::string<KernelAlloc>(*kernelAlloc, "pci-device"));
			properties.hexStringProperty("pci-segment", device->seg, 4);
			properties.hexStringProperty("pci-bus", device->bus, 2);
			properties.hexStringProperty("pci-slot", device->slot, 2);
			properties.hexStringProperty("pci-function", device->function, 1);
			properties.hexStringProperty("pci-vendor", device->vendor, 4);
			properties.hexStringProperty("pci-device", device->deviceId, 4);
			properties.hexStringProperty("pci-revision", device->revision, 2);
			properties.hexStringProperty("pci-class", device->classCode, 2);
			properties.hexStringProperty("pci-subclass", device->subClass, 2);
			properties.hexStringProperty("pci-interface", device->interface, 2);
			properties.hexStringProperty("pci-subsystem-vendor", device->subsystemVendor, 2);
			properties.hexStringProperty("pci-subsystem-device", device->subsystemDevice, 2);

			if(device->parentBus->associatedBridge) {
				co_await device->parentBus->associatedBridge->mbusPublished.wait();
				properties.decStringProperty("drvcore.mbus-parent", device->parentBus->associatedBridge->mbusId, 1);
			} else {
				co_await device->parentBus->mbusPublished.wait();
				properties.decStringProperty("drvcore.mbus-parent", device->parentBus->mbusId, 1);
			}

			if(device->associatedFrameBuffer) {
				properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "framebuffer"));
			}

			// TODO(qookie): Better error handling here.
			auto ret = co_await device->createObject("pci-device", std::move(properties));
			assert(ret);
			device->mbusId = ret.value();
		}(this));
	});
}

// --------------------------------------------------------
// PciEntity implementation.
// --------------------------------------------------------

coroutine<frg::expected<Error>> PciEntity::handleRequest(LaneHandle lane) {
	auto [acceptError, conversation] = co_await AcceptSender{lane};
	if(acceptError != Error::success)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
	if(reqError != Error::success)
		co_return reqError;

	auto preamble = bragi::read_preamble(reqBuffer);
	if (preamble.error())
		co_return Error::protocolViolation;

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

	if(preamble.id() == bragi::message_id<managarm::hw::GetPciInfoRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::GetPciInfoRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		if (parentBus->msiController)
			resp.set_num_msis(numMsis);

		for(size_t i = 0; i < caps.size(); i++) {
			managarm::hw::PciCapability<KernelAlloc> msg(*kernelAlloc);
			msg.set_type(caps[i].type);
			msg.set_offset(caps[i].offset);
			msg.set_length(caps[i].length);
			resp.add_capabilities(std::move(msg));
		}

		auto bars = getBars();

		for(size_t k = 0; k < bars.size(); k++) {
			managarm::hw::PciBar<KernelAlloc> msg(*kernelAlloc);
			if(bars[k].type == PciBar::kBarIo) {
				msg.set_io_type(managarm::hw::IoType::PORT);
			}else if(bars[k].type == PciBar::kBarMemory) {
				msg.set_io_type(managarm::hw::IoType::MEMORY);
			}else{
				assert(bars[k].type == PciBar::kBarNone);
				msg.set_io_type(managarm::hw::IoType::NO_BAR);
			}

			if(bars[k].hostType == PciBar::kBarIo) {
				msg.set_host_type(managarm::hw::IoType::PORT);
				msg.set_address(bars[k].address);
				msg.set_length(bars[k].length);
			}else if(bars[k].hostType == PciBar::kBarMemory) {
				msg.set_host_type(managarm::hw::IoType::MEMORY);
				msg.set_address(bars[k].address);
				msg.set_length(bars[k].length);
				msg.set_offset(bars[k].offset);
			}else{
				assert(bars[k].hostType == PciBar::kBarNone);
				msg.set_host_type(managarm::hw::IoType::NO_BAR);
			}
			resp.add_bars(std::move(msg));
		}

		managarm::hw::PciExpansionRom<KernelAlloc> msg(*kernelAlloc);
		msg.set_address(expansionRom.address);
		msg.set_length(expansionRom.length);
		resp.set_expansion_rom(msg);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::AccessBarRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::AccessBarRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		auto index = req->index();
		auto bars = getBars();

		if(index < 0 || std::size_t(index) >= bars.size()) {
			infoLogger() << "thor: Closing lane due to out-ouf-bounds BAR " << index << " in HW request." << frg::endlog;
			co_return Error::illegalArgs;
		}

		AnyDescriptor descriptor;
		if(bars[index].type == PciBar::kBarIo) {
			descriptor = IoDescriptor{bars[index].io};
		}else{
			assert(bars[index].type == PciBar::kBarMemory);
			descriptor = MemoryViewDescriptor{bars[index].memory};
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

		auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};

		if (descError != Error::success)
			co_return descError;
	} else if(preamble.id() == bragi::message_id<managarm::hw::AccessExpansionRomRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::AccessExpansionRomRequest>(reqBuffer, *kernelAlloc);

		if(!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		assert(expansionRom.address);
		AnyDescriptor descriptor = MemoryViewDescriptor{expansionRom.memory};

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

		auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};

		if (descError != Error::success)
			co_return descError;
	}else if(preamble.id() == bragi::message_id<managarm::hw::AccessIrqRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::AccessIrqRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		if(type() != PciEntityType::Device) {
			infoLogger() << "thor: Unsupported operation on PCI entity." << frg::endlog;
			co_return Error::protocolViolation;
		}


		auto object = static_cast<PciDevice *>(this)->obtainIrqObject();

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

		auto descError = co_await PushDescriptorSender{conversation, IrqDescriptor{object}};

		if (descError != Error::success)
			co_return descError;
	}else if(preamble.id() == bragi::message_id<managarm::hw::InstallMsiRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::InstallMsiRequest>(
				reqBuffer, *kernelAlloc);
		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		if(type() != PciEntityType::Device) {
			infoLogger() << "thor: Unsupported operation on PCI entity." << frg::endlog;
			co_return Error::protocolViolation;
		}


		if ((msiIndex < 0 && msixIndex < 0)
				|| !parentBus->msiController
				|| req->index() >= numMsis) {
			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
			co_return frg::success;
		}

		// Allocate the MSI.
		auto interrupt = parentBus->msiController->allocateMsiPin(
				frg::string<KernelAlloc>{*kernelAlloc, "pci-msi."}
				+ frg::to_allocated_string(*kernelAlloc, bus)
				+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
				+ frg::to_allocated_string(*kernelAlloc, slot)
				+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
				+ frg::to_allocated_string(*kernelAlloc, function)
				+ frg::string<KernelAlloc>{*kernelAlloc, "."}
				+ frg::to_allocated_string(*kernelAlloc, req->index()));
		if(!interrupt) {
			infoLogger() << "thor: Could not allocate interrupt vector for MSI" << frg::endlog;

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::RESOURCE_EXHAUSTION);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
			co_return frg::success;
		}

		// Obtain an IRQ object for the interrupt.
		auto object = smarter::allocate_shared<GenericIrqObject>(*kernelAlloc,
				frg::string<KernelAlloc>{*kernelAlloc, "pci-msi."}
				+ frg::to_allocated_string(*kernelAlloc, bus)
				+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
				+ frg::to_allocated_string(*kernelAlloc, slot)
				+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
				+ frg::to_allocated_string(*kernelAlloc, function)
				+ frg::string<KernelAlloc>{*kernelAlloc, "."}
				+ frg::to_allocated_string(*kernelAlloc, req->index()));
		IrqPin::attachSink(interrupt, object.get());

		static_cast<PciDevice *>(this)->setupMsi(interrupt, req->index());

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

		auto descError = co_await PushDescriptorSender{conversation, IrqDescriptor{object}};

		if (descError != Error::success)
			co_return descError;
	}else if(preamble.id() == bragi::message_id<managarm::hw::ClaimDeviceRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::ClaimDeviceRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		if(type() != PciEntityType::Device) {
			infoLogger() << "thor: Unsupported operation on PCI entity." << frg::endlog;
			co_return Error::protocolViolation;
		}

		if(static_cast<PciDevice *>(this)->associatedScreen) {
			infoLogger() << "thor: Disabling screen associated with PCI device "
					<< bus << "." << slot << "." << function
					<< frg::endlog;
			disableLogHandler(static_cast<PciDevice *>(this)->associatedScreen);
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::EnableBusIrqRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::EnableBusIrqRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		if(type() != PciEntityType::Device) {
			infoLogger() << "thor: Unsupported operation on PCI entity." << frg::endlog;
			co_return Error::protocolViolation;
		}

		static_cast<PciDevice *>(this)->enableIrq();

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::EnableMsiRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::EnableMsiRequest>(
				reqBuffer, *kernelAlloc);
		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		if(type() != PciEntityType::Device) {
			infoLogger() << "thor: Unsupported operation on PCI entity." << frg::endlog;
			co_return Error::protocolViolation;
		}

		if ((msiIndex < 0 && msixIndex < 0)
				|| !parentBus->msiController) {
			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
			co_return frg::success;
		}

		static_cast<PciDevice *>(this)->enableMsi();

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::EnableBusmasterRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::EnableBusmasterRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		auto io = parentBus->io;

		auto command = io->readConfigHalf(parentBus,
				slot, function, kPciCommand);
		io->writeConfigHalf(parentBus, slot, function,
				kPciCommand, command | 0x0004);

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::LoadPciSpaceRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::LoadPciSpaceRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

		auto io = parentBus->io;

		if(req->size() == 1) {
			if(isValidConfigAccess(1, req->offset())) {
				auto word = io->readConfigByte(parentBus,
						slot, function, req->offset());
				resp.set_error(managarm::hw::Errors::SUCCESS);
				resp.set_word(word);
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}
		}else if(req->size() == 2) {
			if(isValidConfigAccess(2, req->offset())) {
				auto word = io->readConfigHalf(parentBus,
						slot, function, req->offset());
				resp.set_error(managarm::hw::Errors::SUCCESS);
				resp.set_word(word);
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}
		}else if(req->size() == 4) {
			if(isValidConfigAccess(4, req->offset())) {
				auto word = io->readConfigWord(parentBus,
						slot, function, req->offset());
				resp.set_error(managarm::hw::Errors::SUCCESS);
				resp.set_word(word);
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}
		}else{
			resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
		}

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::StorePciSpaceRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::StorePciSpaceRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

		auto io = parentBus->io;

		if(req->size() == 1) {
			if(isValidConfigAccess(1, req->offset())) {
				io->writeConfigByte(parentBus, slot, function,
						req->offset(), req->word());
				resp.set_error(managarm::hw::Errors::SUCCESS);
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}
		}else if(req->size() == 2) {
			if(isValidConfigAccess(2, req->offset())) {
				io->writeConfigHalf(parentBus, slot, function,
						req->offset(), req->word());
				resp.set_error(managarm::hw::Errors::SUCCESS);
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}
		}else if(req->size() == 4) {
			if(isValidConfigAccess(4, req->offset())) {
				io->writeConfigWord(parentBus, slot, function,
						req->offset(), req->word());
				resp.set_error(managarm::hw::Errors::SUCCESS);
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}
		}else{
			resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
		}

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::LoadPciCapabilityRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::LoadPciCapabilityRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

		auto io = parentBus->io;

		if(static_cast<size_t>(req->index()) < caps.size()) {
			if(req->size() == 1) {
				if(isValidConfigAccess(1, req->offset())) {
					auto word = io->readConfigByte(parentBus,
							slot, function,
							caps[req->index()].offset + req->offset());
					resp.set_error(managarm::hw::Errors::SUCCESS);
					resp.set_word(word);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else if(req->size() == 2) {
				if(isValidConfigAccess(2, req->offset())) {
					auto word = io->readConfigHalf(parentBus,
							slot, function,
							caps[req->index()].offset + req->offset());
					resp.set_error(managarm::hw::Errors::SUCCESS);
					resp.set_word(word);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else if(req->size() == 4) {
				if(isValidConfigAccess(4, req->offset())) {
					auto word = io->readConfigWord(parentBus,
							slot, function,
							caps[req->index()].offset + req->offset());
					resp.set_error(managarm::hw::Errors::SUCCESS);
					resp.set_word(word);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}
		}else{
			resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
		}

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::GetFbInfoRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::GetFbInfoRequest>(reqBuffer, *kernelAlloc);

		if (!req || type() != PciEntityType::Device) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		auto fb = static_cast<PciDevice *>(this)->associatedFrameBuffer;

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

		if (!fb) {
			resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
		} else {
			resp.set_error(managarm::hw::Errors::SUCCESS);

			resp.set_fb_pitch(fb->pitch);
			resp.set_fb_width(fb->width);
			resp.set_fb_height(fb->height);
			resp.set_fb_bpp(fb->bpp);
			resp.set_fb_type(fb->type);
		}

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
	}else if(preamble.id() == bragi::message_id<managarm::hw::AccessFbMemoryRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::AccessFbMemoryRequest>(reqBuffer, *kernelAlloc);

		if (!req || type() != PciEntityType::Device) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		auto fb = static_cast<PciDevice *>(this)->associatedFrameBuffer;
		MemoryViewDescriptor descriptor{nullptr};

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

		if (!fb) {
			resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
		} else {
			descriptor = MemoryViewDescriptor{fb->memory};
			resp.set_error(managarm::hw::Errors::SUCCESS);
		}

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

		auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};
		// TODO: improve error handling here.
		assert(descError == Error::success);
	}else{
		infoLogger() << "thor: Dismissing conversation due to illegal HW request." << frg::endlog;
		co_await DismissSender{conversation};
	}

	co_return frg::success;
}

namespace {
	struct PciIrqObject final : IrqObject {
		PciIrqObject(PciDevice *pciDevice, frg::string<KernelAlloc> name)
		: IrqObject{name}, pciDevice_{pciDevice} { }

		void dumpHardwareState() override {
			auto io = pciDevice_->parentBus->io;
			auto status = io->readConfigHalf(pciDevice_->parentBus,
					pciDevice_->slot, pciDevice_->function, kPciStatus);
			infoLogger() << "thor: PCI IRQ " << name() << " is "
					<< ((status & 0x08) ? "asserted" : "inactive") << frg::endlog;
		}

	private:
		PciDevice *pciDevice_;
	};
}

smarter::shared_ptr<IrqObject> PciDevice::obtainIrqObject() {
	assert(interrupt);
	auto object = smarter::allocate_shared<PciIrqObject>(*kernelAlloc, this,
			frg::string<KernelAlloc>{*kernelAlloc, "pci-irq."}
			+ frg::to_allocated_string(*kernelAlloc, bus)
			+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
			+ frg::to_allocated_string(*kernelAlloc, slot)
			+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
			+ frg::to_allocated_string(*kernelAlloc, function));
	IrqPin::attachSink(interrupt, object.get());
	return object;
}

IrqPin *PciDevice::getIrqPin() {
	return interrupt;
}

void PciDevice::enableIrq() {
	auto io = parentBus->io;

	auto command = io->readConfigHalf(parentBus, slot, function, kPciCommand);
	io->writeConfigHalf(parentBus, slot, function,
			kPciCommand, command & ~uint16_t{0x400});
}

void PciDevice::setupMsi(MsiPin *msi, size_t index) {
	auto io = parentBus->io;

	if (msixIndex >= 0) {
		// Setup the MSI-X table.
		auto space = arch::mem_space{msixMapping}.subspace(index * 16);
		space.store(msixMessageAddress, msi->getMessageAddress());
		space.store(msixMessageData, msi->getMessageData());
		space.store(msixVectorControl,
				space.load(msixVectorControl) & ~uint32_t{1});
	} else {
		assert(msiIndex >= 0);

		// TODO(qookie): support non-zero indices
		assert(!index);
		auto offset = caps[msiIndex].offset;

		auto msgControl = io->readConfigHalf(parentBus,
				slot, function, offset + 2);

		bool is64Capable = msgControl & (1 << 7);

		io->writeConfigWord(parentBus,
				slot, function, offset + 4, msi->getMessageAddress() & 0xFFFFFFFF);

		if (is64Capable) {
			io->writeConfigWord(parentBus,
				slot, function, offset + 8, msi->getMessageAddress() >> 32);

			io->writeConfigHalf(parentBus,
				slot, function, offset + 12, msi->getMessageData());
		} else {
			assert(!(msi->getMessageAddress() >> 32));

			io->writeConfigHalf(parentBus,
				slot, function, offset + 8, msi->getMessageData());
		}

		if (msiEnabled) {
			// Enable MSI
			msgControl |= 0x0001;

			io->writeConfigHalf(parentBus,
					slot, function, offset + 2, msgControl);
		}

		msiInstalled = true;
	}
}

void PciDevice::enableMsi() {
	auto io = parentBus->io;

	enableIrq();

	if (msixIndex >= 0) {
		auto offset = caps[msixIndex].offset;

		auto msgControl = io->readConfigHalf(parentBus,
				slot, function, offset + 2);

		msgControl |= 0x8000; // Enable MSI-X.

		msgControl &= ~uint16_t{0x4000}; // Disable the overall mask.
		io->writeConfigHalf(parentBus,
				slot, function, offset + 2, msgControl);

	} else {
		assert(msiIndex >= 0);

		auto offset = caps[msiIndex].offset;

		auto msgControl = io->readConfigHalf(parentBus,
				slot, function, offset + 2);

		if (!msiInstalled) {
			// Disable MSI by default, configure to only 1 message
			// MSIs will be reenabled once one is installed in setupMsi, since
			// we may not have a way to mask it otherwise (mask register only
			// exists if MSIs are 64-bit).
			msgControl &= ~0x0071;
		} else {
			// setupMsi was called before enableMsi, so we can enable them
			// without worrying about needing the MSI to be masked.
			msgControl &= ~0x0070; // Only one message
			msgControl |= 0x0001; // Enable MSI
		}

		io->writeConfigHalf(parentBus,
				slot, function, offset + 2, msgControl);

		msiEnabled = true;
	}
}

// --------------------------------------------------------
// Discovery functionality
// --------------------------------------------------------

size_t computeBarLength(uintptr_t mask) {
	static_assert(sizeof(long) == 8, "Fix builtin usage");
	static_assert(sizeof(uintptr_t) == 8, "Fix builtin usage");

	assert(mask);
	size_t length_bits = __builtin_ctzl(mask);
	// TODO: Bits decoded by the PCI device.
	// size_t decoded_bits = 64 - __builtin_clzl(mask);
	// FIXME: assert(__builtin_popcountl(mask) == decoded_bits - length_bits);

	return size_t(1) << length_bits;
}

frg::manual_box<frg::vector<PciBus *, KernelAlloc>> enumerationQueue;

void readEntityBars(PciEntity *entity, int nBars) {
	auto bars = entity->getBars();
	auto bus = entity->parentBus;
	auto io = bus->io;

	auto slot = entity->slot;
	auto function = entity->function;

	// Determine the BARs
	for(int i = 0; i < nBars; i++) {
		uint32_t offset = kPciRegularBar0 + i * 4;
		uint32_t bar = io->readConfigWord(bus, slot, function, offset);

		if((bar & 1) != 0) {
			uintptr_t address = bar & 0xFFFFFFFC;

			// disable I/O and memory decode while reading BARs
			auto command = io->readConfigHalf(bus, slot, function, kPciCommand);
			if(command & 0x03)
				io->writeConfigHalf(bus, slot, function, kPciCommand, command & ~(0x03));

			// write all 1s to the BAR and read it back to determine this its length.
			io->writeConfigWord(bus, slot, function, offset, 0xFFFFFFFF);
			uint32_t mask = io->readConfigWord(bus, slot, function, offset) & 0xFFFFFFFC;
			io->writeConfigWord(bus, slot, function, offset, bar);

			// restore the original command byte value
			if(command & 0x03)
				io->writeConfigHalf(bus, slot, function, kPciCommand, command);

			// Device doesn't decode any address bits from this BAR
			if (!mask)
				continue;

			auto length = computeBarLength(mask);

			bars[i].type = PciBar::kBarIo;
			bars[i].address = address;
			bars[i].length = length;

			if (!address) {
				infoLogger() << "            unallocated I/O space BAR #" << i
						<< ", length: " << length << " ports" << frg::endlog;
			} else {
				// Check all parent resources to see if this BAR is actually memory mapped
				bool isMemoryMapped = false;
				PciBusResource *resource = nullptr;

				for (auto &res : entity->parentBus->resources) {
					if (res.flags() == PciBusResource::io
							&& address >= res.base()
							&& (address + length) <= (res.base() + res.size())) {
						resource = &res;
						isMemoryMapped = res.isHostMmio();
						break;
					}
				}

				if (isMemoryMapped) {
					uintptr_t hostAddress = resource->hostBase()
						+ (address - resource->base());

					auto offset = hostAddress & (kPageSize - 1);

					bars[i].hostType = PciBar::kBarMemory;
					bars[i].allocated = true;
					bars[i].offset = offset;
					bars[i].memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
							hostAddress & ~(kPageSize - 1),
							(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
							CachingMode::mmioNonPosted);
				} else {
					bars[i].hostType = PciBar::kBarIo;
					bars[i].allocated = true;
					bars[i].io = smarter::allocate_shared<IoSpace>(*kernelAlloc);
					for(size_t p = 0; p < length; ++p)
						bars[i].io->addPort(address + p);
					bars[i].offset = 0;
				}

				infoLogger() << "            I/O space BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " ports" << frg::endlog;

			}
		}else if(((bar >> 1) & 3) == 0) {
			uint32_t address = bar & 0xFFFFFFF0;

			// disable I/O and memory decode while reading BARs
			auto command = io->readConfigHalf(bus, slot, function, kPciCommand);
			if(command & 0x03)
				io->writeConfigHalf(bus, slot, function, kPciCommand, command & ~(0x03));

			// Write all 1s to the BAR and read it back to determine this its length.
			io->writeConfigWord(bus, slot, function, offset, 0xFFFFFFFF);
			uint32_t mask = io->readConfigWord(bus, slot, function, offset) & 0xFFFFFFF0;
			io->writeConfigWord(bus, slot, function, offset, bar);

			// restore the original command byte value
			if(command & 0x03)
				io->writeConfigHalf(bus, slot, function, kPciCommand, command);

			// Device doesn't decode any address bits from this BAR
			if (!mask)
				continue;

			auto length = computeBarLength(mask);

			bars[i].type = PciBar::kBarMemory;
			bars[i].address = address;
			bars[i].length = length;
			bars[i].prefetchable = bar & (1 << 3);

			if (!address) {
				infoLogger() << "            unallocated 32-bit memory BAR #" << i
						<< ", length: " << length << " bytes"
						<< (bar & (1 << 3) ? " (prefetchable)" : "")
						<< frg::endlog;
			} else {
				bars[i].hostType = PciBar::kBarMemory;
				bars[i].allocated = true;
				auto offset = address & (kPageSize - 1);
				bars[i].memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::mmio);
				bars[i].offset = offset;

				infoLogger() << "            32-bit memory BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " bytes"
						<< (bar & (1 << 3) ? " (prefetchable)" : "")
						<< frg::endlog;
			}
		}else if(((bar >> 1) & 3) == 2) {
			assert(i < (nBars - 1)); // Otherwise there is no next bar.
			auto high = io->readConfigWord(bus, slot, function, offset + 4);;
			auto address = (uint64_t{high} << 32) | (bar & 0xFFFFFFF0);

			// disable I/O and memory decode while reading BARs
			auto command = io->readConfigHalf(bus, slot, function, kPciCommand);
			if(command & 0x03)
				io->writeConfigHalf(bus, slot, function, kPciCommand, command & ~(0x03));

			// Write all 1s to the BAR and read it back to determine this its length.
			io->writeConfigWord(bus, slot, function, offset, 0xFFFFFFFF);
			io->writeConfigWord(bus, slot, function, offset + 4, 0xFFFFFFFF);
			uint32_t mask = (uint64_t{io->readConfigWord(bus, slot, function, offset + 4)} << 32)
					| (io->readConfigWord(bus, slot, function, offset) & 0xFFFFFFF0);
			io->writeConfigWord(bus, slot, function, offset, bar);
			io->writeConfigWord(bus, slot, function, offset + 4, high);

			// restore the original command byte value
			if(command & 0x03)
				io->writeConfigHalf(bus, slot, function, kPciCommand, command);

			// Device doesn't decode any address bits from this BAR
			if (!mask) {
				i++;
				continue;
			}

			auto length = computeBarLength(mask);

			bars[i].type = PciBar::kBarMemory;
			bars[i].address = address;
			bars[i].length = length;
			bars[i].prefetchable = bar & (1 << 3);

			if (!address) {
				infoLogger() << "            unallocated 64-bit memory BAR #" << i
						<< ", length: " << length << " bytes"
						<< (bar & (1 << 3) ? " (prefetchable)" : "")
						<< frg::endlog;
			} else {
				bars[i].hostType = PciBar::kBarMemory;
				bars[i].allocated = true;
				auto offset = address & (kPageSize - 1);
				bars[i].memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::mmio);
				bars[i].offset = offset;

				infoLogger() << "            64-bit memory BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " bytes"
						<< (bar & (1 << 3) ? " (prefetchable)" : "")
						<< frg::endlog;
			}

			i++;
		}else{
			assert(!"Unexpected BAR type");
		}
	}
}

void findPciCaps(PciEntity *entity) {
	auto io = entity->parentBus->io;

	auto status = io->readConfigByte(entity->parentBus, entity->slot, entity->function, kPciStatus);

	// Find all capabilities.
	if(status & 0x10) {
		// The bottom two bits of each capability offset must be masked!
		uint8_t offset = io->readConfigHalf(entity->parentBus, entity->slot, entity->function, kPciRegularCapabilities) & 0xFC;
		while(offset) {
			auto ent = io->readConfigHalf(entity->parentBus, entity->slot, entity->function, offset);
			uint8_t type = ent & 0xFF;

			auto name = nameOfCapability(type);
			if(name) {
				infoLogger() << "            " << name << " capability"
						<< frg::endlog;
			}else{
				infoLogger() << "            Capability of type 0x"
						<< frg::hex_fmt((int)type) << frg::endlog;
			}

			if(type == 0x10) {
				entity->isPcie = true;

				auto flags = io->readConfigHalf(entity->parentBus, entity->slot, entity->function, offset + 2);
				auto type = (flags >> 4) & 0xF;
				entity->isDownstreamPort =
					type == 4 // Root port
					|| type == 6 // Downstream
					|| type == 8; // PCI/-X to PCIe bridge
			}

			// TODO:
			size_t size = -1;
			if(type == 0x09)
				size = io->readConfigHalf(entity->parentBus, entity->slot, entity->function, offset + 2);

			entity->caps.push({type, offset, size});

			offset = (ent >> 8) & 0xFC;
		}
	}
}

template <typename EnumFunc>
void checkPciFunction(PciBus *bus, uint32_t slot, uint32_t function,
		EnumFunc &&enumerateDownstream) {
	auto io = bus->io;

	auto parseExpansionRom = [&io, bus, slot, function](PciEntity *device, uint32_t offset) {
		uint32_t expansion_rom_addr = io->readConfigWord(bus, slot, function, offset);
		// write all 1s to the expansion rom addr and read it back to determine this its length.
		io->writeConfigWord(bus, slot, function, offset, 0xFFFFFFFF);

		uint32_t expansion_rom_mask = io->readConfigWord(bus, slot, function, offset) & 0xFFFFFFFC;
		io->writeConfigWord(bus, slot, function, offset, expansion_rom_addr);

		if(expansion_rom_mask) {
			auto expansion_rom_length = computeBarLength(expansion_rom_mask);
			// Enable it
			io->writeConfigWord(bus, slot, function, offset, expansion_rom_addr | 1);
			// Map it
			auto offset = expansion_rom_addr & (kPageSize - 1);
			device->expansionRom.memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
						expansion_rom_addr & ~(kPageSize - 1),
						(expansion_rom_length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::uncached); // Some cards have problems with caching the PCI Expansion Rom
			device->expansionRom.offset = offset;
			device->expansionRom.address = expansion_rom_addr;
			device->expansionRom.length = expansion_rom_length;
		} else {
			device->expansionRom.address = 0;
		}
	};

	uint16_t vendor = io->readConfigHalf(bus, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;

	auto log = infoLogger();

	uint8_t header_type = io->readConfigByte(bus, slot, function, kPciHeaderType);
	if ((header_type & 0x7F) == 0) {
		log << "        Function " << function << ": Device";
	} else if ((header_type & 0x7F) == 1) {
		uint8_t downstreamId = io->readConfigByte(bus, slot, function, kPciBridgeSecondary);

		if (!downstreamId) {
			log << "        Function " << function
					<< ": unconfigured PCI-to-PCI bridge";
		} else {
			log << "        Function " << function
					<< ": PCI-to-PCI bridge to bus " << (int)downstreamId;
		}
	} else {
		log << "        Function " << function
				<< ": Unexpected PCI header type " << (header_type & 0x7F);
	}

	auto command = io->readConfigHalf(bus, slot, function, kPciCommand);
	if(command & 0x01)
		log << " (Decodes IO)";
	if(command & 0x02)
		log << " (Decodes Memory)";
	if(command & 0x04)
		log << " (Busmaster)";
	if(command & 0x400)
		log << " (IRQs masked)";
	log << frg::endlog;
	io->writeConfigHalf(bus, slot, function, kPciCommand, command | 0x400);

	auto device_id = io->readConfigHalf(bus, slot, function, kPciDevice);
	auto revision = io->readConfigByte(bus, slot, function, kPciRevision);
	auto class_code = io->readConfigByte(bus, slot, function, kPciClassCode);
	auto sub_class = io->readConfigByte(bus, slot, function, kPciSubClass);
	auto interface = io->readConfigByte(bus, slot, function, kPciInterface);

	infoLogger() << "            Vendor/device: " << frg::hex_fmt(vendor)
			<< "." << frg::hex_fmt(device_id) << "." << frg::hex_fmt(revision)
			<< ", class: " << frg::hex_fmt(class_code)
			<< "." << frg::hex_fmt(sub_class)
			<< "." << frg::hex_fmt(interface) << frg::endlog;

	if ((header_type & 0x7F) == 0) {
		uint16_t subsystem_vendor = io->readConfigHalf(bus, slot, function, kPciRegularSubsystemVendor);
		uint16_t subsystem_device = io->readConfigHalf(bus, slot, function, kPciRegularSubsystemDevice);
//		infoLogger() << "        Subsystem vendor: 0x" << frg::hex_fmt(subsystem_vendor)
//				<< ", device: 0x" << frg::hex_fmt(subsystem_device) << frg::endlog;

		auto status = io->readConfigHalf(bus, slot, function, kPciStatus);

		if(status & 0x08)
			infoLogger() << "\e[35m                IRQ is asserted!\e[39m" << frg::endlog;

		auto device = smarter::allocate_shared<PciDevice>(*kernelAlloc, bus, bus->segId, bus->busId, slot, function,
				vendor, device_id, revision, class_code, sub_class, interface, subsystem_vendor, subsystem_device);

		findPciCaps(device.get());

		for (size_t i = 0; i < device->caps.size(); i++) {
			if (device->caps[i].type == 0x5)
				device->msiIndex = i;
			if (device->caps[i].type == 0x11)
				device->msixIndex = i;
		}

		readEntityBars(device.get(), 6);
		parseExpansionRom(device.get(), kPciRegularExpansionRomBaseAddress);

		auto irq_index = static_cast<IrqIndex>(io->readConfigByte(bus, slot, function,
				kPciRegularInterruptPin));
		if(irq_index != IrqIndex::null) {
			assert(bus->irqRouter);
			auto irq_pin = bus->irqRouter->resolveIrqRoute(slot, irq_index);
			if(irq_pin) {
				infoLogger() << "            Interrupt: "
						<< nameOf(irq_index)
						<< " (routed to " << irq_pin->name() << ")" << frg::endlog;
				device->interrupt = irq_pin;
			}else{
				infoLogger() << "\e[31m" "            Interrupt routing not available!"
					"\e[39m" << frg::endlog;
			}
		}

		// Setup MSI-X.
		if(device->msixIndex >= 0) {
			auto offset = device->caps[device->msixIndex].offset;

			auto msgControl = io->readConfigHalf(bus, slot, function, offset + 2);
			device->numMsis = (msgControl & 0x7F) + 1;
			infoLogger() << "            " << device->numMsis
					<< " MSI-X vectors available" << frg::endlog;

			// Map the MSI-X BAR.
			auto tableInfo = io->readConfigWord(bus, slot, function, offset + 4);
			auto tableBar = tableInfo & 0x7;
			auto tableOffset = tableInfo & 0xFFFF'FFF8;
			assert(tableBar < 6);

			auto bar = device->bars[tableBar];
			assert(bar.type == PciBar::kBarMemory);
			auto mappingDisp = (bar.address + tableOffset) & (kPageSize - 1);
			auto mappingSize = (mappingDisp + device->numMsis * 16 + kPageSize - 1)
					& ~(kPageSize - 1);

			auto window = KernelVirtualMemory::global().allocate(0x10000);
			for(uintptr_t page = 0; page < mappingSize; page += kPageSize)
				KernelPageSpace::global().mapSingle4k(
						reinterpret_cast<uintptr_t>(window) + page,
						(bar.address + tableOffset + page) & ~(kPageSize - 1),
						page_access::write, CachingMode::null);
			device->msixMapping = reinterpret_cast<std::byte *>(window) + mappingDisp;

			// Mask all MSIs.
			for(unsigned int i = 0; i < device->numMsis; ++i) {
				auto space = arch::mem_space{device->msixMapping}.subspace(i * 16);
				space.store(msixVectorControl,
						space.load(msixVectorControl) | 1);
			}
		} else if (device->msiIndex >= 0) {
			auto offset = device->caps[device->msiIndex].offset;

			auto msgControl = io->readConfigHalf(bus, slot, function, offset + 2);
			device->numMsis = 1; // TODO(qookie): 1 << ((msgControl >> 1) & 0b111)
			infoLogger() << "            " << device->numMsis
					<< " MSI vectors available" << frg::endlog;

			msgControl &= ~0x0001; // Disable MSI

			io->writeConfigHalf(bus, slot, function, offset + 2, msgControl);
		}

		allDevices->push_back(device);
		bus->childDevices.push_back(device.get());

		applyPciDeviceQuirks(device);
	} else if ((header_type & 0x7F) == 1) {
		auto bridge = frg::construct<PciBridge>(*kernelAlloc, bus, bus->segId, bus->busId, slot, function,
				vendor, device_id, revision, class_code, sub_class, interface);
		bus->childBridges.push_back(bridge);

		findPciCaps(bridge);

		readEntityBars(bridge, 2);
		parseExpansionRom(bridge, kPciBridgeExpansionRomBaseAddress);

		uint8_t downstreamId = io->readConfigByte(bus, slot, function, kPciBridgeSecondary);

		if (downstreamId) {
			bridge->downstreamId = downstreamId;
			bridge->subordinateId = io->readConfigByte(bus, slot, function, kPciBridgeSubordinate);

			auto downstreamBus = bus->makeDownstreamBus(bridge, downstreamId);
			bridge->associatedBus = downstreamBus;
			enumerateDownstream(downstreamBus);
		} else {
			infoLogger() << "            Deferring enumeration until bridge is configured" << frg::endlog;
		}
	}
}

template <typename EnumFunc>
void checkPciDevice(PciBus *bus, uint32_t slot, EnumFunc &&enumerateDownstream) {
	auto io = bus->io;

	uint16_t vendor = io->readConfigHalf(bus, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;

	infoLogger() << "    Segment: " << bus->segId << ", bus: " << bus->busId << ", slot " << slot << frg::endlog;

	uint8_t header_type = io->readConfigByte(bus, slot, 0, kPciHeaderType);
	if((header_type & 0x80) != 0) {
		for(uint32_t function = 0; function < 8; function++)
			checkPciFunction(bus, slot, function, enumerateDownstream);
	}else{
		checkPciFunction(bus, slot, 0, enumerateDownstream);
	}
}

template <typename EnumFunc>
void checkPciBus(PciBus *bus, EnumFunc &&enumerateDownstream) {
	auto bridge = bus->associatedBridge;
	uint32_t nSlots = 32;

	// A PCIe downstream port has only one device (slot 0) attached.
	// In theory, this is only an optimization, in practice however omitting
	// this causes a SError on the BCM2711 when trying to access the vendor ID
	// of a non-existant device.
	if (bridge && bridge->isPcie && bridge->isDownstreamPort)
		nSlots = 1;

	for(uint32_t slot = 0; slot < nSlots; slot++)
		checkPciDevice(bus, slot, enumerateDownstream);
}

void runAllBridges() {
	for(auto root_bus : *allRootBuses) {
		root_bus->runRootBus();

		for(auto bridge : root_bus->childBridges) {
			bridge->runBridge();
		}
	}
}

void runAllDevices() {
	if (!allDevices)
		allDevices.initialize(*kernelAlloc);

	for (auto dev : *allDevices)
		dev->runDevice();
}

void addToEnumerationQueue(PciBus *bus) {
	if (!enumerationQueue)
		enumerationQueue.initialize(*kernelAlloc);

	enumerationQueue->push_back(bus);
}

void addRootBus(PciBus *bus) {
	if (!allRootBuses)
		allRootBuses.initialize(*kernelAlloc);

	allRootBuses->push_back(bus);

	// This assumes we discover all root buses before enumeration
	addToEnumerationQueue(bus);
}

void checkForBridgeResources(PciBridge *bridge) {
	auto io = bridge->parentBus->io;

	{
		uint32_t base, limit;
		base = io->readConfigByte(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgeIoBase);
		limit = io->readConfigByte(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgeIoLimit);

		uint64_t hostBase = 0;
		bool isHostMmio = false;
		auto addr = base << 8;
		auto size = (limit << 8) + 0x100 - addr;

		// Try to look up the host base address in our parent's resources
		for (auto &res : bridge->parentBus->resources) {
			if (res.base() <= addr && (res.base() + res.size()) >= (addr + size)) {
				isHostMmio = res.isHostMmio();
				hostBase = res.hostBase() + (addr - res.base());
				break;
			}
		}

		// If not found, assume PCIe and host address space are the same
		if (!hostBase) {
			hostBase = addr;
		}

		if (size) {
			infoLogger() << "thor: Discovered existing I/O window of bridge "
					<< frg::hex_fmt{bridge->seg} << ":"
					<< frg::hex_fmt{bridge->bus} << ":"
					<< frg::hex_fmt{bridge->slot} << "."
					<< frg::hex_fmt{bridge->function}
					<< " address: " << frg::hex_fmt{addr} << " size: " << size
					<< " (host base: " << frg::hex_fmt{hostBase} << ")" << frg::endlog;

			bridge->associatedBus->resources.push_back({addr, size, hostBase, PciBusResource::io, isHostMmio});
		}
	}

	{
		uint32_t base, limit;

		base = io->readConfigHalf(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgeMemBase);
		limit = io->readConfigHalf(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgeMemLimit);

		uint64_t hostBase = 0;
		auto addr = base << 16;
		auto size = (limit << 16) + 0x100000 - addr;

		// Try to look up the host base address in our parent's resources
		for (auto &res : bridge->parentBus->resources) {
			if (res.base() <= addr && (res.base() + res.size()) >= (addr + size)) {
				hostBase = res.hostBase() + (addr - res.base());
				break;
			}
		}

		// If not found, assume PCIe and host address space are the same
		if (!hostBase) {
			hostBase = addr;
		}

		if (size) {
			infoLogger() << "thor: Discovered existing memory window of bridge "
					<< frg::hex_fmt{bridge->seg} << ":"
					<< frg::hex_fmt{bridge->bus} << ":"
					<< frg::hex_fmt{bridge->slot} << "."
					<< frg::hex_fmt{bridge->function}
					<< " address: " << frg::hex_fmt{addr} << " size: " << size
					<< " (host base: " << frg::hex_fmt{hostBase} << ")" << frg::endlog;

			bridge->associatedBus->resources.push_back({addr, size, hostBase, PciBusResource::memory, true});
		}
	}

	{
		uint64_t base, limit, baseUpper, limitUpper;
		base = io->readConfigHalf(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgePrefetchMemBase);
		limit = io->readConfigHalf(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgePrefetchMemLimit);

		baseUpper = io->readConfigWord(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgePrefetchMemBaseUpper);
		limitUpper = io->readConfigWord(bridge->parentBus, bridge->slot, bridge->function,
			kPciBridgePrefetchMemLimitUpper);

		uint64_t hostBase = 0;
		auto addr = (base << 16) | (baseUpper << 32);
		auto size = ((limit << 16) | (limitUpper << 32)) + 0x100000 - addr;

		// Try to look up the host base address in our parent's resources
		for (auto &res : bridge->parentBus->resources) {
			if (res.base() <= addr && (res.base() + res.size()) >= (addr + size)) {
				hostBase = res.hostBase() + (addr - res.base());
				break;
			}
		}

		// If not found, assume PCIe and host address space are the same
		if (!hostBase) {
			hostBase = addr;
		}

		if (size) {
			infoLogger() << "thor: Discovered existing prefetch memory window of bridge "
					<< frg::hex_fmt{bridge->seg} << ":"
					<< frg::hex_fmt{bridge->bus} << ":"
					<< frg::hex_fmt{bridge->slot} << "."
					<< frg::hex_fmt{bridge->function}
					<< " address: " << frg::hex_fmt{addr} << " size: " << size
					<< " (host base: " << frg::hex_fmt{hostBase} << ")" << frg::endlog;

			bridge->associatedBus->resources.push_back({addr, size, hostBase, PciBusResource::prefMemory, true});
		}
	}
}

void configureBridges(PciBus *root, PciBus *bus, uint32_t &highestId) {
	for (size_t i = 0; i < bus->childBridges.size(); i++) {
		auto bridge = bus->childBridges[i];
		if (!bridge->downstreamId) {
			auto parent = bridge->parentBus->associatedBridge;

			auto b = parent;
			while (b) {
				infoLogger() << "thor: Bumping bridge "
					<< frg::hex_fmt{b->seg} << ":"
					<< frg::hex_fmt{b->bus} << ":"
					<< frg::hex_fmt{b->slot} << "."
					<< frg::hex_fmt{b->function}
					<< " from subordinate id " << b->subordinateId
					<< " to subordinate id " << (b->subordinateId + 1) << frg::endlog;

				b->subordinateId++;
				root->io->writeConfigByte(b->parentBus, b->slot, b->function, kPciBridgeSubordinate, b->subordinateId);
				b = b->parentBus->associatedBridge;
			}

			if (parent) {
				assert(highestId < parent->subordinateId);
				highestId = parent->subordinateId;

				bridge->downstreamId = parent->subordinateId;
				bridge->subordinateId = parent->subordinateId;
			} else {
				// We're directly on the root bus
				// TODO: this ID may be in use by a bridge on a different root bus
				highestId++;

				bridge->downstreamId = highestId;
				bridge->subordinateId = highestId;
			}

			root->io->writeConfigByte(bridge->parentBus, bridge->slot, bridge->function,
					kPciBridgeSecondary, bridge->downstreamId);
			root->io->writeConfigByte(bridge->parentBus, bridge->slot, bridge->function,
					kPciBridgeSubordinate, bridge->subordinateId);

			infoLogger() << "thor: Found unconfigured bridge "
				<< frg::hex_fmt{bridge->seg} << ":"
				<< frg::hex_fmt{bridge->bus} << ":"
				<< frg::hex_fmt{bridge->slot} << "."
				<< frg::hex_fmt{bridge->function}
				<< ", now configured to downstream " << bridge->downstreamId
				<< ", subordinate " << bridge->subordinateId << frg::endlog;

			auto downstreamBus = bus->makeDownstreamBus(bridge, bridge->downstreamId);
			bridge->associatedBus = downstreamBus;
			checkPciBus(downstreamBus,
				[](PciBus *bus) {
					auto br = bus->associatedBridge;
					panicLogger() << "thor: error: found already configured bridge "
						<< frg::hex_fmt{br->seg} << ":"
						<< frg::hex_fmt{br->bus} << ":"
						<< frg::hex_fmt{br->slot} << "."
						<< frg::hex_fmt{br->function}
						<< " under an unconfigured bridge" << frg::endlog;
				}
			);
		}

		assert(bridge->associatedBus && "Bridge has no associated bus");

		// Look for any existing bridge resources
		checkForBridgeResources(bridge);

		configureBridges(root, bridge->associatedBus, highestId);
	}
}

struct SpaceRequirement {
	uintptr_t size;
	uint32_t flags;

	// For devices (or bridge BARs)
	int index = 0;
	PciEntity *associatedEntity = nullptr;

	// For devices behind this bridge
	PciBridge *associatedBridge = nullptr;
};

frg::vector<SpaceRequirement, KernelAlloc> getRequiredSpaceForBus(PciBus *bus) {
	frg::vector<SpaceRequirement, KernelAlloc> required{*kernelAlloc};

	auto processBar = [&required] (PciEntity *entity, int i) {
		auto &bar = entity->getBars()[i];

		if (bar.allocated)
			return;

		uint32_t flags = 0;

		switch (bar.type) {
			case PciBar::kBarNone:
				break;
			case PciBar::kBarIo:
				flags = PciBusResource::io;
				break;
			case PciBar::kBarMemory:
				flags = PciBusResource::memory;

				if (bar.prefetchable)
					flags = PciBusResource::prefMemory;
				break;
			default:
				break;
				panicLogger() << "thor: Invalid BAR type" << frg::endlog;
		}

		if (flags)
			required.push_back({bar.length, flags, i, entity});
	};

	for (auto dev : bus->childDevices) {
		for (int i = 0; i < 6; i++) {
			processBar(dev, i);
		}
	}

	for (auto bridge : bus->childBridges) {
		for (int i = 0; i < 2; i++) {
			processBar(bridge, i);
		}

		assert(bridge->associatedBus);

		// Only require memory allocations if this bus doesn't already have any resources.
		if (!bridge->associatedBus->resources.size()) {
			// Check requirements below bridge
			auto bridgeReq = getRequiredSpaceForBus(bridge->associatedBus);

			size_t requiredIo = 0, requiredMem = 0, requiredPrefMemory = 0;

			for (auto req : bridgeReq) {
				if (req.flags == PciBusResource::io) {
					requiredIo += req.size;
				} else if (req.flags == PciBusResource::prefMemory) {
					requiredPrefMemory += req.size;
				} else {
					assert(req.flags == PciBusResource::memory);
					requiredMem += req.size;
				}
			}

			if (requiredIo) {
				// IO decoded by bridge has 256 byte granularity,
				// but the spec requires it to be 4K aligned
				required.push_back({(requiredIo + 0xFFF) & ~0xFFF,
						PciBusResource::io,
						0, nullptr, bridge});
			}

			// Memory decoded by bridge has 1 MiB granularity

			if (requiredMem) {
				required.push_back({(requiredMem + 0xFFFFF) & ~0xFFFFF,
						PciBusResource::memory,
						0, nullptr, bridge});
			}

			if (requiredPrefMemory) {
				required.push_back({(requiredPrefMemory + 0xFFFFF) & ~0xFFFFF,
						PciBusResource::prefMemory,
						0, nullptr, bridge});
			}
		}
	}

	// We group the same requirement types and sort them by size in descending
	// order to guarantee best fit allocations for requirements of the same type.
	frg::insertion_sort(required.begin(), required.end(), [] (auto a, auto b) {
		if (a.flags == b.flags)
			return a.size < b.size;
		return a.flags > b.flags;
	});

	return required;
}

frg::tuple<PciBusResource *, uint64_t, uint32_t> allocateBar(PciBus *bus, size_t size, uint32_t reqFlags) {
	PciBusResource *best = nullptr;

	auto isAddressable = [] (uint32_t flags, uint64_t addr) {
		if (flags == PciBusResource::io)
			return true;

		if (flags != PciBusResource::prefMemory)
			return addr < 0x100000000;

		return true;
	};

	auto isPreferred = [] (PciBusResource *oldRes, PciBusResource *newRes) {
		if (!oldRes)
			return true;

		if (newRes->base() > oldRes->base())
			return true;

		return newRes->remaining() < oldRes->remaining();
	};

	for (auto &res : bus->resources) {
		if ((reqFlags == PciBusResource::prefMemory
					|| reqFlags == PciBusResource::memory)
				&& res.flags() == PciBusResource::io)
			continue;

		if ((res.flags() == PciBusResource::prefMemory
					|| res.flags() == PciBusResource::memory)
				&& reqFlags == PciBusResource::io)
			continue;

		if (reqFlags == res.flags() && res.canFit(size)
				&& isAddressable(reqFlags, res.base())) {
			best = &res;
			break;
		}

		if ((reqFlags == PciBusResource::prefMemory)
				&& res.flags() != PciBusResource::prefMemory
				&& res.canFit(size)
				&& isPreferred(best, &res)) {
			best = &res;
		}
	}

	if (!best) {
		return {nullptr, 0, 0};
	}

	auto v = best->allocate(size);
	assert(v);

	return {best, *v, best->flags()};
}

void allocateBars(PciBus *bus) {
	auto required = getRequiredSpaceForBus(bus);

	infoLogger() << "thor: Allocating space for entities on bus "
		<< frg::hex_fmt{bus->segId} << ":"
		<< frg::hex_fmt{bus->busId}
		<< ":" << frg::endlog;

	for (auto req : required) {
		auto [resource, off, flags] = allocateBar(bus, req.size, req.flags);

		auto flagsToStr = [] (uint32_t flags) -> const char * {
			if (flags == PciBusResource::io) return "I/O";
			if (flags == PciBusResource::prefMemory) return "pref memory";
			assert(flags == PciBusResource::memory);
			return "memory";
		};

		if (!flags) {
			PciEntity *entity = nullptr;

			auto log = infoLogger();
			log << "thor: Failed to allocate ";

			if (req.associatedBridge) {
				entity = req.associatedBridge;
				log << flagsToStr(req.flags) << " window of bridge ";
			} else {
				entity = req.associatedEntity;
				log << flagsToStr(req.flags) << " BAR #" << req.index << " of entity ";
			}

			log << frg::hex_fmt{entity->seg} << ":"
				<< frg::hex_fmt{entity->bus} << ":"
				<< frg::hex_fmt{entity->slot} << "."
				<< frg::hex_fmt{entity->function}
				<< frg::endlog;

			continue;
		}

		auto childBase = off + resource->base();
		auto hostBase = off + resource->hostBase();

		PciEntity *entity = req.associatedEntity ?: req.associatedBridge;
		auto io = entity->parentBus->io;

		auto log = infoLogger();
		log << "thor: " << flagsToStr(flags) << " ";

		if (req.associatedBridge) {
			log << "window of bridge ";

			switch (req.flags) {
				case PciBusResource::io:
					io->writeConfigByte(entity->parentBus, entity->slot, entity->function,
							kPciBridgeIoBase, childBase >> 8);
					io->writeConfigByte(entity->parentBus, entity->slot, entity->function,
							kPciBridgeIoLimit, (childBase + req.size - 0x100) >> 8);
					break;
				case PciBusResource::memory:
					io->writeConfigHalf(entity->parentBus, entity->slot, entity->function,
							kPciBridgeMemBase, childBase >> 16);
					io->writeConfigHalf(entity->parentBus, entity->slot, entity->function,
							kPciBridgeMemLimit, (childBase + req.size - 0x100000) >> 16);
					break;
				case PciBusResource::prefMemory:
					io->writeConfigHalf(entity->parentBus, entity->slot, entity->function,
							kPciBridgePrefetchMemBase, childBase >> 16);
					io->writeConfigHalf(entity->parentBus, entity->slot, entity->function,
							kPciBridgePrefetchMemLimit, (childBase + req.size - 0x100000) >> 16);
					io->writeConfigWord(entity->parentBus, entity->slot, entity->function,
							kPciBridgePrefetchMemBaseUpper, childBase >> 32);
					io->writeConfigWord(entity->parentBus, entity->slot, entity->function,
							kPciBridgePrefetchMemLimitUpper, (childBase + req.size - 0x100000) >> 32);
					break;
			}

			req.associatedBridge->associatedBus->resources.push_back(
					{childBase, req.size, hostBase, req.flags, resource->isHostMmio()});
		} else {
			log << "BAR #" << req.index << " of entity ";

			auto barVal = io->readConfigWord(entity->parentBus,
					entity->slot, entity->function,
					kPciRegularBar0 + req.index * 4);

			// Write BAR address
			io->writeConfigWord(entity->parentBus, entity->slot, entity->function,
					kPciRegularBar0 + req.index * 4, childBase);

			if (((barVal >> 1) & 3) == 2) {
				io->writeConfigWord(entity->parentBus, entity->slot, entity->function,
					kPciRegularBar0 + (req.index + 1) * 4, childBase >> 32);
			}

			auto &bar = entity->getBars()[req.index];

			// Update our associated BAR object
			bar.allocated = true;
			bar.address = childBase;
			bar.hostType = PciBar::kBarMemory;
			auto offset = hostBase & (kPageSize - 1);
			bar.memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
					hostBase & ~(kPageSize - 1),
					(req.size + offset + (kPageSize - 1)) & ~(kPageSize - 1),
					flags == PciBusResource::io
						? CachingMode::mmioNonPosted
						: CachingMode::mmio);
			bar.offset = offset;

			// Enable address decoding
			auto cmd = io->readConfigHalf(entity->parentBus,
					entity->slot, entity->function, kPciCommand);

			if (flags == PciBusResource::io)
				cmd |= 0x01;
			else
				cmd |= 0x02;

			io->writeConfigHalf(entity->parentBus, entity->slot,
					entity->function, kPciCommand, cmd);
		}

		log << frg::hex_fmt{entity->seg} << ":"
			<< frg::hex_fmt{entity->bus} << ":"
			<< frg::hex_fmt{entity->slot} << "."
			<< frg::hex_fmt{entity->function}
			<< " allocated to " << (void *)childBase;

		if (childBase != hostBase)
			log << " (host " << (void *)hostBase << ")";

		log << ", size " << req.size << " bytes"
			<< frg::endlog;
	}

	for (auto bridge : bus->childBridges)
		allocateBars(bridge->associatedBus);
}

uint32_t findHighestId(PciBus *bus) {
	uint32_t id = bus->busId;

	for (auto bridge : bus->childBridges) {
		if (!bridge->subordinateId)
			continue;

		if (id < bridge->subordinateId)
			id = bridge->subordinateId;
	}

	return id;
}

void enumerateAll() {
	if (!enumerationQueue)
		enumerationQueue.initialize(*kernelAlloc);

	if (!allRootBuses)
		allRootBuses.initialize(*kernelAlloc);

	if (!allDevices)
		allDevices.initialize(*kernelAlloc);

	for(size_t i = 0; i < enumerationQueue->size(); i++) {
		auto bus = (*enumerationQueue)[i];
		checkPciBus(bus, addToEnumerationQueue);
	}

	// Configure unconfigured bridges
	infoLogger() << "thor: Looking for unconfigured PCI bridges" << frg::endlog;

	for (auto rootBus : *allRootBuses) {
		uint32_t i = findHighestId(rootBus);
		configureBridges(rootBus, rootBus, i);
		allocateBars(rootBus);
	}
}

void addConfigSpaceIo(uint32_t seg, uint32_t bus, PciConfigIo *io) {
	if (!allConfigSpaces)
		allConfigSpaces.initialize(frg::hash<uint32_t>{}, *kernelAlloc);

	allConfigSpaces->insert((seg << 8) | bus, io);
}

uint32_t readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset) {
	auto io = (*allConfigSpaces)[(seg << 8) | bus];
	assert(io);

	return io->readConfigWord(seg, bus, slot, function, offset);
}

uint16_t readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset) {
	auto io = (*allConfigSpaces)[(seg << 8) | bus];
	assert(io);

	return io->readConfigHalf(seg, bus, slot, function, offset);
}

uint8_t readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset) {
	auto io = (*allConfigSpaces)[(seg << 8) | bus];
	assert(io);

	return io->readConfigByte(seg, bus, slot, function, offset);
}

// write to pci configuration space
void writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint32_t value) {
	auto io = (*allConfigSpaces)[(seg << 8) | bus];
	assert(io);

	io->writeConfigWord(seg, bus, slot, function, offset, value);
}

void writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint16_t value) {
	auto io = (*allConfigSpaces)[(seg << 8) | bus];
	assert(io);

	io->writeConfigHalf(seg, bus, slot, function, offset, value);
}

void writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint8_t value) {
	auto io = (*allConfigSpaces)[(seg << 8) | bus];
	assert(io);

	io->writeConfigByte(seg, bus, slot, function, offset, value);
}

} } // namespace thor::pci
