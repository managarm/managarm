#include <algorithm>
#include <hw.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/framebuffer/boot-screen.hpp>
#include <thor-internal/pci/pci.hpp>

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

namespace {
	coroutine<bool> handleReq(LaneHandle lane, smarter::shared_ptr<PciDevice> device) {
		auto [acceptError, conversation] = co_await AcceptSender{lane};
		if(acceptError == Error::endOfLane)
			co_return false;
		// TODO: improve error handling here.
		assert(acceptError == Error::success);

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		// TODO: improve error handling here.
		assert(reqError == Error::success);

		managarm::hw::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

		if(req.req_type() == managarm::hw::CntReqType::GET_PCI_INFO) {
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			for(size_t i = 0; i < device->caps.size(); i++) {
				managarm::hw::PciCapability<KernelAlloc> msg(*kernelAlloc);
				msg.set_type(device->caps[i].type);
				msg.set_offset(device->caps[i].offset);
				msg.set_length(device->caps[i].length);
				resp.add_capabilities(std::move(msg));
			}

			for(size_t k = 0; k < 6; k++) {
				managarm::hw::PciBar<KernelAlloc> msg(*kernelAlloc);
				if(device->bars[k].type == PciBar::kBarIo) {
					assert(device->bars[k].offset == 0);
					msg.set_io_type(managarm::hw::IoType::PORT);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
				}else if(device->bars[k].type == PciBar::kBarMemory) {
					msg.set_io_type(managarm::hw::IoType::MEMORY);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
					msg.set_offset(device->bars[k].offset);
				}else{
					assert(device->bars[k].type == PciBar::kBarNone);
					msg.set_io_type(managarm::hw::IoType::NO_BAR);
				}
				resp.add_bars(std::move(msg));
			}

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_BAR) {
			auto index = req.index();

			AnyDescriptor descriptor;
			if(device->bars[index].type == PciBar::kBarIo) {
				descriptor = IoDescriptor{device->bars[index].io};
			}else{
				assert(device->bars[index].type == PciBar::kBarMemory);
				descriptor = MemoryViewDescriptor{device->bars[index].memory};
			}

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);

			auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};
			// TODO: improve error handling here.
			assert(descError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_IRQ) {
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			assert(device->interrupt);
			auto object = smarter::allocate_shared<IrqObject>(*kernelAlloc,
					frg::string<KernelAlloc>{*kernelAlloc, "pci-irq."}
					+ frg::to_allocated_string(*kernelAlloc, device->bus)
					+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
					+ frg::to_allocated_string(*kernelAlloc, device->slot)
					+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
					+ frg::to_allocated_string(*kernelAlloc, device->function));
			IrqPin::attachSink(device->interrupt, object.get());

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);

