
#include <asm/ioctls.h>

#include "file.hpp"
#include "pts.hpp"
#include "fs.pb.h"

namespace pts {

namespace {

struct MasterFile;
struct RootLink;
struct DeviceNode;
struct RootNode;

int nextPtsIndex = 0;

extern std::shared_ptr<RootLink> globalRootLink;

//-----------------------------------------------------------------------------
// Device and file structs.
//-----------------------------------------------------------------------------

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

	expected<PollResult>
	poll(uint64_t sequence) override;

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

struct SlaveFile : File {
public:
	static void serve(smarter::shared_ptr<SlaveFile> file) {
		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane),
				smarter::shared_ptr<File>{file}, &File::fileOperations);
	}

	SlaveFile(std::shared_ptr<FsLink> link);

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	helix::UniqueLane _passthrough;
};

//-----------------------------------------------------------------------------
// File system structs.
//-----------------------------------------------------------------------------

struct Link : FsLink {
public:
	explicit Link(RootNode *root, std::string name, std::shared_ptr<DeviceNode> device)
	: _root{root}, _name{std::move(name)}, _device{std::move(device)} { }
	
	std::shared_ptr<FsNode> getOwner() override;

	std::string getName() override;

	std::shared_ptr<FsNode> getTarget() override;

private:
	RootNode *_root;
	std::string _name;
	std::shared_ptr<DeviceNode> _device;
};

struct RootLink : FsLink {
	RootLink();
	
	RootNode *rootNode() {
		return _root.get();
	}

	std::shared_ptr<FsNode> getOwner() override {
		throw std::logic_error("posix: pts RootLink has no owner");
	}

	std::string getName() override {
		throw std::logic_error("posix: pts RootLink has no name");
	}

	std::shared_ptr<FsNode> getTarget() override;

private:
	std::shared_ptr<RootNode> _root;
};

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
		return link->getName() < name;
	}
	bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
		return name < link->getName();
	}

	bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
		return a->getName() < b->getName();
	}
};

struct DeviceNode : FsNode {
public:
	DeviceNode(DeviceId id)
	: _type{VfsType::charDevice}, _id{id} { }

	VfsType getType() override {
		return _type;
	}
	
	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix pts DeviceNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

	DeviceId readDevice() override {
		return _id;
	}
	
	FutureMaybe<SharedFilePtr> open(std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openDevice(_type, _id, std::move(link), semantic_flags);
	}

private:
	VfsType _type;
	DeviceId _id;
};

struct RootNode : FsNode, std::enable_shared_from_this<RootNode> {
	friend struct Superblock;
	friend struct DirectoryFile;

public:
	VfsType getType() override {
		return VfsType::directory;
	}

	void linkDevice(std::string name, std::shared_ptr<DeviceNode> node) {
		auto link = std::make_shared<Link>(this, name, std::move(node));
		_entries.insert(std::move(link));
	}

	COFIBER_ROUTINE(FutureMaybe<FileStats>, getStats() override, ([=] {
		std::cout << "\e[31mposix: Fix pts RootNode::getStats()\e[39m" << std::endl;
		COFIBER_RETURN(FileStats{});
	}))

/*
	COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
	open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags), ([=] {
		assert(!semantic_flags);

		auto file = smarter::make_shared<DirectoryFile>(std::move(link));
		DirectoryFile::serve(file);
		COFIBER_RETURN(File::constructHandle(std::move(file)));
	}))
*/

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			getLink(std::string name) override, ([=] {
		auto it = _entries.find(name);
		if(it != _entries.end())
			COFIBER_RETURN(*it);
		COFIBER_RETURN(nullptr); // TODO: Return an error code.
	}))

private:
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
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

	globalRootLink->rootNode()->linkDevice(std::to_string(_ptsIndex),
			std::make_shared<DeviceNode>(_slaveDevice->getId()));
}

COFIBER_ROUTINE(expected<PollResult>, MasterFile::poll(uint64_t sequence), ([=] {
	std::cout << "posix: Fix pts MasterFile::poll()" << std::endl;
}))

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

SlaveFile::SlaveFile(std::shared_ptr<FsLink> link)
: File{StructName::get("pts.slave"), std::move(link)} { }

COFIBER_ROUTINE(FutureMaybe<SharedFilePtr>,
SlaveDevice::open(std::shared_ptr<FsLink> link, SemanticFlags semantic_flags), ([=] {
	assert(!semantic_flags);
	auto file = smarter::make_shared<SlaveFile>(std::move(link));
	SlaveFile::serve(file);
	COFIBER_RETURN(File::constructHandle(std::move(file)));
}))

//-----------------------------------------------------------------------------
// Link and RootLink implementation.
//-----------------------------------------------------------------------------

std::shared_ptr<FsNode> Link::getOwner() {
	return _root->shared_from_this();
}

std::string Link::getName() {
	return _name;
}

std::shared_ptr<FsNode> Link::getTarget() {
	return _device;
}

RootLink::RootLink()
: _root(std::make_shared<RootNode>()) { }

std::shared_ptr<FsNode> RootLink::getTarget() {
	return _root->shared_from_this();
}

std::shared_ptr<RootLink> globalRootLink = std::make_shared<RootLink>();

} // anonymous namespace

std::shared_ptr<UnixDevice> createMasterDevice() {
	return std::make_shared<MasterDevice>();
}

std::shared_ptr<FsLink> getFsRoot() {
	return globalRootLink;
}

} // namespace pts

