
#include <algorithm>
#include <frigg/debug.hpp>
#include <hw.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>
#include "../../arch/x86/pic.hpp"
#include "../../generic/fiber.hpp"
#include "../../generic/io.hpp"
#include "../../generic/kernel_heap.hpp"
#include "../../generic/service_helpers.hpp"
#include "../../generic/usermem.hpp"
#include "pci.hpp"

namespace thor {

// TODO: Move this to a header file.
extern frigg::LazyInitializer<LaneHandle> mbusClient;

namespace pci {

namespace {
	void handleReq(LaneHandle lane, frigg::SharedPtr<PciDevice> device) {
		auto branch = fiberAccept(lane);

		auto buffer = fiberRecv(branch);
		managarm::hw::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(buffer.data(), buffer.size());
	
		if(req.req_type() == managarm::hw::CntReqType::GET_PCI_INFO) {
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
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
			resp.set_error(managarm::hw::Errors::SUCCESS);
		
			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_BAR) {
			auto index = req.index();

			AnyDescriptor descriptor;
			if(device->bars[index].type == PciDevice::kBarIo) {
				descriptor = IoDescriptor{device->bars[index].io};
			}else{
				assert(device->bars[index].type == PciDevice::kBarMemory);
				descriptor = MemoryAccessDescriptor{device->bars[index].memory};
			}
			
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);
		
			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
			fiberPushDescriptor(branch, descriptor);
		}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_IRQ) {
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::SUCCESS);
	
			assert(device->interrupt);
			auto object = frigg::makeShared<IrqObject>(*kernelAlloc);
			attachIrq(device->interrupt, object.get());

			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
			fiberPushDescriptor(branch, IrqDescriptor{object});
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
		
			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
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
		
			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
		}else{
			managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::hw::Errors::ILLEGAL_REQUEST);
			
			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
		}
	}

	// ------------------------------------------------------------------------
	// mbus object creation and management.
	// ------------------------------------------------------------------------

	LaneHandle createObject(LaneHandle mbus_lane, frigg::SharedPtr<PciDevice> device) {
		auto branch = fiberOffer(mbus_lane);
		
		managarm::mbus::PropertyEntry<KernelAlloc> vendor_prop(*kernelAlloc);
		vendor_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "pci-vendor"));
		vendor_prop.set_value(frigg::to_string(*kernelAlloc, device->vendor, 16, 4));
		
		managarm::mbus::PropertyEntry<KernelAlloc> dev_prop(*kernelAlloc);
		dev_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "pci-device"));
		dev_prop.set_value(frigg::to_string(*kernelAlloc, device->deviceId, 16, 4));
		
		managarm::mbus::PropertyEntry<KernelAlloc> rev_prop(*kernelAlloc);
		rev_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "pci-revision"));
		rev_prop.set_value(frigg::to_string(*kernelAlloc, device->revision, 16, 2));
		
		managarm::mbus::PropertyEntry<KernelAlloc> class_prop(*kernelAlloc);
		class_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "pci-class"));
		class_prop.set_value(frigg::to_string(*kernelAlloc, device->classCode, 16, 2));
		
		managarm::mbus::PropertyEntry<KernelAlloc> subclass_prop(*kernelAlloc);
		subclass_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "pci-subclass"));
		subclass_prop.set_value(frigg::to_string(*kernelAlloc, device->subClass, 16, 2));
		
		managarm::mbus::PropertyEntry<KernelAlloc> if_prop(*kernelAlloc);
		if_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "pci-interface"));
		if_prop.set_value(frigg::to_string(*kernelAlloc, device->interface, 16, 2));
		
		managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
		req.set_parent_id(1);
		req.add_properties(vendor_prop);
		req.add_properties(dev_prop);
		req.add_properties(rev_prop);
		req.add_properties(class_prop);
		req.add_properties(subclass_prop);
		req.add_properties(if_prop);

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

	void handleBind(LaneHandle object_lane, frigg::SharedPtr<PciDevice> device) {
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
		KernelFiber::run([lane = stream.get<0>(), device] () {
			while(true)
				handleReq(lane, device);
		});
	}
}