			auto descError = co_await PushDescriptorSender{conversation, IrqDescriptor{object}};
			// TODO: improve error handling here.
			assert(descError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::CLAIM_DEVICE) {
			if(device->associatedScreen) {
				infoLogger() << "thor: Disabling screen associated with PCI device "
						<< device->bus << "." << device->slot << "." << device->function
						<< frg::endlog;
				disableLogHandler(device->associatedScreen);
			}

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::BUSIRQ_ENABLE) {
			auto io = device->parentBus->io;

			auto command = io->readConfigHalf(device->parentBus,
					device->slot, device->function, kPciCommand);
			io->writeConfigHalf(device->parentBus, device->slot, device->function,
					kPciCommand, command & ~uint16_t{0x400});

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::BUSMASTER_ENABLE) {
			auto io = device->parentBus->io;

			auto command = io->readConfigHalf(device->parentBus,
					device->slot, device->function, kPciCommand);
			io->writeConfigHalf(device->parentBus, device->slot, device->function,
					kPciCommand, command | 0x0004);

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::LOAD_PCI_SPACE) {
			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

			auto io = device->parentBus->io;

			if(req.size() == 1) {
				if(isValidConfigAccess(1, req.offset())) {
					auto word = io->readConfigByte(device->parentBus,
							device->slot, device->function, req.offset());
					resp.set_error(managarm::hw::Errors::SUCCESS);
					resp.set_word(word);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else if(req.size() == 2) {
				if(isValidConfigAccess(2, req.offset())) {
					auto word = io->readConfigHalf(device->parentBus,
							device->slot, device->function, req.offset());
					resp.set_error(managarm::hw::Errors::SUCCESS);
					resp.set_word(word);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else if(req.size() == 4) {
				if(isValidConfigAccess(4, req.offset())) {
					auto word = io->readConfigWord(device->parentBus,
							device->slot, device->function, req.offset());
					resp.set_error(managarm::hw::Errors::SUCCESS);
					resp.set_word(word);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::STORE_PCI_SPACE) {
			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

			auto io = device->parentBus->io;

			if(req.size() == 1) {
				if(isValidConfigAccess(1, req.offset())) {
					io->writeConfigByte(device->parentBus, device->slot, device->function,
							req.offset(), req.word());
					resp.set_error(managarm::hw::Errors::SUCCESS);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else if(req.size() == 2) {
				if(isValidConfigAccess(2, req.offset())) {
					io->writeConfigHalf(device->parentBus, device->slot, device->function,
							req.offset(), req.word());
					resp.set_error(managarm::hw::Errors::SUCCESS);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else if(req.size() == 4) {
				if(isValidConfigAccess(4, req.offset())) {
					io->writeConfigWord(device->parentBus, device->slot, device->function,
							req.offset(), req.word());
					resp.set_error(managarm::hw::Errors::SUCCESS);
				}else{
					resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
				}
			}else{
				resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
			}

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::LOAD_PCI_CAPABILITY) {
			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};

			auto io = device->parentBus->io;

			if(req.index() < device->caps.size()) {
				if(req.size() == 1) {
					if(isValidConfigAccess(1, req.offset())) {
						auto word = io->readConfigByte(device->parentBus,
								device->slot, device->function,
								device->caps[req.index()].offset + req.offset());
						resp.set_error(managarm::hw::Errors::SUCCESS);
						resp.set_word(word);
					}else{
						resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
					}
				}else if(req.size() == 2) {
					if(isValidConfigAccess(2, req.offset())) {
						auto word = io->readConfigHalf(device->parentBus,
								device->slot, device->function,
								device->caps[req.index()].offset + req.offset());
						resp.set_error(managarm::hw::Errors::SUCCESS);
						resp.set_word(word);
					}else{
						resp.set_error(managarm::hw::Errors::ILLEGAL_ARGUMENTS);
					}
				}else if(req.size() == 4) {
					if(isValidConfigAccess(4, req.offset())) {
						auto word = io->readConfigWord(device->parentBus,
								device->slot, device->function,
								device->caps[req.index()].offset + req.offset());
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

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::GET_FB_INFO) {
			auto fb = device->associatedFrameBuffer;
			assert(fb);

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_fb_pitch(fb->pitch);
			resp.set_fb_width(fb->width);
			resp.set_fb_height(fb->height);
			resp.set_fb_bpp(fb->bpp);
			resp.set_fb_type(fb->type);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_FB_MEMORY) {
			auto fb = device->associatedFrameBuffer;
			assert(fb);

			MemoryViewDescriptor descriptor{fb->memory};

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);

			auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};
			// TODO: improve error handling here.
			assert(descError == Error::success);
		}else{
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::ILLEGAL_REQUEST);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}

		co_return true;
	}

	// ------------------------------------------------------------------------
	// mbus object creation and management.
	// ------------------------------------------------------------------------

	coroutine<LaneHandle> createObject(LaneHandle mbusLane, smarter::shared_ptr<PciDevice> device) {
		auto [offerError, conversation] = co_await OfferSender{mbusLane};
		// TODO: improve error handling here.
		assert(offerError == Error::success);

		managarm::mbus::Property<KernelAlloc> subsystem_prop(*kernelAlloc);
		subsystem_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "unix.subsystem"));
		auto &subsystem_item = subsystem_prop.mutable_item().mutable_string_item();
		subsystem_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "pci"));

		managarm::mbus::Property<KernelAlloc> bus_prop(*kernelAlloc);
		bus_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-bus"));
		auto &bus_item = bus_prop.mutable_item().mutable_string_item();
		bus_item.set_value(frg::to_allocated_string(*kernelAlloc, device->bus, 16, 2));

		managarm::mbus::Property<KernelAlloc> slot_prop(*kernelAlloc);
		slot_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-slot"));
		auto &slot_item = slot_prop.mutable_item().mutable_string_item();
		slot_item.set_value(frg::to_allocated_string(*kernelAlloc, device->slot, 16, 2));

		managarm::mbus::Property<KernelAlloc> function_prop(*kernelAlloc);
		function_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-function"));
		auto &function_item = function_prop.mutable_item().mutable_string_item();
		function_item.set_value(frg::to_allocated_string(*kernelAlloc, device->function, 16, 1));

		managarm::mbus::Property<KernelAlloc> vendor_prop(*kernelAlloc);
		vendor_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-vendor"));
		auto &vendor_item = vendor_prop.mutable_item().mutable_string_item();
		vendor_item.set_value(frg::to_allocated_string(*kernelAlloc, device->vendor, 16, 4));

		managarm::mbus::Property<KernelAlloc> dev_prop(*kernelAlloc);
		dev_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-device"));
		auto &dev_item = dev_prop.mutable_item().mutable_string_item();
		dev_item.set_value(frg::to_allocated_string(*kernelAlloc, device->deviceId, 16, 4));

		managarm::mbus::Property<KernelAlloc> rev_prop(*kernelAlloc);
		rev_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-revision"));
		auto &rev_item = rev_prop.mutable_item().mutable_string_item();
		rev_item.set_value(frg::to_allocated_string(*kernelAlloc, device->revision, 16, 2));

		managarm::mbus::Property<KernelAlloc> class_prop(*kernelAlloc);
		class_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-class"));
		auto &class_item = class_prop.mutable_item().mutable_string_item();
		class_item.set_value(frg::to_allocated_string(*kernelAlloc, device->classCode, 16, 2));

		managarm::mbus::Property<KernelAlloc> subclass_prop(*kernelAlloc);
		subclass_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-subclass"));
		auto &subclass_item = subclass_prop.mutable_item().mutable_string_item();
		subclass_item.set_value(frg::to_allocated_string(*kernelAlloc, device->subClass, 16, 2));

		managarm::mbus::Property<KernelAlloc> if_prop(*kernelAlloc);
		if_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "pci-interface"));
		auto &if_item = if_prop.mutable_item().mutable_string_item();
		if_item.set_value(frg::to_allocated_string(*kernelAlloc, device->interface, 16, 2));

		managarm::mbus::Property<KernelAlloc> subsystem_vendor_prop(*kernelAlloc);
		subsystem_vendor_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc,
				"pci-subsystem-vendor"));
		auto &subsystem_vendor_item = subsystem_vendor_prop.mutable_item().mutable_string_item();
		subsystem_vendor_item.set_value(frg::to_allocated_string(*kernelAlloc,
				device->subsystemVendor, 16, 2));

		managarm::mbus::Property<KernelAlloc> subsystem_device_prop(*kernelAlloc);
		subsystem_device_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc,
				"pci-subsystem-device"));
		auto &subsystem_device_item = subsystem_device_prop.mutable_item().mutable_string_item();
		subsystem_device_item.set_value(frg::to_allocated_string(*kernelAlloc,
				device->subsystemDevice, 16, 2));

		managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
		req.set_parent_id(1);
		req.add_properties(std::move(subsystem_prop));
		req.add_properties(std::move(bus_prop));
		req.add_properties(std::move(slot_prop));
		req.add_properties(std::move(function_prop));
		req.add_properties(std::move(vendor_prop));
		req.add_properties(std::move(dev_prop));
		req.add_properties(std::move(rev_prop));
		req.add_properties(std::move(class_prop));
		req.add_properties(std::move(subclass_prop));
		req.add_properties(std::move(if_prop));
		req.add_properties(std::move(subsystem_vendor_prop));
		req.add_properties(std::move(subsystem_device_prop));

		if(device->associatedFrameBuffer) {
			managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
			cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
			auto &cls_item = cls_prop.mutable_item().mutable_string_item();
			cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "framebuffer"));
			req.add_properties(std::move(cls_prop));
		}

		frg::string<KernelAlloc> ser(*kernelAlloc);
		req.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
		memcpy(reqBuffer.data(), ser.data(), ser.size());
		auto reqError = co_await SendBufferSender{conversation, std::move(reqBuffer)};
		// TODO: improve error handling here.
		assert(reqError == Error::success);

		auto [respError, respBuffer] = co_await RecvBufferSender{conversation};
		// TODO: improve error handling here.
		assert(respError == Error::success);
		managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.ParseFromArray(respBuffer.data(), respBuffer.size());
		assert(resp.error() == managarm::mbus::Error::SUCCESS);

		auto [descError, descriptor] = co_await PullDescriptorSender{conversation};
		// TODO: improve error handling here.
		assert(descError == Error::success);
		assert(descriptor.is<LaneDescriptor>());
		co_return descriptor.get<LaneDescriptor>().handle;
	}

	coroutine<void> handleBind(LaneHandle objectLane, smarter::shared_ptr<PciDevice> device) {
		auto [acceptError, conversation] = co_await AcceptSender{objectLane};
		// TODO: improve error handling here.
		assert(acceptError == Error::success);

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		// TODO: improve error handling here.
		assert(reqError == Error::success);
		managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
		assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

		managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::mbus::Error::SUCCESS);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
		// TODO: improve error handling here.
		assert(respError == Error::success);

		auto stream = createStream();
		auto descError = co_await PushDescriptorSender{conversation,
				LaneDescriptor{stream.get<1>()}};
		// TODO: improve error handling here.
		assert(descError == Error::success);

		async::detach_with_allocator(*kernelAlloc, [] (LaneHandle lane,
				smarter::shared_ptr<PciDevice> device) -> coroutine<void> {
			while(true) {
				if(!(co_await handleReq(lane, device)))
					break;
			}
		}(std::move(stream.get<0>()), std::move(device)));
	}
}

