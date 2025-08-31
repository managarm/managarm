
#include <vector>

#include <string.h>

#include <frg/std_compat.hpp>

#include <helix/ipc.hpp>
#include <hw.bragi.hpp>
#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>
#include "protocols/hw/client.hpp"

namespace protocols::hw {

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

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recv_tail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);
	recv_head.reset();

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	PciInfo info{};
	info.numMsis = resp.num_msis();
	info.msiX = resp.msi_x();

	for(size_t i = 0; i < resp.capabilities_size(); i++)
		info.caps.push_back({resp.capabilities(i).type()});

	for(size_t i = 0; i < 6; i++) {
		if(i >= resp.bars_size()) {
			info.barInfo[i].ioType = IoType::kIoTypeNone;
			info.barInfo[i].hostType = IoType::kIoTypeNone;
			info.barInfo[i].address = 0;
			info.barInfo[i].length = 0;
			info.barInfo[i].offset = 0;
			continue;
		}

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

	info.expansionRomInfo.address = resp.expansion_rom().address();
	info.expansionRomInfo.length = resp.expansion_rom().length();

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

async::result<helix::UniqueDescriptor> Device::accessExpansionRom() {
	managarm::hw::AccessExpansionRomRequest req;

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

	auto expansion_rom = pull_bar.descriptor();
	co_return std::move(expansion_rom);
}

async::result<helix::UniqueDescriptor> Device::accessIrq(size_t index) {
	managarm::hw::AccessIrqRequest req;
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

async::result<void> Device::getBatteryState(BatteryState &state, bool block) {
	managarm::hw::BatteryStateRequest req;
	req.set_block_until_ready(block);

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

	auto resp = *bragi::parse_head_tail<managarm::hw::BatteryStateReply>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	auto copy_state = []<typename T>(std::optional<T> &state, T data) {
		if(data)
			state = data;
		else
			state = std::nullopt;
	};

	state.charging = (resp.charging() != 0);
	copy_state(state.current_now, resp.current_now());
	copy_state(state.power_now, resp.power_now());
	copy_state(state.energy_now, resp.energy_now());
	copy_state(state.energy_full, resp.energy_full());
	copy_state(state.energy_full_design, resp.energy_full_design());
	copy_state(state.voltage_now, resp.voltage_now());
	copy_state(state.voltage_min_design, resp.voltage_min_design());

	co_return;
}

async::result<std::shared_ptr<AcpiResources>> Device::getResources() {
	managarm::hw::AcpiGetResourcesRequest req;

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

	auto resp = *bragi::parse_head_tail<managarm::hw::AcpiGetResourcesReply>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
	auto res = std::make_shared<AcpiResources>();
	res->io_ports = resp.io_ports();
	res->irqs = resp.irqs();

	co_return res;
}

async::result<DtInfo> Device::getDtInfo() {
	managarm::hw::GetDtInfoRequest req;

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

	DtInfo info{};

	info.numIrqs = resp.num_dt_irqs();

	info.regs.resize(resp.dt_regs_size());

	for(size_t i = 0; i < resp.dt_regs_size(); i++) {
		auto &reg = resp.dt_regs(i);
		info.regs[i].address = reg.address();
		info.regs[i].length = reg.length();
		info.regs[i].offset = reg.offset();
	}

	co_return info;
}

async::result<std::string> Device::getDtPath() {
	managarm::hw::GetDtPathRequest req;

	auto [offer, sendReq, recvHead] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvHead.error());

	auto preamble = bragi::read_preamble(recvHead);
	assert(!preamble.error());
	recvHead.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recvTail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recvTail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::GetDtPathResponse>(recvHead, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);
	co_return resp.path();
}

async::result<std::optional<int64_t>> Device::getDtEntityByPhandle(uint32_t phandle) {
	managarm::hw::GetDtEntityByPhandleRequest req;
	req.set_phandle(phandle);

	auto [offer, sendReq, recvHead] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvHead.error());

	auto resp = *bragi::parse_head_only<managarm::hw::GetDtEntityByPhandleResponse>(recvHead);

	if (resp.error() == managarm::hw::Errors::ILLEGAL_ARGUMENTS) {
		co_return std::nullopt;
	} else {
		assert(resp.error() == managarm::hw::Errors::SUCCESS);
		co_return resp.entity();
	}
}

async::result<std::optional<DtProperty>> Device::getDtProperty(std::string_view name) {
	managarm::hw::GetDtPropertyRequest req;
	req.set_name(std::string(name));

	auto [offer, sendReq, recvHead] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvHead.error());

	auto preamble = bragi::read_preamble(recvHead);
	assert(!preamble.error());
	recvHead.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recvTail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recvTail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::GetDtPropertyResponse>(recvHead, tailBuffer);

	if (resp.error() == managarm::hw::Errors::SUCCESS)
		co_return DtProperty{std::move(resp.data())};

	assert(resp.error() == managarm::hw::Errors::PROPERTY_NOT_FOUND);

	co_return std::nullopt;
}

async::result<std::vector<std::pair<std::string, DtProperty>>> Device::getDtProperties() {
	managarm::hw::GetDtPropertiesRequest req;

	auto [offer, sendReq, recvHead] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvHead.error());

	auto preamble = bragi::read_preamble(recvHead);
	assert(!preamble.error());
	recvHead.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recvTail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recvTail.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::GetDtPropertiesResponse>(recvHead, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	std::vector<std::pair<std::string, DtProperty>> res;
	res.reserve(resp.properties_size());

	for (auto &prop : resp.properties()) {
		res.push_back({std::move(prop.name()), std::move(prop.data())});
	}

	co_return res;
}