void registerDevice(frigg::SharedPtr<PciDevice> device) {
	KernelFiber::run([=] {
		auto object_lane = createObject(*mbusClient, device);
		while(true)
			handleBind(object_lane, device);
	});
/*	char vendor[5], device_id[5], revision[3];
	sprintf(vendor, "%.4x", device->vendor);
	sprintf(device_id, "%.4x", device->deviceId);
	sprintf(revision, "%.2x", device->revision);
	
	char class_code[3], sub_class[3], interface[3];
	sprintf(class_code, "%.2x", device->classCode);
	sprintf(sub_class, "%.2x", device->subClass);
	sprintf(interface, "%.2x", device->interface);*/

/*
	std::unordered_map<std::string, std::string> descriptor {
		{ "pci-vendor", vendor },
		{ "pci-device", device_id },
		{ "pci-revision", revision },
		{ "pci-class", class_code },
		{ "pci-subclass", sub_class },
		{ "pci-interface", interface }
	};

	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	char name[9];
	sprintf(name, "%.2x.%.2x.%.1x", device->bus, device->slot, device->function);
	auto object = COFIBER_AWAIT root.createObject(name, descriptor,
			[=] (mbus::AnyQuery query) -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		std::cout << "pre handleDevice()" << std::endl;
		handleDevice(device, std::move(local_lane));
		std::cout << "post handleDevice()" << std::endl;

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});
	std::cout << "Created object " << name << std::endl;
*/
}

// --------------------------------------------------------
// Discovery functionality
// --------------------------------------------------------

size_t computeBarLength(uintptr_t mask) {
	static_assert(sizeof(long) == 8, "Fix builtin usage");
	static_assert(sizeof(uintptr_t) == 8, "Fix builtin usage");
	
	assert(mask);
	size_t length_bits = __builtin_ctzl(mask);
	size_t decoded_bits = 64 - __builtin_clzl(mask);
	//FIXME: assert(__builtin_popcountl(mask) == decoded_bits - length_bits);

	return size_t(1) << length_bits;
}

//FIXME: std::queue<unsigned int> enumerationQueue;

IrqPin *resolveRoute(const RoutingInfo &info, unsigned int slot, IrqIndex index) {
	auto entry = std::find_if(info.begin(), info.end(), [&] (const auto &ref) {
		return ref.slot == slot && ref.index == index;
	});
	assert(entry != info.end());
	assert(entry->pin);
	return entry->pin;
}