void runDevice(smarter::shared_ptr<PciDevice> device) {
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, [] (smarter::shared_ptr<PciDevice> device)
				-> coroutine<void> {
			auto objectLane = co_await createObject(*mbusClient, device);
			while(true)
				co_await handleBind(objectLane, device);
		}(device));
	});
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

			// write all 1s to the BAR and read it back to determine this its length.
			io->writeConfigWord(bus, slot, function, offset, 0xFFFFFFFF);
			uint32_t mask = io->readConfigWord(bus, slot, function, offset) & 0xFFFFFFFC;
			io->writeConfigWord(bus, slot, function, offset, bar);

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
				bars[i].allocated = true;
				bars[i].io = smarter::allocate_shared<IoSpace>(*kernelAlloc);
				for(size_t p = 0; p < length; ++p)
					bars[i].io->addPort(address + p);
				bars[i].offset = 0;

				infoLogger() << "            I/O space BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " ports" << frg::endlog;
			}
		}else if(((bar >> 1) & 3) == 0) {
			uint32_t address = bar & 0xFFFFFFF0;

			// Write all 1s to the BAR and read it back to determine this its length.
			io->writeConfigWord(bus, slot, function, offset, 0xFFFFFFFF);
			uint32_t mask = io->readConfigWord(bus, slot, function, offset) & 0xFFFFFFF0;
			io->writeConfigWord(bus, slot, function, offset, bar);

			// Device doesn't decode any address bits from this BAR
			if (!mask)
				continue;

			auto length = computeBarLength(mask);

			bars[i].type = PciBar::kBarMemory;
			bars[i].address = address;
			bars[i].length = length;

			if (!address) {
				infoLogger() << "            unallocated 32-bit memory BAR #" << i
						<< ", length: " << length << " bytes" << frg::endlog;
			} else {
				bars[i].allocated = true;
				auto offset = address & (kPageSize - 1);
				bars[i].memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::null);
				bars[i].offset = offset;

				infoLogger() << "            32-bit memory BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " bytes" << frg::endlog;
			}
		}else if(((bar >> 1) & 3) == 2) {
			assert(i < (nBars - 1)); // Otherwise there is no next bar.
			auto high = io->readConfigWord(bus, slot, function, offset + 4);;
			auto address = (uint64_t{high} << 32) | (bar & 0xFFFFFFF0);

			// Write all 1s to the BAR and read it back to determine this its length.
			io->writeConfigWord(bus, slot, function, offset, 0xFFFFFFFF);
			io->writeConfigWord(bus, slot, function, offset + 4, 0xFFFFFFFF);
			uint32_t mask = (uint64_t{io->readConfigWord(bus, slot, function, offset + 4)} << 32)
					| (io->readConfigWord(bus, slot, function, offset) & 0xFFFFFFF0);
			io->writeConfigWord(bus, slot, function, offset, bar);
			io->writeConfigWord(bus, slot, function, offset + 4, high);

			// Device doesn't decode any address bits from this BAR
			if (!mask) {
				i++;
				continue;
			}

			auto length = computeBarLength(mask);

			bars[i].type = PciBar::kBarMemory;
			bars[i].address = address;
			bars[i].length = length;

			if (!address) {
				infoLogger() << "            unallocated 64-bit memory BAR #" << i
						<< ", length: " << length << " bytes" << frg::endlog;
			} else {
				bars[i].allocated = true;
				auto offset = address & (kPageSize - 1);
				bars[i].memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::null);
				bars[i].offset = offset;

				infoLogger() << "            64-bit memory BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " bytes" << frg::endlog;
			}

			i++;
		}else{
			assert(!"Unexpected BAR type");
		}
	}
}

