
#include <set>

#include <protocols/fs/client.hpp>
#include "common.hpp"
#include "tmp_fs.hpp"

namespace tmp_fs {

namespace {

/*struct OpenFile {
	OpenFile(int fd)
	: _file(helix::UniqueDescriptor(__mlibc_getPassthrough(fd))) { }

	COFIBER_ROUTINE(FutureMaybe<off_t>, seek(off_t offset, VfsSeek whence), ([=] {
		assert(whence == VfsSeek::absolute);
		COFIBER_AWAIT _file.seekAbsolute(offset);
		COFIBER_RETURN(offset);
	}))

	COFIBER_ROUTINE(FutureMaybe<size_t>, readSome(void *data, size_t max_length), ([=] {
		size_t length = COFIBER_AWAIT _file.readSome(data, max_length);
		COFIBER_RETURN(length);
	}))

	COFIBER_ROUTINE(FutureMaybe<helix::UniqueDescriptor>, accessMemory(), ([=] {
		auto memory = COFIBER_AWAIT _file.accessMemory();
		COFIBER_RETURN(std::move(memory));
	}))

	helix::BorrowedDescriptor getPassthroughLane() {
		return _file.getLane();
	}

private:
	protocols::fs::File _file;
};

struct RegularNode {
	RegularNode(int fd)
	: _fd(fd) { }

	COFIBER_ROUTINE(FutureMaybe<SharedFile>, open(SharedEntry entry), ([=] {
		COFIBER_RETURN(SharedFile::create<OpenFile>(std::move(entry), _fd));
	}))

private:
	int _fd;
};*/

struct Symlink : SymlinkData {
	Symlink(std::string link)
	: _link(std::move(link)) { }
	
	COFIBER_ROUTINE(FutureMaybe<std::string>, readSymlink(), ([=] {
		COFIBER_RETURN(_link);
	}))

private:
	std::string _link;
};

struct Directory : TreeData {
	COFIBER_ROUTINE(FutureMaybe<SharedLink>, getLink(std::string name), ([=] {
		auto it = _entries.find(name);
		assert(it != _entries.end());
		COFIBER_RETURN(SharedLink{*it});
	}))

	COFIBER_ROUTINE(FutureMaybe<SharedLink>, mkdir(std::string name), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = SharedNode{std::make_shared<Directory>()};
		auto link = std::make_shared<Link>(SharedNode{}, std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(SharedLink{link});
	}))
	
	COFIBER_ROUTINE(FutureMaybe<SharedLink>, symlink(std::string name, std::string path), ([=] {
		assert(_entries.find(name) == _entries.end());
		auto node = SharedNode{std::make_shared<Symlink>(std::move(path))};
		auto link = std::make_shared<Link>(SharedNode{}, std::move(name), std::move(node));
		_entries.insert(link);
		COFIBER_RETURN(SharedLink{link});
	}))

private:
	struct Link : LinkData {
		explicit Link(SharedNode owner, std::string name, SharedNode target)
		: owner(std::move(owner)), name(std::move(name)), target(std::move(target)) { }

		SharedNode getOwner() override {
			return owner;
		}

		std::string getName() override {
			return name;
		}

		SharedNode getTarget() override {
			return target;
		}

		SharedNode owner;
		std::string name;
		SharedNode target;
	};

	struct Compare {
		struct is_transparent { };

		bool operator() (const std::shared_ptr<Link> &link, const std::string &name) const {
			return link->name < name;
		}
		bool operator() (const std::string &name, const std::shared_ptr<Link> &link) const {
			return name < link->name;
		}

		bool operator() (const std::shared_ptr<Link> &a, const std::shared_ptr<Link> &b) const {
			return a->name < b->name;
		}
	};

	std::set<std::shared_ptr<Link>, Compare> _entries;
};

} // anonymous namespace

SharedLink createRoot() {
	auto node = SharedNode{std::make_shared<Directory>()};
	return SharedLink::createRoot(std::move(node));
}

} // namespace tmp_fs

