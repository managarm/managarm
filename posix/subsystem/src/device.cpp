
#include <string.h>

#include "common.hpp"
#include "device.hpp"
#include "extern_fs.hpp"
#include "tmp_fs.hpp"
#include <fs.pb.h>

UnixDeviceRegistry charRegistry;
UnixDeviceRegistry blockRegistry;

// --------------------------------------------------------
// UnixDevice
// --------------------------------------------------------

FutureMaybe<std::shared_ptr<FsLink>> UnixDevice::mount() {
	// TODO: Return an error.
	throw std::logic_error("Device cannot be mounted!");
}

// --------------------------------------------------------
// UnixDeviceRegistry
// --------------------------------------------------------

void UnixDeviceRegistry::install(std::shared_ptr<UnixDevice> device) {
	assert(device->getId() != DeviceId(0, 0));
	// TODO: Ensure that the insert succeeded.
	_devices.insert(device);

	createDeviceNode(device->getName(), device->type(), device->getId());
}

std::shared_ptr<UnixDevice> UnixDeviceRegistry::get(DeviceId id) {
	auto it = _devices.find(id);
	assert(it != _devices.end());
	return *it;
}

// --------------------------------------------------------
// devtmpfs functions.
// --------------------------------------------------------

std::shared_ptr<FsLink> getDevtmpfs() {
	static std::shared_ptr<FsLink> devtmpfs = tmp_fs::createRoot();
	return devtmpfs;
}

COFIBER_ROUTINE(async::result<void>, createDeviceNode(std::string path,
		VfsType type, DeviceId id), ([=] {
	size_t k = 0;
	auto node = getDevtmpfs()->getTarget();
	while(true) {
		size_t s = path.find('/', k);
		if(s == std::string::npos) {
			COFIBER_AWAIT node->mkdev(path.substr(k), type, id);
			break;
		}else{
			assert(s > k);
			auto link = COFIBER_AWAIT node->mkdir(path.substr(k, s - k));
			k = s + 1;
			node = link->getTarget();
		}
	}

	COFIBER_RETURN();
}))

// --------------------------------------------------------
// External device helpers.
// --------------------------------------------------------

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>, openExternalDevice(helix::BorrowedLane lane,
		std::shared_ptr<FsLink> link), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_node;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::DEV_OPEN);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_node));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_node.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	COFIBER_RETURN(extern_fs::createFile(pull_node.descriptor(),
			std::move(link)));
}))

COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>, mountExternalDevice(helix::BorrowedLane lane),
		([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_node;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::DEV_MOUNT);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_node));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_node.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	COFIBER_RETURN(extern_fs::createRoot(pull_node.descriptor()));
}))

