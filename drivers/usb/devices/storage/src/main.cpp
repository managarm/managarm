
#include <deque>
#include <experimental/optional>
#include <iostream>

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <async/result.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "storage.hpp"

namespace {
	constexpr bool logEnumeration = false;
	constexpr bool logRequests = false;
	constexpr bool logSteps = false;

	// I own a USB key that does not support the READ6 command. ~AvdG
	constexpr bool enableRead6 = false;
}

async::detached StorageDevice::run(int config_num, int intf_num) {
	auto descriptor = co_await _usbDevice.configurationDescriptor();

	std::experimental::optional<int> in_endp_number;
	std::experimental::optional<int> out_endp_number;
	
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == descriptor_type::endpoint) {
			if(info.endpointIn.value()) {
				in_endp_number = info.endpointNumber.value();
			}else if(!info.endpointIn.value()) {
				out_endp_number = info.endpointNumber.value();
			}else {
				throw std::runtime_error("Illegal endpoint!\n");
			}
		}else{
			if(logEnumeration)
				printf("block-usb: Unexpected descriptor type: %d!\n", type);
		}
	});

	if(logSteps)
		std::cout << "block-usb: Setting up configuration" << std::endl;
	
	auto config = co_await _usbDevice.useConfiguration(config_num);
	auto intf = co_await config.useInterface(intf_num, 0);
	auto endp_in = co_await intf.getEndpoint(PipeType::in, in_endp_number.value());
	auto endp_out = co_await intf.getEndpoint(PipeType::out, out_endp_number.value());

	if(logSteps)
		std::cout << "block-usb: Device is ready" << std::endl;

	while(true) {
		if(!_queue.empty()) {
			auto req = &_queue.front();
			
			if(logRequests)
				std::cout << "block-usb: Reading " << req->numSectors << " sectors" << std::endl;
			assert(req->numSectors);
			assert(req->numSectors <= 0xFFFF);

			CommandBlockWrapper cbw;
			memset(&cbw, 0, sizeof(CommandBlockWrapper));
			cbw.signature = Signatures::kSignCbw;
			cbw.tag = 1;
			cbw.transferLength = req->numSectors * 512;
			if(!req->isWrite) {
				cbw.flags = 0x80; // Direction: Device-to-Host.
			}else{
				cbw.flags = 0; // Direction: Host-to-Device.
			}
			cbw.lun = 0;

			if(!req->isWrite) {
				if(enableRead6 && req->sector <= 0x1FFFFF && req->numSectors <= 0xFF) {
					scsi::Read6 command;
					memset(&command, 0, sizeof(scsi::Read6));
					command.opCode = 0x08;
					command.lba[0] = req->sector >> 16;
					command.lba[1] = (req->sector >> 8) & 0xFF;
					command.lba[2] = req->sector & 0xFF;
					command.transferLength = req->numSectors;

					cbw.cmdLength = sizeof(scsi::Read6);
					memcpy(cbw.cmdData, &command, sizeof(scsi::Read6));
				}else if(req->sector <= 0xFFFFFFFF) {
					scsi::Read10 command;
					memset(&command, 0, sizeof(scsi::Read10));
					command.opCode = 0x28;
					command.lba[0] = req->sector >> 24;
					command.lba[1] = (req->sector >> 16) & 0xFF;
					command.lba[2] = (req->sector >> 8) & 0xFF;
					command.lba[3] = req->sector & 0xFF;
					command.transferLength[0] = req->numSectors >> 8;
					command.transferLength[1] = req->numSectors & 0xFF;

					cbw.cmdLength = sizeof(scsi::Read10);
					memcpy(cbw.cmdData, &command, sizeof(scsi::Read10));
				}else{
					throw std::logic_error("USB storage does not currently support high LBAs!");
				}
			}else{
				if(req->sector <= 0xFFFFFFFF) {
					scsi::Write10 command;
					memset(&command, 0, sizeof(scsi::Write10));
					command.opCode = 0x2A;
					command.lba[0] = req->sector >> 24;
					command.lba[1] = (req->sector >> 16) & 0xFF;
					command.lba[2] = (req->sector >> 8) & 0xFF;
					command.lba[3] = req->sector & 0xFF;
					command.transferLength[0] = req->numSectors >> 8;
					command.transferLength[1] = req->numSectors & 0xFF;

					cbw.cmdLength = sizeof(scsi::Write10);
					memcpy(cbw.cmdData, &command, sizeof(scsi::Write10));
				}else{
					throw std::logic_error("USB storage does not currently support high LBAs!");
				}
			}

			// TODO: Respect USB device DMA requirements.

			// TODO: Ideally, we want to post the IN-transfer first.
			// We do this to try to avoid unnecessary IRQs
			// and round-trips to the device and host-controller driver.
			CommandStatusWrapper csw;

			if(logSteps)
				std::cout << "block-usb: Sending CBW" << std::endl;
			co_await endp_out.transfer(BulkTransfer{XferFlags::kXferToDevice,
					arch::dma_buffer_view{nullptr, &cbw, sizeof(CommandBlockWrapper)}});
			
			if(logSteps)
				std::cout << "block-usb: Waiting for data" << std::endl;
			if(!req->isWrite) {
				BulkTransfer data_info{XferFlags::kXferToHost,
						arch::dma_buffer_view{nullptr, req->buffer, req->numSectors * 512}};
				// TODO: We want this to be lazy but that only works if can ensure that
				// the next transaction is also posted to the queue.
	//			data_info.lazyNotification = true;
				auto data_xfer = endp_in.transfer(data_info);
				co_await std::move(data_xfer);
			}else{
				co_await endp_out.transfer(BulkTransfer{XferFlags::kXferToDevice,
						arch::dma_buffer_view{nullptr, req->buffer, req->numSectors * 512}});
			}

			if(logSteps)
				std::cout << "block-usb: Waiting for CSW" << std::endl;
			auto csw_xfer = endp_in.transfer(BulkTransfer{XferFlags::kXferToHost,
					arch::dma_buffer_view{nullptr, &csw, sizeof(CommandStatusWrapper)}});
			co_await std::move(csw_xfer);

			if(logSteps)
				std::cout << "block-usb: Request complete" << std::endl;
			assert(csw.signature == Signatures::kSignCsw);
			assert(csw.tag == 1);
			assert(!csw.dataResidue);
			if(csw.status) {
				std::cout << "block-usb: Error status 0x"
						<< std::hex << (unsigned int)csw.status << std::dec
						<<  " in CSW" << std::endl;
				throw std::runtime_error("block-usb: Giving up");
			}

			req->promise.set_value();
			_queue.pop_front();
			delete req;
		}else{
			co_await _doorbell.async_wait();
		}
	}
}

