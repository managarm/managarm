
#include <string.h>

#include <protocols/fs/server.hpp>
#include "common.hpp"
#include "device.hpp"
#include "sysfs.hpp"

namespace {

// ----------------------------------------------------------------------------
// FS data structures.
// ----------------------------------------------------------------------------

struct DirectoryNode;
struct Link;
struct LinkCompare;

struct LinkCompare {
	struct is_transparent { };

	bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const;
	bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const;
	bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const;
};

struct DirectoryFile : ProperFile {
private:
	static async::result<protocols::fs::ReadEntriesResult>
	ptReadEntries(std::shared_ptr<void> object);

	FutureMaybe<size_t> readSome(void *data, size_t max_length) override {
		throw std::runtime_error("sysfs: DirectoryFile::readSome() is missing");
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

public:
	static constexpr auto fileOperations = protocols::fs::FileOperations{}
		.withReadEntries(&ptReadEntries);

public:
	static void serve(std::shared_ptr<DirectoryFile> file) {
//TODO:		assert(!file->_passthrough);

		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		protocols::fs::servePassthrough(std::move(lane), file,
				&fileOperations);
	}

	DirectoryFile(std::shared_ptr<FsLink> link);

private:
	DirectoryNode *_node;

	helix::UniqueLane _passthrough;
	std::set<std::shared_ptr<Link>, LinkCompare>::iterator _iter;
};

struct Link : FsLink {
private:
	std::shared_ptr<FsNode> getOwner() override {
		return owner;
	}

	std::string getName() override {
		return name;
	}

	std::shared_ptr<FsNode> getTarget() override {
		return target;
	}

public:
	explicit Link(std::shared_ptr<FsNode> owner, std::string name, std::shared_ptr<FsNode> target)
	: owner(std::move(owner)), name(std::move(name)), target(std::move(target)) { }

	std::shared_ptr<FsNode> owner;
	std::string name;
	std::shared_ptr<FsNode> target;
};

struct DirectoryNode : FsNode, std::enable_shared_from_this<DirectoryNode> {
	friend struct DirectoryFile;
private:
	VfsType getType() override {
		return VfsType::directory;
	}

	FileStats getStats() override {
		std::cout << "\e[31mposix: Fix sysfs Directory::getStats()\e[39m" << std::endl;
		return FileStats{};
	}
	
	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<ProperFile>>,
			open(std::shared_ptr<FsLink> link) override, ([=] {
		auto file = std::make_shared<DirectoryFile>(std::move(link));
		DirectoryFile::serve(file);
		COFIBER_RETURN(std::move(file));
	}))

	COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<FsLink>>,
			getLink(std::string name) override, ([=] {
		auto it = _entries.find(name);
		if(it != _entries.end())
			COFIBER_RETURN(*it);
		COFIBER_RETURN(nullptr); // TODO: Return an error code.
	}))

public:
	DirectoryNode() = default;

	std::shared_ptr<FsLink> directMkdir(std::string name) {
		assert(_entries.find(name) == _entries.end());
		auto node = std::make_shared<DirectoryNode>();
		auto link = std::make_shared<Link>(shared_from_this(), std::move(name), std::move(node));
		_entries.insert(link);
		return link;
	}

private:
	std::set<std::shared_ptr<Link>, LinkCompare> _entries;
};

// ----------------------------------------------------------------------------
// LinkCompare implementation.
// ----------------------------------------------------------------------------

bool LinkCompare::operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
	return a->name < b->name;
}

bool LinkCompare::operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
	return link->name < name;
}

bool LinkCompare::operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
	return name < link->name;
}

// ----------------------------------------------------------------------------
// DirectoryFile implementation.
// ----------------------------------------------------------------------------

// TODO: This iteration mechanism only works as long as _iter is not concurrently deleted.
COFIBER_ROUTINE(async::result<protocols::fs::ReadEntriesResult>,
DirectoryFile::ptReadEntries(std::shared_ptr<void> object), ([=] {
	auto self = static_cast<DirectoryFile *>(object.get());
	if(self->_iter != self->_node->_entries.end()) {
		auto name = (*self->_iter)->name;
		self->_iter++;
		COFIBER_RETURN(name);
	}else{
		COFIBER_RETURN(std::nullopt);
	}
}))

DirectoryFile::DirectoryFile(std::shared_ptr<FsLink> link)
: ProperFile{std::move(link)},
		_node{static_cast<DirectoryNode *>(associatedLink()->getTarget().get())},
		_iter{_node->_entries.begin()} { }

// ----------------------------------------------------------------------------
// Free functions.
// ----------------------------------------------------------------------------

std::shared_ptr<FsLink> createRoot() {
	auto node = std::make_shared<DirectoryNode>();
	return std::make_shared<Link>(nullptr, std::string{}, std::move(node));
}

} // anonymous namespace

std::shared_ptr<FsLink> getSysfs() {
	static std::shared_ptr<FsLink> sysfs = createRoot();
	return sysfs;
}

std::shared_ptr<FsLink> sysfsMkdir(FsNode *ptr, std::string name) {
	auto node = static_cast<DirectoryNode *>(ptr);
	return node->directMkdir(std::move(name));
}

