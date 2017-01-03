
#include <memory>
#include <iostream>

#include <string.h>

#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <cofiber.hpp>
#include "hw.pb.h"
#include "protocols/hw/client.hpp"

namespace protocols {
namespace hw {

COFIBER_ROUTINE(async::result<PciInfo>, Device::getPciInfo(),
		([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;

	managarm::hw::CntRequest req;
	req.set_req_type(managarm::hw::CntReqType::GET_PCI_INFO);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::hw::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::hw::Errors::SUCCESS);
	assert(resp.bars_size() == 6);

	PciInfo info;
	for(int i = 0; i < 6; i++) {
		if(resp.bars(i).io_type() == managarm::hw::IoType::NO_BAR) {
			info.barInfo[i].ioType = IoType::kIoTypeNone;
		}else if(resp.bars(i).io_type() == managarm::hw::IoType::PORT) {
			info.barInfo[i].ioType = IoType::kIoTypePort;
		}else if(resp.bars(i).io_type() == managarm::hw::IoType::MEMORY) {
			info.barInfo[i].ioType = IoType::kIoTypeMemory;
		}else{
			throw std::runtime_error("Illegal IoType!\n");
		}
		info.barInfo[i].address = resp.bars(i).address();
		info.barInfo[i].length = resp.bars(i).length();
	}

	COFIBER_RETURN(info);
}))

COFIBER_ROUTINE(async::result<helix::UniqueDescriptor>, Device::accessBar(int index),
		([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_bar;

	managarm::hw::CntRequest req;
	req.set_req_type(managarm::hw::CntReqType::ACCESS_BAR);
	req.set_index(index);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_bar));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_bar.error());

	managarm::hw::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	auto bar = pull_bar.descriptor();
	COFIBER_RETURN(std::move(bar));
}))

COFIBER_ROUTINE(async::result<helix::UniqueDescriptor>, Device::accessIrq(),
		([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_irq;

	managarm::hw::CntRequest req;
	req.set_req_type(managarm::hw::CntReqType::ACCESS_IRQ);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_irq));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_irq.error());

	managarm::hw::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::hw::Errors::SUCCESS);

	COFIBER_RETURN(pull_irq.descriptor());
}))

COFIBER_ROUTINE(async::result<uint32_t>, Device::loadPciSpace(size_t offset), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;

	managarm::hw::CntRequest req;
	req.set_req_type(managarm::hw::CntReqType::LOAD_PCI_SPACE);
	req.set_offset(offset);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::hw::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::hw::Errors::SUCCESS);
	COFIBER_RETURN(resp.word());
}))

} } // namespace protocols::hw