void checkPciFunction(uint32_t bus, uint32_t slot, uint32_t function,
		const RoutingInfo &routing) {
	uint16_t vendor = readPciHalf(bus, slot, function, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	uint8_t header_type = readPciByte(bus, slot, function, kPciHeaderType);
	if((header_type & 0x7F) == 0) {
		frigg::infoLogger() << "        Function " << function << ": Device";
	}else if((header_type & 0x7F) == 1) {
		uint8_t secondary = readPciByte(bus, slot, function, kPciBridgeSecondary);
		frigg::infoLogger() << "        Function " << function
				<< ": PCI-to-PCI bridge to bus " << (int)secondary;
		//FIXME: enumerationQueue.push(secondary);
	}else{
		frigg::infoLogger() << "        Function " << function
				<< ": Unexpected PCI header type " << (header_type & 0x7F);
	}

	auto command = readPciHalf(bus, slot, function, kPciCommand);
	if(command & 0x01)
		frigg::infoLogger() << " (Decodes IO)";
	if(command & 0x02)
		frigg::infoLogger() << " (Decodes Memory)";
	if(command & 0x04)
		frigg::infoLogger() << " (Busmaster)";
	if(command & 0x400)
		frigg::infoLogger() << " (IRQs masked)";
	frigg::infoLogger() << frigg::endLog;

	auto device_id = readPciHalf(bus, slot, function, kPciDevice);
	auto revision = readPciByte(bus, slot, function, kPciRevision);
	auto class_code = readPciByte(bus, slot, function, kPciClassCode);
	auto sub_class = readPciByte(bus, slot, function, kPciSubClass);
	auto interface = readPciByte(bus, slot, function, kPciInterface);
	frigg::infoLogger() << "            Vendor/device: " << frigg::logHex(vendor)
			<< "." << frigg::logHex(device_id) << "." << frigg::logHex(revision)
			<< ", class: " << frigg::logHex(class_code)
			<< "." << frigg::logHex(sub_class)
			<< "." << frigg::logHex(interface) << frigg::endLog;

	if((header_type & 0x7F) == 0) {
//		uint16_t subsystem_vendor = readPciHalf(bus, slot, function, kPciRegularSubsystemVendor);
//		uint16_t subsystem_device = readPciHalf(bus, slot, function, kPciRegularSubsystemDevice);
//		frigg::infoLogger() << "        Subsystem vendor: 0x" << frigg::logHex(subsystem_vendor)
//				<< ", device: 0x" << frigg::logHex(subsystem_device) << frigg::endLog;

		auto status = readPciHalf(bus, slot, function, kPciStatus);

		if(status & 0x08)
			frigg::infoLogger() << "\e[35m                IRQ is asserted!\e[39m" << frigg::endLog;

		if(status & 0x10) {
			// NOTE: the bottom two bits of each capability offset must be masked
			uint8_t offset = readPciByte(bus, slot, function, kPciRegularCapabilities) & 0xFC;
			while(offset != 0) {
				uint8_t capability = readPciByte(bus, slot, function, offset);
				uint8_t successor = readPciByte(bus, slot, function, offset + 1);
				
				frigg::infoLogger() << "            Capability 0x"
						<< frigg::logHex((int)capability) << frigg::endLog;
/*
				if(capability == 0x09) {
					uint8_t size = readPciByte(bus, slot, function, offset + 2);

					std::cout << "            Bytes: ";
					for(size_t i = 2; i < size; i++) {
						uint8_t byte = readPciByte(bus, slot, function, offset + i);
						std::cout << (i > 2 ? ", " : "") << std::hex << (int)byte << std::dec;
					}
					std::cout << std::endl;
				}
*/
				offset = successor & 0xFC;
			}
		}

		auto device = frigg::makeShared<PciDevice>(*kernelAlloc, bus, slot, function,
				vendor, device_id, revision, class_code, sub_class, interface);
		
		// determine the BARs
		for(int i = 0; i < 6; i++) {
			uint32_t offset = kPciRegularBar0 + i * 4;
			uint32_t bar = readPciWord(bus, slot, function, offset);
			if(bar == 0)
				continue;
			
			if((bar & 1) != 0) {
				uintptr_t address = bar & 0xFFFFFFFC;
				
				// write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus, slot, function, offset) & 0xFFFFFFFC;
				writePciWord(bus, slot, function, offset, bar);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarIo;
				device->bars[i].address = address;
				device->bars[i].length = length;

				device->bars[i].io = frigg::makeShared<IoSpace>(*kernelAlloc);
				for(size_t p = 0; p < length; ++p)
					device->bars[i].io->addPort(address + p);
				device->bars[i].offset = 0;

				frigg::infoLogger() << "            I/O space BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " ports" << frigg::endLog;
			}else if(((bar >> 1) & 3) == 0) {
				uint32_t address = bar & 0xFFFFFFF0;
				
				// Write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus, slot, function, offset, 0xFFFFFFFF);
				uint32_t mask = readPciWord(bus, slot, function, offset) & 0xFFFFFFF0;
				writePciWord(bus, slot, function, offset, bar);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarMemory;
				device->bars[i].address = address;
				device->bars[i].length = length;
				
				auto offset = address & (kPageSize - 1);
				device->bars[i].memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1));
				device->bars[i].offset = offset;

				frigg::infoLogger() << "            32-bit memory BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " bytes" << frigg::endLog;
			}else if(((bar >> 1) & 3) == 2) {
				assert(i < 5); // Otherwise there is no next bar.
				auto high = readPciWord(bus, slot, function, offset + 4);;
				auto address = (uint64_t{high} << 32) | (bar & 0xFFFFFFF0);
				
				// Write all 1s to the BAR and read it back to determine this its length.
				writePciWord(bus, slot, function, offset, 0xFFFFFFFF);
				writePciWord(bus, slot, function, offset + 4, 0xFFFFFFFF);
				uint32_t mask = (uint64_t{readPciWord(bus, slot, function, offset + 4)} << 32)
						| (readPciWord(bus, slot, function, offset) & 0xFFFFFFF0);
				writePciWord(bus, slot, function, offset, bar);
				writePciWord(bus, slot, function, offset + 4, high);
				auto length = computeBarLength(mask);

				device->bars[i].type = PciDevice::kBarMemory;
				device->bars[i].address = address;
				device->bars[i].length = length;
				
				auto offset = address & (kPageSize - 1);
				device->bars[i].memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
						address & ~(kPageSize - 1),
						(length + offset + (kPageSize - 1)) & ~(kPageSize - 1));
				device->bars[i].offset = offset;

				frigg::infoLogger() << "            64-bit memory BAR #" << i
						<< " at 0x" << frigg::logHex(address)
						<< ", length: " << length << " bytes" << frigg::endLog;
				i++;
			}else{
				assert(!"Unexpected BAR type");
			}
		}

		auto irq_index = static_cast<IrqIndex>(readPciByte(bus, slot, function,
				kPciRegularInterruptPin));
		if(irq_index != IrqIndex::null) {
			auto irq_pin = resolveRoute(routing, slot, irq_index);
			frigg::infoLogger() << "            Interrupt: "
					<< nameOf(irq_index)
					<< " (routed to " << irq_pin->name() << ")" << frigg::endLog;
			device->interrupt = irq_pin;
		}

		registerDevice(device);
		//allDevices.push_back(device);
	}

	// TODO: This should probably be moved somewhere else.
	if(class_code == 0x0C && sub_class == 0x03 && interface == 0x00) {
		frigg::infoLogger() << "            \e[32mDisabling UHCI SMI generation!\e[39m"
				<< frigg::endLog;
		writePciHalf(bus, slot, function, 0xC0, 0x2000);
	}
}

