#include <iostream>
#include <memory>
#include <string.h>
#include <coroutine>
#include <queue>
#include <span>
#include <algorithm>
#include <bitset>
#include <optional>
#include <async/result.hpp>
#include <linux/fuse.h>

#include "common.hpp"
#include "process.hpp"
#include "file.hpp"
#include "fuse.hpp"

namespace fuse {

// This struct stores a chunk of data to read from FuseFile.
// Such a chunk can either be a struct from fuse.h, or raw data.
struct FusePacket {
private:
	std::vector<unsigned char> m_data;
	size_t m_read_bytes = 0;
public:
	size_t read(std::span<unsigned char> buffer) {
		size_t delta = m_data.size() - m_read_bytes;
		size_t new_read_bytes = m_data.size();
		size_t byte_count = delta;
		if(buffer.size() < delta) {
			new_read_bytes = m_read_bytes + buffer.size();
			byte_count = buffer.size();
		}
		std::copy(m_data.begin() + m_read_bytes, m_data.begin() + new_read_bytes, buffer.begin());
		m_read_bytes = new_read_bytes;
		return byte_count;
	}

	bool all_read() {
		return m_data.size() == m_read_bytes;
	}
	
	FusePacket(std::vector<unsigned char> data) {
		m_data = data;
	}
};

// This struct stores a queue of FUSE data chunks. This data is stored
// in a queue so that incomplete reads do not prematurely discard
// a FusePacket. Moreover, it provides a way to read data from the next
// FusePacket if necessary.
struct FuseQueue {
private:
	std::queue<FusePacket> m_queue;
	size_t m_unique = 0;
public:
	size_t get_unique() {
		return m_unique++;
	}

	size_t read(std::span<unsigned char> buffer) {
		size_t total_read = 0;
		while(total_read < buffer.size() && !m_queue.empty()) {
			auto& packet = m_queue.front();
			total_read += packet.read(buffer.subspan(total_read));
			if(packet.all_read()) {
				m_queue.pop();
			}
		}
		return total_read;
	}

	void save(std::vector<unsigned char> data) {
		m_queue.emplace(data);
	}
};

struct FuseFile final : File {
private:
	FuseQueue m_queue;
	bool m_setup_complete = false;
	size_t m_unique = 0;

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t length) override {
		size_t read_count = m_queue.read(std::span((unsigned char*)data, length));
		co_return read_count;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *process, const void *data, size_t length) override {
		co_return Error::noSpaceLeft;
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

public:
	static void serve(smarter::shared_ptr<FuseFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	FuseFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("fuse-file"), std::move(mount), std::move(link)} {
		fuse_in_header header = {
			.len = sizeof(fuse_in_header) + sizeof(fuse_init_in),
			.opcode = FUSE_INIT,
			.unique = m_queue.get_unique(),
		};

		fuse_init_in init = {
			.major = FUSE_KERNEL_VERSION,
			.minor = FUSE_KERNEL_MINOR_VERSION,
		};

		m_queue.save(std::vector((unsigned char*)&header, (unsigned char*)&header + sizeof(fuse_in_header)));
		m_queue.save(std::vector((unsigned char*)&init, (unsigned char*)&init + sizeof(fuse_init_in)));
	}
};

struct FuseDevice final : UnixDevice {
	FuseDevice()
	: UnixDevice(VfsType::charDevice) {
		assignId({10, 229});
	}

	std::string nodePath() override {
		return "fuse";
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		if(semantic_flags & ~(semanticRead | semanticWrite)){
			std::cout << "\e[31mposix: open() received illegal arguments:"
				<< std::bitset<32>(semantic_flags)
				<< "\nOnly semanticRead (0x2) and semanticWrite(0x4) are allowed.\e[39m"
				<< std::endl;
			co_return Error::illegalArguments;
		}

		auto file = smarter::make_shared<FuseFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		FuseFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

std::shared_ptr<UnixDevice> createFuseDevice() {
	return std::make_shared<FuseDevice>();
}

struct FuseNode : FsNode {
private:
	smarter::shared_ptr<File, FileHandle> m_fuse_file;
public:
	async::result<VfsType> getType() override {
	    co_return VfsType::directory;
	}

	FuseNode(smarter::shared_ptr<File, FileHandle> fuse_file)
	: FsNode(getAnonymousSuperblock()) {
		m_fuse_file = fuse_file;
	}
};

struct FuseRootNode final : FuseNode, std::enable_shared_from_this<FuseRootNode> {
	using FuseNode::FuseNode;
};

struct FuseRootLink final : FsLink {
private:
	std::shared_ptr<FuseRootNode> m_root;
public:
	FuseRootLink(std::shared_ptr<FuseRootNode> root) {
		m_root = root;
	}

	std::shared_ptr<FsNode> getOwner() override {
		throw std::logic_error("posix: FUSE RootLink has no owner");
	}

	std::string getName() override {
		throw std::logic_error("posix: FUSE RootLink has no name");
	}

	std::shared_ptr<FsNode> getTarget() override {
		return m_root->shared_from_this();
	}
};

struct FuseSettings {
private:
	int m_fd;
	bool m_has_fd;
public:
	void set_fd(int fd) {
		m_fd = fd;
		m_has_fd = true;
	}

	int fd() {
		return m_fd;
	}

	bool has_fd() {
		return m_has_fd;
	}
};

std::optional<FuseSettings> parseArguments(std::string arguments) {
	std::vector<std::string> split_args;
	std::string arg;

	for(auto c : arguments) {
		if(c == ','){
			if(arg.size()){
				split_args.push_back(arg);
				arg = "";
			}
			continue;
		}
		arg.push_back(c);
	}

	if(arg.size()) {
		split_args.push_back(arg);
	}

	if(!split_args.size()) {
		return {};
	}

	FuseSettings settings;
	for(auto s : split_args){
		if(s.substr(0, 3) == "fd=") {
			std::string num = s.substr(3);
			try {
				size_t pos = 0;
				settings.set_fd(std::stoi(num, &pos));
				if(pos != num.size()){
					return {};
				}
			}
			catch(const std::exception &e) {
				std::cout << "posix: " << e.what() << std::endl;
				return {};
			}
		}
		else {
			continue; //TODO: handle other arguments
		}
	}
	return settings;
}

std::optional<std::shared_ptr<FsLink>> getFsRoot(std::shared_ptr<Process> proc, std::string arguments) {
	auto o_settings = parseArguments(arguments);
	if(!o_settings.has_value()) {
		return {};
	}
	FuseSettings settings = *o_settings;
	if(!settings.has_fd()) {
		return {};
	}
	auto file = proc->fileContext()->getFile(settings.fd());
	if(file.get() == nullptr || file->structName().type() != "fuse-file") {
		return {};
	}
	return std::make_shared<FuseRootLink>(std::make_shared<FuseRootNode>(file));
}

} // namespace fuse
