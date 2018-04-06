
#include <asm/ioctls.h>

#include "file.hpp"
#include "pts.hpp"
#include "fs.pb.h"

namespace pts {

namespace {

struct MasterFile;

int nextPtsIndex = 0;

struct MasterDevice : UnixDevice {
	MasterDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({5, 2});
	}
	
	std::string getName() override {
		return "ptmx";
	}
	
	async::result<SharedFilePtr>
	open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override;
};

struct SlaveDevice : UnixDevice {
	SlaveDevice(MasterFile *master_file);
	
	std::string getName() override {
		return std::string{};
	}
	
	async::result<SharedFilePtr>
	open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override;

private:
	MasterFile *_masterFile;
};

struct MasterFile : File {
public:
	static void serve(smarter::shared_ptr<MasterFile> file) {
		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations);
	}

	MasterFile(std::shared_ptr<FsLink> link);

	int ptsIndex() {
		return _ptsIndex;
	}

	async::result<void>
	ioctl(Process *process, managarm::fs::CntRequest req, helix::UniqueLane conversation);

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;

	int _ptsIndex;

	std::shared_ptr<SlaveDevice> _slaveDevice;
};

//-----------------------------------------------------------------------------
// MasterDevice implementation.
//-----------------------------------------------------------------------------

COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
MasterDevice::open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags), ([=] {
	assert(!semantic_flags);
	auto file = smarter::make_shared<MasterFile>(std::move(link));
	MasterFile::serve(file);
	COFIBER_RETURN(File::constructHandle(std::move(file)));
}))

MasterFile::MasterFile(std::shared_ptr<FsLink> link)
: File{StructName::get("pts.master"), std::move(link)},
		_slaveDevice{std::make_shared<SlaveDevice>(this)} {
	_ptsIndex = nextPtsIndex++;
	charRegistry.install(_slaveDevice);
}

COFIBER_ROUTINE(async::result<void>, MasterFile::ioctl(Process *, managarm::fs::CntRequest req,
		helix::UniqueLane conversation), ([this, req = std::move(req),
			conversation = std::move(conversation)] {
	if(req.command() == TIOCGPTN) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;
	
		resp.set_error(managarm::fs::Errors::SUCCESS);
		resp.set_pts_index(_ptsIndex);
		
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else{
		throw std::runtime_error("posix: Unknown ioctl() with ID " + std::to_string(req.command()));
	}
	COFIBER_RETURN();
}))

//-----------------------------------------------------------------------------
// SlaveDevice implementation.
//-----------------------------------------------------------------------------

SlaveDevice::SlaveDevice(MasterFile *master_file)
: UnixDevice(VfsType::charDevice), _masterFile{master_file} {
	assignId({136, _masterFile->ptsIndex()});
}

COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
SlaveDevice::open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags), ([=] {
	assert(!"Fix this");
}))

} // anonymous namespace

std::shared_ptr<UnixDevice> createMasterDevice() {
	return std::make_shared<MasterDevice>();
}

} // namespace pts

