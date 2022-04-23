
#include <string.h>

#include <helix/memory.hpp>
#include <protocols/fs/client.hpp>
#include <protocols/fs/defs.hpp>

#include "common.hpp"
#include "device.hpp"
#include "extern_fs.hpp"
#include "tmp_fs.hpp"
#include <fs.bragi.hpp>

#include <bitset>

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

	// TODO: Make createDeviceNode() synchronous and get rid of the post_awaitable().
	auto node_path = device->nodePath();
	if(!node_path.empty())
		async::detach(createDeviceNode(std::move(node_path),
				device->type(), device->getId()));
}

std::shared_ptr<UnixDevice> UnixDeviceRegistry::get(DeviceId id) {
	auto it = _devices.find(id);
	if(it == _devices.end())
		return nullptr;
	return *it;
}

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
openDevice(VfsType type, DeviceId id, std::shared_ptr<MountView> mount,
	std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) {
	if(type == VfsType::charDevice) {
		auto device = charRegistry.get(id);
		if(device == nullptr)
			co_return Error::noBackingDevice;
		co_return co_await device->open(std::move(mount), std::move(link),
				semantic_flags);
	}else{
		assert(type == VfsType::blockDevice);
		auto device = blockRegistry.get(id);
		if(device == nullptr)
			co_return Error::noBackingDevice;
		co_return co_await device->open(std::move(mount), std::move(link),
				semantic_flags);
	}
}

// --------------------------------------------------------
// devtmpfs functions.
// --------------------------------------------------------

std::shared_ptr<FsLink> getDevtmpfs() {
	static std::shared_ptr<FsLink> devtmpfs = tmp_fs::createRoot();
	return devtmpfs;
}

async::result<void> createDeviceNode(std::string path, VfsType type, DeviceId id) {
	size_t k = 0;
	auto node = getDevtmpfs()->getTarget();
	while(true) {
		size_t s = path.find('/', k);
		if(s == std::string::npos) {
			auto result = co_await node->mkdev(path.substr(k), type, id);
			assert(result);
			break;
		}else{
			assert(s > k);
			std::shared_ptr<FsLink> link;
			auto linkResult = co_await node->getLink(path.substr(k, s - k));
			assert(linkResult);
			link = linkResult.value();
			// TODO: Check for errors from mkdir().
			if(!link)
				link = std::get<std::shared_ptr<FsLink>>(
						co_await node->mkdir(path.substr(k, s - k)));
			k = s + 1;
			node = link->getTarget();
		}
	}
}

// --------------------------------------------------------
// File implementation for external devices.
// --------------------------------------------------------

namespace {

constexpr bool logStatusSeqlock = false;

struct DeviceFile : File {
private:
	async::result<frg::expected<Error, off_t>> seek(off_t offset, VfsSeek whence) override {
		assert(whence == VfsSeek::absolute);
		co_await _file.seekAbsolute(offset);
		co_return offset;
	}

