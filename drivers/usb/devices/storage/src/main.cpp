
#include <deque>
#include <optional>
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

namespace proto = protocols::usb;

async::detached StorageDevice::run(int config_num, int intf_num) {
	auto descriptor = (co_await _usbDevice.configurationDescriptor()).unwrap();

	std::optional<int> in_endp_number;
	std::optional<int> out_endp_number;

	proto::walkConfiguration(descriptor, [&] (int type, size_t, void *, const auto &info) {
		if(type == proto::descriptor_type::endpoint) {
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

	auto config = (co_await _usbDevice.useConfiguration(config_num)).unwrap();
	auto intf = (co_await config.useInterface(intf_num, 0)).unwrap();
	auto endp_in = (co_await intf.getEndpoint(proto::PipeType::in, in_endp_number.value())).unwrap();
	auto endp_out = (co_await intf.getEndpoint(proto::PipeType::out, out_endp_number.value())).unwrap();

	if(logSteps)
		std::cout << "block-usb: Device is ready" << std::endl;

	while(true) {
		if(!_queue.empty()) {
			auto req = &_queue.front();
			_queue.pop_front();

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
			(co_await endp_out.transfer(proto::BulkTransfer{proto::XferFlags::kXferToDevice,
					arch::dma_buffer_view{nullptr, &cbw, sizeof(CommandBlockWrapper)}})).unwrap();

			if(logSteps)
				std::cout << "block-usb: Waiting for data" << std::endl;
			if(!req->isWrite) {
				proto::BulkTransfer data_info{proto::XferFlags::kXferToHost,
						arch::dma_buffer_view{nullptr, req->buffer, req->numSectors * 512}};
				// TODO: We want this to be lazy but that only works if can ensure that
				// the next transaction is also posted to the queue.
	//			data_info.lazyNotification = true;
				(co_await endp_in.transfer(data_info)).unwrap();
			}else{
				(co_await endp_out.transfer(proto::BulkTransfer{proto::XferFlags::kXferToDevice,
						arch::dma_buffer_view{nullptr, req->buffer, req->numSectors * 512}})).unwrap();
			}

			if(logSteps)
				std::cout << "block-usb: Waiting for CSW" << std::endl;
			(co_await endp_in.transfer(proto::BulkTransfer{proto::XferFlags::kXferToHost,
					arch::dma_buffer_view{nullptr, &csw, sizeof(CommandStatusWrapper)}})).unwrap();

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

			req->event.raise();
		}else{
			co_await _doorbell.async_wait();
		}
	}
}

async::result<void> StorageDevice::readSectors(uint64_t sector,
		void *buffer, size_t numSectors) {
	Request req{false, sector, buffer, numSectors};
	_queue.push_back(req);
	_doorbell.raise();
	co_await req.event.wait();
}

async::result<void> StorageDevice::writeSectors(uint64_t sector,
		const void *buffer, size_t numSectors) {
	Request req{true, sector, const_cast<void *>(buffer), numSectors};
	_queue.push_back(req);
	_doorbell.raise();
	co_await req.event.wait();
}

async::result<size_t> StorageDevice::getSize() {
	std::cout << "usb: StorageDevice::getSize() is a stub!" << std::endl;
	co_return 0;
}

async::detached bindDevice(mbus_ng::Entity entity) {
	auto lane = (co_await entity.getRemoteLane()).unwrap();
	auto device = proto::connect(std::move(lane));

	std::optional<int> config_number;
	std::optional<int> intf_number;
	std::optional<int> intf_class;
	std::optional<int> intf_subclass;
	std::optional<int> intf_protocol;

	if(logEnumeration)
		std::cout << "block-usb: Getting configuration descriptor" << std::endl;

	auto descriptorOrError = co_await device.configurationDescriptor();
	if(!descriptorOrError) {
		std::cout << "usb-hid: Failed to get device descriptor" << std::endl;
		co_return;
	}

	proto::walkConfiguration(descriptorOrError.value(), [&] (int type, size_t, void *p, const auto &info) {
		if(type == proto::descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
		}else if(type == proto::descriptor_type::interface) {
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
			auto desc = (proto::InterfaceDescriptor *)p;
			intf_class = desc->interfaceClass;
			intf_subclass = desc->interfaceSubClass;
			intf_protocol = desc->interfaceProtocol;
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
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"usb.type", "device"},
		mbus_ng::EqualsFilter{"usb.class", "00"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			bindDevice(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "block-usb: Starting driver" << std::endl;

	observeDevices();
	async::run_forever(helix::currentDispatcher);

	return 0;
}


