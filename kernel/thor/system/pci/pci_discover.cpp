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

frg::manual_box<frg::vector<frigg::SharedPtr<PciDevice>, KernelAlloc>> allDevices;

namespace {
	coroutine<bool> handleReq(LaneHandle lane, frigg::SharedPtr<PciDevice> device) {
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
				if(device->bars[k].type == PciDevice::kBarIo) {
					assert(device->bars[k].offset == 0);
					msg.set_io_type(managarm::hw::IoType::PORT);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
				}else if(device->bars[k].type == PciDevice::kBarMemory) {
					msg.set_io_type(managarm::hw::IoType::MEMORY);
					msg.set_address(device->bars[k].address);
					msg.set_length(device->bars[k].length);
					msg.set_offset(device->bars[k].offset);
				}else{
					assert(device->bars[k].type == PciDevice::kBarNone);
					msg.set_io_type(managarm::hw::IoType::NO_BAR);
				}
				resp.add_bars(std::move(msg));
			}

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_BAR) {
			auto index = req.index();

			AnyDescriptor descriptor;
			if(device->bars[index].type == PciDevice::kBarIo) {
				descriptor = IoDescriptor{device->bars[index].io};
			}else{
				assert(device->bars[index].type == PciDevice::kBarMemory);
				descriptor = MemoryViewDescriptor{device->bars[index].memory};
			}

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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
			auto object = frigg::makeShared<IrqObject>(*kernelAlloc,
					frg::string<KernelAlloc>{*kernelAlloc, "pci-irq."}
					+ frg::to_allocated_string(*kernelAlloc, device->bus)
					+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
					+ frg::to_allocated_string(*kernelAlloc, device->slot)
					+ frg::string<KernelAlloc>{*kernelAlloc, "-"}
					+ frg::to_allocated_string(*kernelAlloc, device->function));
			IrqPin::attachSink(device->interrupt, object.get());

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::BUSIRQ_ENABLE) {
			auto command = readPciHalf(device->bus, device->slot, device->function, kPciCommand);
			writePciHalf(device->bus, device->slot, device->function,
					kPciCommand, command & ~uint16_t{0x400});

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::BUSMASTER_ENABLE) {
			auto command = readPciHalf(device->bus, device->slot, device->function, kPciCommand);
			writePciHalf(device->bus, device->slot, device->function,
					kPciCommand, command | 0x0004);

			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::LOAD_PCI_SPACE) {
			// TODO: Perform some sanity checks on the offset.
			uint32_t word;
			if(req.size() == 1) {
				word = readPciByte(device->bus, device->slot, device->function, req.offset());
			}else if(req.size() == 2) {
				word = readPciHalf(device->bus, device->slot, device->function, req.offset());
			}else{
				assert(req.size() == 4);
				word = readPciWord(device->bus, device->slot, device->function, req.offset());
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_word(word);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::STORE_PCI_SPACE) {
			// TODO: Perform some sanity checks on the offset.
			if(req.size() == 1) {
				writePciByte(device->bus, device->slot, device->function, req.offset(), req.word());
			}else if(req.size() == 2) {
				writePciHalf(device->bus, device->slot, device->function, req.offset(), req.word());
			}else{
				assert(req.size() == 4);
				writePciWord(device->bus, device->slot, device->function, req.offset(), req.word());
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			// TODO: improve error handling here.
			assert(respError == Error::success);
		}else if(req.req_type() == managarm::hw::CntReqType::LOAD_PCI_CAPABILITY) {
			assert(req.index() < device->caps.size());

			// TODO: Perform some sanity checks on the offset.
			uint32_t word;
			if(req.size() == 1) {
				word = readPciByte(device->bus, device->slot, device->function,
						device->caps[req.index()].offset + req.offset());
			}else if(req.size() == 2) {
				word = readPciHalf(device->bus, device->slot, device->function,
						device->caps[req.index()].offset + req.offset());
			}else{
				assert(req.size() == 4);
				word = readPciWord(device->bus, device->slot, device->function,
						device->caps[req.index()].offset + req.offset());
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_word(word);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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
			frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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

	coroutine<LaneHandle> createObject(LaneHandle mbusLane, frigg::SharedPtr<PciDevice> device) {
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
		frigg::UniqueMemory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
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

	coroutine<void> handleBind(LaneHandle objectLane, frigg::SharedPtr<PciDevice> device) {
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
		frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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
				frigg::SharedPtr<PciDevice> device) -> coroutine<void> {
			while(true) {
				if(!(co_await handleReq(lane, device)))
					break;
			}
		}(std::move(stream.get<0>()), std::move(device)));
	}
}

void runDevice(frigg::SharedPtr<PciDevice> device) {
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, [] (frigg::SharedPtr<PciDevice> device)
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

void checkPciFunction(PciBus *bus, uint32_t slot, uint32_t function) {
	uint16_t vendor = readPciHalf(bus->busId, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;

	uint8_t header_type = readPciByte(bus->busId, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		infoLogger() << "        Function " << function << ": Device";
	}else if((header_type & 0x7F) == 1) {
		uint8_t downstreamId = readPciByte(bus->busId, slot, function, kPciBridgeSecondary);
		infoLogger() << "        Function " << function
				<< ": PCI-to-PCI bridge to bus " << (int)downstreamId;
	}else{
		infoLogger() << "        Function " << function
				<< ": Unexpected PCI header type " << (header_type & 0x7F);
	}

	auto command = readPciHalf(bus->busId, slot, function, kPciCommand);
	if(command & 0x01)
		infoLogger() << " (Decodes IO)";
	if(command & 0x02)
		infoLogger() << " (Decodes Memory)";
	if(command & 0x04)
		infoLogger() << " (Busmaster)";
	if(command & 0x400)
		infoLogger() << " (IRQs masked)";
	infoLogger() << frg::endlog;
	writePciHalf(bus->busId, slot, function, kPciCommand, command | 0x400);

	auto device_id = readPciHalf(bus->busId, slot, function, kPciDevice);
	auto revision = readPciByte(bus->busId, slot, function, kPciRevision);
	auto class_code = readPciByte(bus->busId, slot, function, kPciClassCode);
	auto sub_class = readPciByte(bus->busId, slot, function, kPciSubClass);
	auto interface = readPciByte(bus->busId, slot, function, kPciInterface);
	infoLogger() << "            Vendor/device: " << frg::hex_fmt(vendor)
			<< "." << frg::hex_fmt(device_id) << "." << frg::hex_fmt(revision)
			<< ", class: " << frg::hex_fmt(class_code)
			<< "." << frg::hex_fmt(sub_class)
			<< "." << frg::hex_fmt(interface) << frg::endlog;

	if((header_type & 0x7F) == 0) {
		uint16_t subsystem_vendor = readPciHalf(bus->busId, slot, function, kPciRegularSubsystemVendor);
		uint16_t subsystem_device = readPciHalf(bus->busId, slot, function, kPciRegularSubsystemDevice);
//		infoLogger() << "        Subsystem vendor: 0x" << frg::hex_fmt(subsystem_vendor)
//				<< ", device: 0x" << frg::hex_fmt(subsystem_device) << frg::endlog;

		auto status = readPciHalf(bus->busId, slot, function, kPciStatus);

		if(status & 0x08)
			infoLogger() << "\e[35m                IRQ is asserted!\e[39m" << frg::endlog;

		auto device = frigg::makeShared<PciDevice>(*kernelAlloc, bus, bus->busId, slot, function,
				vendor, device_id, revision, class_code, sub_class, interface, subsystem_vendor, subsystem_device);

		// Find all capabilities.
		if(status & 0x10) {
			// The bottom two bits of each capability offset must be masked!
			uint8_t offset = readPciByte(bus->busId, slot, function, kPciRegularCapabilities) & 0xFC;
			while(offset != 0) {
				auto type = readPciByte(bus->busId, slot, function, offset);

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
					size = readPciByte(bus->busId, slot, function, offset + 2);

				device->caps.push({type, offset, size});

				uint8_t successor = readPciByte(bus->busId, slot, function, offset + 1);
				offset = successor & 0xFC;
			}
		}

		// Determine the BARs
		for(int i = 0; i < 6; i++) {
			uint32_t offset = kPciRegularBar0 + i * 4;
			uint32_t bar = readPciWord(bus->busId, slot, function, offset);
			if(bar == 0)
				continue;

			if((bar & 1) != 0) {
				uintptr_t address = bar & 0xFFFFFFFC;

				// write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus->busId, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus->busId, slot, function, offset) & 0xFFFFFFFC;
				writePciWord(bus->busId, slot, function, offset, bar);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarIo;
				device->bars[i].address = address;
				device->bars[i].length = length;

				device->bars[i].io = frigg::makeShared<IoSpace>(*kernelAlloc);
				for(size_t p = 0; p < length; ++p)
					device->bars[i].io->addPort(address + p);
				device->bars[i].offset = 0;

				infoLogger() << "            I/O space BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " ports" << frg::endlog;
			}else if(((bar >> 1) & 3) == 0) {
				uint32_t address = bar & 0xFFFFFFF0;

				// Write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus->busId, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus->busId, slot, function, offset) & 0xFFFFFFF0;
				writePciWord(bus->busId, slot, function, offset, bar);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarMemory;
				device->bars[i].address = address;
				device->bars[i].length = length;

				auto offset = address & (kPageSize - 1);
				device->bars[i].memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::null);
				device->bars[i].offset = offset;

				infoLogger() << "            32-bit memory BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " bytes" << frg::endlog;
			}else if(((bar >> 1) & 3) == 2) {
				assert(i < 5); // Otherwise there is no next bar.
				auto high = readPciWord(bus->busId, slot, function, offset + 4);;
				auto address = (uint64_t{high} << 32) | (bar & 0xFFFFFFF0);

				// Write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus->busId, slot, function, offset, 0xFFFFFFFF);
				writePciWord(bus->busId, slot, function, offset + 4, 0xFFFFFFFF);
				uint32_t mask = (uint64_t{readPciWord(bus->busId, slot, function, offset + 4)} << 32)
						| (readPciWord(bus->busId, slot, function, offset) & 0xFFFFFFF0);
				writePciWord(bus->busId, slot, function, offset, bar);
				writePciWord(bus->busId, slot, function, offset + 4, high);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarMemory;
				device->bars[i].address = address;
				device->bars[i].length = length;

				auto offset = address & (kPageSize - 1);
				device->bars[i].memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1),
						CachingMode::null);
				device->bars[i].offset = offset;

				infoLogger() << "            64-bit memory BAR #" << i
						<< " at 0x" << frg::hex_fmt(address)
						<< ", length: " << length << " bytes" << frg::endlog;
				i++;
			}else{
				assert(!"Unexpected BAR type");
			}
		}

		auto irq_index = static_cast<IrqIndex>(readPciByte(bus->busId, slot, function,
				kPciRegularInterruptPin));
		if(irq_index != IrqIndex::null) {
			auto irq_pin = bus->resolveIrqRoute(slot, irq_index);
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

		allDevices->push(device);
	}else if((header_type & 0x7F) == 1) {
		auto bridge = frg::construct<PciBridge>(*kernelAlloc, bus, bus->busId, slot, function);

		uint8_t downstreamId = readPciByte(bus->busId, slot, function, kPciBridgeSecondary);
		auto downstreamBus = bus->makeDownstreamBus(bridge, downstreamId);
		enumerationQueue->push_back(std::move(downstreamBus)); // FIXME: move should be unnecessary.
	}

	// TODO: This should probably be moved somewhere else.
	if(class_code == 0x0C && sub_class == 0x03 && interface == 0x00) {
		infoLogger() << "            \e[32mDisabling UHCI SMI generation!\e[39m"
				<< frg::endlog;
		writePciHalf(bus->busId, slot, function, 0xC0, 0x2000);
	}
}

void checkPciDevice(PciBus *bus, uint32_t slot) {
	uint16_t vendor = readPciHalf(bus->busId, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;

	infoLogger() << "    Bus: " << bus->busId << ", slot " << slot << frg::endlog;

	uint8_t header_type = readPciByte(bus->busId, slot, 0, kPciHeaderType);
	if((header_type & 0x80) != 0) {
		for(uint32_t function = 0; function < 8; function++)
			checkPciFunction(bus, slot, function);
	}else{
		checkPciFunction(bus, slot, 0);
	}
}

void checkPciBus(PciBus *bus) {
	for(uint32_t slot = 0; slot < 32; slot++)
		checkPciDevice(bus, slot);
}

void runAllDevices() {
	for(auto it = allDevices->begin(); it != allDevices->end(); ++it) {
		runDevice(*it);
	}
}

void addToEnumerationQueue(PciBus *bus) {
	if (!enumerationQueue)
		enumerationQueue.initialize(*kernelAlloc);

	enumerationQueue->push_back(bus);
}

void enumerateAll() {
	if (!allDevices)
		allDevices.initialize(*kernelAlloc);

	for(size_t i = 0; i < enumerationQueue->size(); i++) {
		auto bus = (*enumerationQueue)[i];
		checkPciBus(bus);
	}
}

} } // namespace thor::pci
