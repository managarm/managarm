#include <iostream>
#include <string.h>
#include <coroutine>
#include <queue>
#include <bitset>
#include <async/result.hpp>
#include <linux/fuse.h>

#include "common.hpp"
#include "fuse.hpp"

namespace fuse {

// This struct stores a chunk of data to read from FuseFile.
// Such a chunk can either be a struct from fuse.h, or raw data.
struct FusePacket {
private:
	unsigned char *m_data = nullptr;
	size_t m_size = 0;
	size_t m_read_bytes = 0;
public:
	size_t read(unsigned char *buffer, size_t nbytes) {
		size_t delta = m_size - m_read_bytes;
		if(nbytes < delta){
			memcpy(buffer, m_data + m_read_bytes, nbytes);
			m_read_bytes += nbytes;
			return nbytes;
		}
		memcpy(buffer, m_data + m_read_bytes, delta);
		m_read_bytes = m_size;
		return delta;
	}

	bool all_read() {
		return m_size == m_read_bytes;
	}
	
	FusePacket(void *data, size_t size) {
		m_data = (unsigned char*)calloc(size, sizeof(unsigned char));
		memcpy(m_data, data, size);
		m_size = size;
	}

	~FusePacket() {
		free(m_data);
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

	size_t read(unsigned char *buffer, size_t nbytes) {
		size_t total_read = 0;
		size_t nbytes_to_read = nbytes;
		while(total_read < nbytes && !m_queue.empty()) {
			auto packet = m_queue.front();
			auto read_bytes = packet.read(buffer + total_read, nbytes_to_read);
			nbytes_to_read -= read_bytes;
			total_read += read_bytes;
			if(packet.all_read()) {
				m_queue.pop();
			}
		}
		return total_read;
	}

	void save(FusePacket packet) {
		m_queue.push(packet);
	}
};

struct FuseFile final : File {
private:
	FuseQueue m_queue;
	bool m_setup_complete = false;
	size_t m_unique = 0;

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t length) override {
		size_t read_count = m_queue.read((unsigned char*)data, length);
		co_return read_count;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *process, const void *data, size_t length) override {
		if(length < sizeof(fuse_out_header)){
			co_return Error::illegalArguments;
		}

		co_return Error::success;
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

		m_queue.save(FusePacket(&header, sizeof(fuse_in_header)));
		m_queue.save(FusePacket(&init, sizeof(fuse_init_in)));
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

} // namespace fuse
