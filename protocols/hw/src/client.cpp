
#include <memory>
#include <iostream>

#include <vector>

#include <string.h>

#include <frg/std_compat.hpp>

#include <helix/ipc.hpp>
#include <hw.bragi.hpp>
#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>
#include "protocols/hw/client.hpp"

// Now we can be confident casting between these values
static_assert((int)protocols::hw::IoType::kIoTypeNone == (int)managarm::hw::IoType::NO_BAR, "Bad kIoTypeNone value");
static_assert((int)protocols::hw::IoType::kIoTypeMemory == (int)managarm::hw::IoType::MEMORY, "Bad kIoTypeMemory value");
static_assert((int)protocols::hw::IoType::kIoTypePort == (int)managarm::hw::IoType::PORT, "Bad kIoTypePort value");

namespace protocols {
namespace hw {

async::result<helix::UniqueDescriptor> accessIrq(helix::UniqueLane& lane) {
	managarm::hw::AccessIrqRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail, pull_irq] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::pullDescriptor()
		);

	HEL_CHECK(recv_tail.error());
	HEL_CHECK(pull_irq.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	co_return pull_irq.descriptor();
}

async::result<PciInfo> Device::getPciInfo() {
	managarm::hw::GetPciInfoRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	PciInfo info{
		.numMsis = resp.num_msis()
	};

	for(size_t i = 0; i < resp.capabilities_size(); i++)
		info.caps.push_back({resp.capabilities(i).type()});

	for(int i = 0; i < 6; i++) {
		if(resp.bars(i).io_type() == managarm::hw::IoType::NO_BAR) {
			info.barInfo[i].ioType = IoType::kIoTypeNone;
		}else if(resp.bars(i).io_type() == managarm::hw::IoType::PORT) {
			info.barInfo[i].ioType = IoType::kIoTypePort;
		}else if(resp.bars(i).io_type() == managarm::hw::IoType::MEMORY) {
			info.barInfo[i].ioType = IoType::kIoTypeMemory;
		}else{
			throw std::runtime_error("Illegal IoType for io_type!\n");
		}

		if(resp.bars(i).host_type() == managarm::hw::IoType::NO_BAR) {
			info.barInfo[i].hostType = IoType::kIoTypeNone;
		}else if(resp.bars(i).host_type() == managarm::hw::IoType::PORT) {
			info.barInfo[i].hostType = IoType::kIoTypePort;
		}else if(resp.bars(i).host_type() == managarm::hw::IoType::MEMORY) {
			info.barInfo[i].hostType = IoType::kIoTypeMemory;
		}else{
			throw std::runtime_error("Illegal IoType for host_type!\n");
		}
		info.barInfo[i].address = resp.bars(i).address();
		info.barInfo[i].length = resp.bars(i).length();
		info.barInfo[i].offset = resp.bars(i).offset();
	}

	co_return info;
}

async::result<helix::UniqueDescriptor> Device::accessBar(int index) {
	managarm::hw::AccessBarRequest req;
	req.set_index(index);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail, pull_bar] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::pullDescriptor()
		);

	HEL_CHECK(recv_tail.error());
	HEL_CHECK(pull_bar.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	auto bar = pull_bar.descriptor();
	co_return std::move(bar);
}

async::result<helix::UniqueDescriptor> Device::accessIrq() {
	return ::protocols::hw::accessIrq(_lane);
}

async::result<helix::UniqueDescriptor> Device::installMsi(int index) {
	managarm::hw::InstallMsiRequest req;
	req.set_index(index);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail, pull_msi] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::pullDescriptor()
		);

	HEL_CHECK(recv_tail.error());
	HEL_CHECK(pull_msi.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	co_return pull_msi.descriptor();
}

async::result<void> Device::claimDevice() {
	managarm::hw::ClaimDeviceRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
}

async::result<void> Device::enableBusIrq() {
	managarm::hw::EnableBusIrqRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
}

async::result<void> Device::enableMsi() {
	managarm::hw::EnableMsiRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
}

async::result<void> Device::enableBusmaster() {
	managarm::hw::EnableBusmasterRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
}

async::result<uint32_t> Device::loadPciSpace(size_t offset, unsigned int size) {
	managarm::hw::LoadPciSpaceRequest req;
	req.set_offset(offset);
	req.set_size(size);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	co_return resp.word();
}

async::result<void> Device::storePciSpace(size_t offset, unsigned int size, uint32_t word) {
	managarm::hw::StorePciSpaceRequest req;
	req.set_offset(offset);
	req.set_size(size);
	req.set_word(word);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
}

async::result<uint32_t> Device::loadPciCapability(unsigned int index, size_t offset, unsigned int size) {
	managarm::hw::LoadPciCapabilityRequest req;
	req.set_index(index);
	req.set_offset(offset);
	req.set_size(size);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	co_return resp.word();
}

async::result<FbInfo> Device::getFbInfo() {
	managarm::hw::GetFbInfoRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	FbInfo info;

	info.pitch = resp.fb_pitch();
	info.width = resp.fb_width();
	info.height = resp.fb_height();
	info.bpp = resp.fb_bpp();
	info.type = resp.fb_type();

	co_return info;
}

async::result<helix::UniqueDescriptor> Device::accessFbMemory() {
	managarm::hw::AccessFbMemoryRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail, pull_bar] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::pullDescriptor()
		);

	HEL_CHECK(recv_tail.error());
	HEL_CHECK(pull_bar.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	auto bar = pull_bar.descriptor();
	co_return std::move(bar);
}

async::result<std::vector<BusDeviceMemoryInfo>> BusDevice::getMemoryRegions() {
	managarm::hw::GetMemoryRegionsRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::want_lane,
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());
	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	std::vector<protocols::hw::BusDeviceMemoryInfo> regions;
	for(auto& region : resp.regions()) {
		regions.emplace_back(region.tag(), (protocols::hw::IoType)region.type(), region.address(), region.length());
	}

	co_return std::move(regions);
}

async::result<helix::UniqueDescriptor> BusDevice::accessMemory(int index) {
	managarm::hw::AccessMemoryRequest req;
	req.set_index(index);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto preamble = bragi::read_preamble(recv_head);
	assert(!preamble.error());
	recv_head.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail, pull_bar] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::pullDescriptor()
		);

	HEL_CHECK(recv_tail.error());
	HEL_CHECK(pull_bar.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	auto bar = pull_bar.descriptor();
	co_return std::move(bar);
}

async::result<helix::UniqueDescriptor> BusDevice::accessIrq() {
	return ::protocols::hw::accessIrq(_lane);
}

} } // namespace protocols::hw