async::result<void> StorageDevice::readSectors(uint64_t sector,
		void *buffer, size_t numSectors) {
	auto req = new Request{false, sector, buffer, numSectors};
	_queue.push_back(*req);
	auto result = req->promise.async_get();
	_doorbell.ring();
	return result;
}

async::result<void> StorageDevice::writeSectors(uint64_t sector,
		const void *buffer, size_t numSectors) {
	auto req = new Request{true, sector, const_cast<void *>(buffer), numSectors};
	_queue.push_back(*req);
	auto result = req->promise.async_get();
	_doorbell.ring();
	return result;
}

async::detached bindDevice(mbus::Entity entity) {
	auto lane = helix::UniqueLane(co_await entity.bind());
	auto device = protocols::usb::connect(std::move(lane));
	
	std::experimental::optional<int> config_number;
	std::experimental::optional<int> intf_number;
	std::experimental::optional<int> intf_class;
	std::experimental::optional<int> intf_subclass;
	std::experimental::optional<int> intf_protocol;
	
	if(logEnumeration)
		std::cout << "block-usb: Getting configuration descriptor" << std::endl;

	auto descriptor = co_await device.configurationDescriptor();
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
		}else if(type == descriptor_type::interface) {
			if(intf_number) {
				std::cout << "block-usb: Ignoring interface "
						<< info.interfaceNumber.value() << std::endl;
				return;
			}
			if(logEnumeration)
				std::cout << "block-usb: Found interface: " << info.interfaceNumber.value()
						<< ", alternative: " << info.interfaceAlternative.value() << std::endl;
			intf_number = info.interfaceNumber.value();
			
			assert(!intf_class);
			assert(!intf_subclass);
			assert(!intf_protocol);
			auto desc = (InterfaceDescriptor *)p;
			intf_class = desc->interfaceClass;
			intf_subclass = desc->interfaceSubClass;
			intf_protocol = desc->interfaceProtocoll;
		}
	});

	if(logEnumeration)
		std::cout << "block-usb: Device class: 0x" << std::hex << intf_class.value()
				<< ", subclass: 0x" << intf_subclass.value()
				<< ", protocol: 0x" << intf_protocol.value()
				<< std::dec << std::endl;
	if(intf_class.value() != 0x08
			|| intf_subclass.value() != 0x06
			|| intf_protocol.value() != 0x50)
		co_return;

	if(logEnumeration)
		std::cout << "block-usb: Detected USB device" << std::endl;

	auto storage_device = new StorageDevice(device);
	storage_device->run(config_number.value(), intf_number.value());
	blockfs::runDevice(storage_device);
}

async::detached observeDevices() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("usb.type", "device"),
		mbus::EqualsFilter("usb.class", "00")
	});


	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		bindDevice(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "block-usb: Starting driver" << std::endl;

	{
		async::queue_scope scope{helix::globalQueue()};
		observeDevices();
	}

	helix::globalQueue()->run();
	
	return 0;
}