void checkPciFunction(uint32_t bus, uint32_t slot, uint32_t function,
		const RoutingInfo &routing);

void checkPciDevice(uint32_t bus, uint32_t slot,
		const RoutingInfo &routing) {
	uint16_t vendor = readPciHalf(bus, slot, 0, kPciVendor);
	if(vendor == 0xFFFF)
		return;
	
	frigg::infoLogger() << "    Bus: " << bus << ", slot " << slot << frigg::endLog;
	
	uint8_t header_type = readPciByte(bus, slot, 0, kPciHeaderType);
	if((header_type & 0x80) != 0) {
		for(uint32_t function = 0; function < 8; function++)
			checkPciFunction(bus, slot, function, routing);
	}else{
		checkPciFunction(bus, slot, 0, routing);
	}
}

void checkPciBus(uint32_t bus, const RoutingInfo &routing) {
	for(uint32_t slot = 0; slot < 32; slot++)
		checkPciDevice(bus, slot, routing);
}

void pciDiscover(const RoutingInfo &routing) {
	frigg::infoLogger() << "thor: Discovering PCI devices" << frigg::endLog;
	checkPciBus(0, routing);
/*	enumerationQueue.push(0);
	while(!enumerationQueue.empty()) {
		auto bus = enumerationQueue.front();
		enumerationQueue.pop();
		checkPciBus(bus);
	}*/
}

} } // namespace thor::pci