	// TODO: Ensure that the process is null? Pass credentials of the thread in the request?
	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t max_length) override {
		size_t length = co_await _file.readSome(data, max_length);
		co_return length;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override {
		size_t progress = 0;
		while(progress < length) {
			size_t chunk = co_await _file.writeSome(
					reinterpret_cast<const char *>(data) + progress,
					length - progress);
			progress += chunk;
		}
		co_return length;
	}
	
	async::result<frg::expected<Error, PollWaitResult>> pollWait(Process *,
			uint64_t sequence, int mask,
			async::cancellation_token cancellation = {}) override {
		auto resultOrError = co_await _file.pollWait(sequence, mask, cancellation);
		assert(resultOrError);
		co_return resultOrError.value();
	}
	
	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override {
		auto pollOverIpc = [this] ()
				-> async::result<frg::expected<Error, PollStatusResult>> {
			auto resultOrError = co_await _file.pollStatus();
			assert(resultOrError);
			co_return resultOrError.value();
		};

		if(!_statusMapping) {
			std::cout << "posix: No file status page. DeviceFile::pollStatus()"
					" falls back to slower IPC request" << std::endl;
			co_return co_await pollOverIpc();
		}

		auto page = reinterpret_cast<protocols::fs::StatusPage *>(_statusMapping.get());

		// Start the seqlock read.
		auto seqlock = __atomic_load_n(&page->seqlock, __ATOMIC_ACQUIRE);
		if(seqlock & 1) {
			if(logStatusSeqlock)
				std::cout << "posix: Status page update in progess;"
						" falling back to IPC request." << std::endl;
			co_return co_await pollOverIpc();
		}

		// Perform the actual loads.
		auto sequence = __atomic_load_n(&page->sequence, __ATOMIC_RELAXED);
		auto status = __atomic_load_n(&page->status, __ATOMIC_RELAXED);

		// Finish the seqlock read.
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if(__atomic_load_n(&page->seqlock, __ATOMIC_RELAXED) != seqlock) {
			if(logStatusSeqlock)
				std::cout << "posix: Stale data from status page;"
						" falling back to IPC request." << std::endl;
			co_return co_await pollOverIpc();
		}

		// TODO: Return a full edge mask or edges since sequence zero.
		co_return PollStatusResult{sequence, status};
	}

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		auto memory = co_await _file.accessMemory();
		co_return std::move(memory);
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _file.getLane();
	}

public:
	DeviceFile(helix::UniqueLane control, helix::UniqueLane lane,
			std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			helix::Mapping status_mapping)
	: File{StructName::get("devicefile"), std::move(mount), std::move(link)},
			_control{std::move(control)}, _file{std::move(lane)},
			_statusMapping{std::move(status_mapping)} { }

	~DeviceFile() {
		// It's not necessary to do any cleanup here.
	}

	void handleClose() override {
		// Close the control lane to inform the server that we closed the file.
		_control = helix::UniqueLane{};
	}

private:
	helix::UniqueLane _control;
	protocols::fs::File _file;
	helix::Mapping _statusMapping;
};

} // anonymous namespace

// --------------------------------------------------------
// External device helpers.
// --------------------------------------------------------

async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
openExternalDevice(helix::BorrowedLane lane,
		std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		SemanticFlags semantic_flags) {
	if(semantic_flags & ~(semanticNonBlock | semanticRead | semanticWrite)){
		std::cout << "\e[31mposix: openExternalDevice() received illegal arguments:"
			<< std::bitset<32>(semantic_flags)
			<< "\nOnly semanticNonBlock (0x1), semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
			<< std::endl;
		co_return Error::illegalArguments;
	}

	uint32_t open_flags = 0;
	if(semantic_flags & semanticNonBlock)
		open_flags |= managarm::fs::OpenFlags::OF_NONBLOCK;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::DEV_OPEN);
	req.set_flags(open_flags);

	auto ser = req.SerializeAsString();
	auto [offer, send_req, recv_resp, pull_pt, pull_page] = co_await helix_ng::exchangeMsgs(lane,
		helix_ng::offer(
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor(),
			helix_ng::pullDescriptor())
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_pt.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	helix::Mapping status_mapping;
	if(resp.caps() & managarm::fs::FileCaps::FC_STATUS_PAGE) {
		assert(!pull_page.error());
		status_mapping = helix::Mapping{pull_page.descriptor(), 0, 0x1000};
	}

	auto file = smarter::make_shared<DeviceFile>(helix::UniqueLane{},
			pull_pt.descriptor(), std::move(mount), std::move(link), std::move(status_mapping));
	file->setupWeakFile(file);
	co_return File::constructHandle(std::move(file));
}

FutureMaybe<std::shared_ptr<FsLink>> mountExternalDevice(helix::BorrowedLane lane) {
	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::DEV_MOUNT);

	auto ser = req.SerializeAsString();
	auto [offer, send_req, recv_resp, pull_node] = co_await helix_ng::exchangeMsgs(lane,
		helix_ng::offer(
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor())
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_node.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	co_return extern_fs::createRoot(lane.dup(), pull_node.descriptor());
}