template <typename EnumFunc>
void checkPciFunction(PciBus *bus, uint32_t slot, uint32_t function,
		EnumFunc &&enumerateDownstream) {
	auto io = bus->io;

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

		// Find all capabilities.
		if(status & 0x10) {
			// The bottom two bits of each capability offset must be masked!
			uint8_t offset = io->readConfigByte(bus, slot, function, kPciRegularCapabilities) & 0xFC;
			while(offset != 0) {
				auto type = io->readConfigByte(bus, slot, function, offset);

				auto name = nameOfCapability(type);
				if(name) {
					infoLogger() << "            " << name << " capability"
							<< frg::endlog;
				}else{
					infoLogger() << "            Capability of type 0x"
							<< frg::hex_fmt((int)type) << frg::endlog;
				}

				// TODO: 
				size_t size = -1;
				if(type == 0x09)
					size = io->readConfigByte(bus, slot, function, offset + 2);

				device->caps.push({type, offset, size});

				uint8_t successor = io->readConfigByte(bus, slot, function, offset + 1);
				offset = successor & 0xFC;
			}
		}

		readEntityBars(device.get(), 6);

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

		allDevices->push_back(device);
	} else if ((header_type & 0x7F) == 1) {
		auto bridge = frg::construct<PciBridge>(*kernelAlloc, bus, bus->segId, bus->busId, slot, function);
		bus->childBridges.push_back(bridge);

		readEntityBars(bridge, 2);

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

	// TODO: This should probably be moved somewhere else.
	if(class_code == 0x0C && sub_class == 0x03 && interface == 0x00) {
		infoLogger() << "            \e[32mDisabling UHCI SMI generation!\e[39m"
				<< frg::endlog;
		io->writeConfigHalf(bus, slot, function, 0xC0, 0x2000);
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
	for(uint32_t slot = 0; slot < 32; slot++)
		checkPciDevice(bus, slot, enumerateDownstream);
}

void runAllDevices() {
	for (auto dev : *allDevices)
		runDevice(dev);
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

void configureBridges(PciBus *root, PciBus *bus, int &highestId) {
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
		configureBridges(root, bridge->associatedBus, highestId);
	}
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
	if (!allDevices)
		allDevices.initialize(*kernelAlloc);

	for(size_t i = 0; i < enumerationQueue->size(); i++) {
		auto bus = (*enumerationQueue)[i];
		checkPciBus(bus, addToEnumerationQueue);
	}

	// Configure unconfigured bridges
	infoLogger() << "thor: Looking for unconfigured PCI bridges" << frg::endlog;

	for (auto rootBus : *allRootBuses) {
		int i = findHighestId(rootBus);
		configureBridges(rootBus, rootBus, i);
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