async::result<helix::UniqueDescriptor> Device::accessDtRegister(uint32_t index) {
	managarm::hw::AccessDtRegisterRequest req;
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
	auto [recv_tail, pull_reg] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::pullDescriptor()
		);

	HEL_CHECK(recv_tail.error());
	HEL_CHECK(pull_reg.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	auto reg = pull_reg.descriptor();
	co_return std::move(reg);
}

async::result<helix::UniqueDescriptor> Device::installDtIrq(uint32_t index) {
	managarm::hw::InstallDtIrqRequest req;
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
	auto [recv_tail, pull_irq] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size()),
			helix_ng::pullDescriptor()
		);

	HEL_CHECK(recv_tail.error());
	HEL_CHECK(pull_irq.error());

	auto resp = *bragi::parse_head_tail<managarm::hw::SvrResponse>(recv_head, tailBuffer);

	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	auto irq = pull_irq.descriptor();
	co_return std::move(irq);
}

async::result<frg::expected<Error>> Device::enableClock() {
	managarm::hw::EnableClockRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto resp = *bragi::parse_head_only<managarm::hw::ClockResponse>(recv_head);

	if (resp.error() == managarm::hw::Errors::ILLEGAL_OPERATION) {
		co_return Error::illegalOperation;
	} else {
		assert(resp.error() == managarm::hw::Errors::SUCCESS);
		co_return frg::success;
	}
}

async::result<frg::expected<Error>> Device::disableClock() {
	managarm::hw::DisableClockRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto resp = *bragi::parse_head_only<managarm::hw::ClockResponse>(recv_head);

	if (resp.error() == managarm::hw::Errors::ILLEGAL_OPERATION) {
		co_return Error::illegalOperation;
	} else {
		assert(resp.error() == managarm::hw::Errors::SUCCESS);
		co_return frg::success;
	}
}

async::result<frg::expected<Error>> Device::setClockFrequency(uint64_t frequency) {
	managarm::hw::SetClockFrequencyRequest req;
	req.set_frequency(frequency);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto resp = *bragi::parse_head_only<managarm::hw::ClockResponse>(recv_head);

	if (resp.error() == managarm::hw::Errors::ILLEGAL_OPERATION) {
		co_return Error::illegalOperation;
	} else if (resp.error() == managarm::hw::Errors::ILLEGAL_ARGUMENTS) {
		co_return Error::illegalArguments;
	} else {
		assert(resp.error() == managarm::hw::Errors::SUCCESS);
		co_return frg::success;
	}
}

async::result<frg::expected<Error>> Device::enableRegulator() {
	managarm::hw::EnableRegulatorRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto resp = *bragi::parse_head_only<managarm::hw::RegulatorResponse>(recv_head);

	if (resp.error() == managarm::hw::Errors::ILLEGAL_OPERATION) {
		co_return Error::illegalOperation;
	} else {
		assert(resp.error() == managarm::hw::Errors::SUCCESS);
		co_return frg::success;
	}
}

async::result<frg::expected<Error>> Device::disableRegulator() {
	managarm::hw::DisableRegulatorRequest req;

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto resp = *bragi::parse_head_only<managarm::hw::RegulatorResponse>(recv_head);

	if (resp.error() == managarm::hw::Errors::ILLEGAL_OPERATION) {
		co_return Error::illegalOperation;
	} else {
		assert(resp.error() == managarm::hw::Errors::SUCCESS);
		co_return frg::success;
	}
}

async::result<frg::expected<Error>> Device::setRegulatorVoltage(uint64_t microvolts) {
	managarm::hw::SetRegulatorVoltageRequest req;
	req.set_voltage(microvolts);

	auto [offer, send_req, recv_head] = co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_head.error());

	auto resp = *bragi::parse_head_only<managarm::hw::RegulatorResponse>(recv_head);

	if (resp.error() == managarm::hw::Errors::ILLEGAL_OPERATION) {
		co_return Error::illegalOperation;
	} else if (resp.error() == managarm::hw::Errors::ILLEGAL_ARGUMENTS) {
		co_return Error::illegalArguments;
	} else {
		assert(resp.error() == managarm::hw::Errors::SUCCESS);
		co_return frg::success;
	}
}

async::result<void> Device::enableDma() {
	managarm::hw::EnableDmaRequest req;

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
}

async::result<std::vector<uint8_t>> Device::getSmbiosHeader() {
	managarm::hw::GetSmbiosHeaderRequest req;

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

	auto resp = *bragi::parse_head_only<managarm::hw::GetSmbiosHeaderReply>(recv_head);

	std::vector<uint8_t> smbiosHeader(resp.size());
	auto [recv_data] = co_await helix_ng::exchangeMsgs(
		offer.descriptor(),
		helix_ng::recvBuffer(smbiosHeader.data(), smbiosHeader.size())
	);

	HEL_CHECK(recv_data.error());
	co_return std::move(smbiosHeader);
}

async::result<std::vector<uint8_t>> Device::getSmbiosTable() {
	managarm::hw::GetSmbiosTableRequest req;

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

	auto resp = *bragi::parse_head_only<managarm::hw::GetSmbiosTableReply>(recv_head);

	std::vector<uint8_t> smbiosTable(resp.size());
	auto [recv_data] = co_await helix_ng::exchangeMsgs(
		offer.descriptor(),
		helix_ng::recvBuffer(smbiosTable.data(), smbiosTable.size())
	);

	HEL_CHECK(recv_data.error());
	co_return std::move(smbiosTable);
}

} // namespace protocols::hw

