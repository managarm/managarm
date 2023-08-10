#include <abi-bits/fcntl.h>
#include <abi-bits/seek-whence.h>
#include <async/basic.hpp>
#include <bits/size_t.h>
#include <boost/mpl/assert.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string.h>
#include <coroutine>
#include <queue>
#include <smarter.hpp>
#include <span>
#include <algorithm>
#include <string>
#include <type_traits>
#include <bitset>
#include <optional>
#include <concepts>
#include <map>
#include <async/result.hpp>
#include <async/recurring-event.hpp>
#include <linux/fuse.h>
#include <sys/stat.h>
#include <poll.h>
#include <string.h>

#include "common.hpp"
#include "frg/expected.hpp"
#include "helix/ipc-structs.hpp"
#include "helix/ipc.hpp"
#include "process.hpp"
#include "file.hpp"
#include "fuse.hpp"
#include "protocols/fs/common.hpp"
#include "protocols/fs/server.hpp"
#include "src/fs.hpp"
#include "src/vfs.hpp"

namespace fuse {

struct FuseDeviceFile;
struct FuseNode;
struct FuseFile;

using WriteInData = std::pair<uint64_t, std::span<unsigned char>>;
using Request = std::vector<unsigned char>;

template <typename T>
std::span<unsigned char> structToSpan(T *base) {
	return std::span((unsigned char*)base, sizeof(T));
}

template <typename T, typename S>
concept StructSpan = requires(T data, S span) {
	std::is_base_of<std::span<unsigned char>, S>();
	span.size() == sizeof(T);
};

template <typename T>
void copyStructFromSpan(T *base, std::span<unsigned char> span) {
	auto struct_span = structToSpan(base);
	std::copy(span.begin(), span.begin() + sizeof(T), struct_span.begin());
}

template <typename T>
Request requestToVector(fuse_in_header *head, T *data) {
	auto head_span = structToSpan(head);
	auto data_span = structToSpan(data);
	Request req(head_span.begin(), head_span.end());
	req.insert(req.end(), data_span.begin(), data_span.end());
	return req;
}

template <>
Request requestToVector<std::string>(fuse_in_header *head, std::string *data) {
	auto head_span = structToSpan(head);
	Request req(head_span.begin(), head_span.end());
	req.insert(req.end(), data->begin(), data->end());
	req.insert(req.end(), (unsigned char){'\0'});
	return req;
}

template<typename T1, typename T2>
Request requestToVector(fuse_in_header *head, T1 *data1, T2 *data2) {
	auto req = requestToVector(head, data1);
	auto data2_span = structToSpan(data2);
	req.insert(req.end(), data2_span.begin(), data2_span.end());
	return req;
}

template<typename T>
Request requestToVector(fuse_in_header *head, T *data1, std::string *data2) {
	auto req = requestToVector(head, data1);
	req.insert(req.end(), data2->begin(), data2->end());
	req.insert(req.end(), (unsigned char){'\0'});
	return req;
}

// This struct stores a chunk of data to read from FuseDeviceFile.
// Such a chunk can either be a struct from fuse.h, or raw data.
struct FusePacket {
private:
	Request m_data;
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

	FusePacket(Request data) {
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

	size_t size() {
		return m_queue.size();
	}
};

//Provide functionality for /dev/fuse
struct FuseDeviceFile final : File {
private:
	friend FuseNode;
	friend FuseFile;

	helix::UniqueLane m_passthrough;
	async::cancellation_event m_cancel_serve;

	bool m_mounted = false;
	FuseQueue m_queue;
	bool m_setup_complete = false;
	int m_poll_events = POLLOUT | POLLWRNORM; //these events are always active
	uint64_t m_current_sequence = 0;
	async::recurring_event m_change_event;
	async::recurring_event m_write_event;
	async::recurring_event m_read_ready_event;
	std::map<uint64_t, ssize_t> m_waiting_requests;
	std::map<uint64_t, std::span<unsigned char>> m_active_requests;

	void set_event(int event) {
		if(!(m_poll_events & event)) {
			m_current_sequence++;
		}

		m_poll_events |= event;
		m_change_event.raise();
	}

	void unset_event(int event) {
		m_poll_events &= ~event;
	}

	async::result<frg::expected<Error, size_t>>
	readSome(Process *, void *data, size_t length) override {
		if(!m_mounted)
			co_return Error::insufficientPermissions;

		//necessary to wait data ready for reading without hanging
		if(!m_queue.size()) {
			co_await m_read_ready_event.async_wait();
		}

		size_t read_count = m_queue.read(std::span((unsigned char*)data, length));
		std::cout << "posix: FUSE read_count = " << read_count << std::endl;
		if(!m_queue.size()) {
			unset_event(POLLIN);
		}
		co_return read_count;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *, const void *data, size_t length) override {
		if(!m_mounted)
			co_return Error::insufficientPermissions;

		if(length < sizeof(fuse_out_header)) {
			co_return Error::illegalArguments;
		}

		fuse_out_header header;
		auto data_span = std::span((unsigned char*)data, length);
		copyStructFromSpan(&header, data_span);

		if(header.len != length || (header.error && header.len > sizeof(fuse_out_header)) ||
			!m_waiting_requests.contains(header.unique) ||
			(m_waiting_requests[header.unique] != -1 && m_waiting_requests[header.unique] != length && !header.error)) {
			std::cout << "posix: header.len				 = " << header.len << std::endl;
			std::cout << "posix: length					 = " << length << std::endl;
			std::cout << "posix: header.error			 = " << header.error << std::endl;
			std::cout << "posix: strerror(header.error)	 = " << strerror(header.error * -1) << std::endl;
			std::cout << "posix: sizeof(fuse_out_header) = " << sizeof(fuse_out_header) << std::endl;
			co_return Error::illegalArguments;
		}

		if(!m_setup_complete) {
			if(length == sizeof(fuse_out_header) + sizeof(uint32_t)) {
				co_return Error::noSpaceLeft; //TODO: version negotiation
			}

			//TODO: actually use flags passed by daemon
			m_waiting_requests.erase(header.unique);
			m_setup_complete = true;
			co_return length;
		}

		assert(!m_active_requests.contains(header.unique)); //TOO: report error instead
		m_active_requests[header.unique] = data_span;
		m_write_event.raise();
		co_return length;
	}

	async::result<std::span<unsigned char>>
	performRequest(std::vector<unsigned char> request, uint64_t unique, ssize_t expected_size) {
		m_queue.save(request);
		m_waiting_requests[unique] = expected_size;
		m_read_ready_event.raise();
		while(!m_active_requests.contains(unique)) {
			co_await m_write_event.async_wait();
		}

		auto out = m_active_requests[unique];
		m_active_requests.erase(unique);
		m_waiting_requests.erase(unique);
		co_return out;
	}

public:
	helix::BorrowedDescriptor getPassthroughLane() override {
		return m_passthrough;
	}

	async::result<frg::expected<Error, PollWaitResult>>
	pollWait(Process *, uint64_t sequence, int mask, async::cancellation_token cancellation) override {
		uint64_t old_sequence = m_current_sequence;
		while(old_sequence == m_current_sequence && !(mask & m_poll_events)) {
			co_await m_change_event.async_wait();
		}
		co_return PollWaitResult{m_current_sequence, m_current_sequence - sequence};
	}

	async::result<frg::expected<Error, PollStatusResult>> pollStatus(Process *) override {
		co_return PollStatusResult{m_current_sequence, m_poll_events};
	}

	static void serve(smarter::shared_ptr<FuseDeviceFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->m_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->m_cancel_serve));
	}

	FuseDeviceFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link)
	: File{StructName::get("fuse-file"), std::move(mount), std::move(link)} {
		size_t unique = m_queue.get_unique();
		fuse_in_header header = {
			.len = sizeof(fuse_in_header) + sizeof(fuse_init_in),
			.opcode = FUSE_INIT,
			.unique = unique,
		};

		fuse_init_in init = {
			.major = FUSE_KERNEL_VERSION,
			.minor = FUSE_KERNEL_MINOR_VERSION,
		};

		auto vec = requestToVector(&header, &init);
		m_queue.save(vec);
		m_waiting_requests[unique] = -1;
		set_event(POLLIN);
	}

	void set_mounted() {
		m_mounted = true;
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

		auto file = smarter::make_shared<FuseDeviceFile>(std::move(mount), std::move(link));
		file->setupWeakFile(file);
		FuseDeviceFile::serve(file);
		co_return File::constructHandle(std::move(file));
	}
};

std::shared_ptr<UnixDevice> createFuseDevice() {
	return std::make_shared<FuseDevice>();
}

//Map VFS operations to FUSE actions
struct FuseFile : File {
private:
	helix::UniqueLane m_passthrough;
	async::cancellation_event m_cancel_serve;

	smarter::shared_ptr<FuseDeviceFile, FileHandle> m_fuse_file;
	uint64_t m_fh;
	uint64_t m_offset;

public:
	helix::BorrowedDescriptor getPassthroughLane() override {
		return m_passthrough;
	}

	static void serve(smarter::shared_ptr<FuseFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->m_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->m_cancel_serve));
	}

	void handleClose() override {
		m_cancel_serve.cancel();
	}

	FuseFile(StructName struct_name, std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
		smarter::shared_ptr<FuseDeviceFile, FileHandle> fuse_file, uint64_t fh, DefaultOps default_ops = 0)
	: File(struct_name, mount, link, default_ops), m_fuse_file{fuse_file}, m_fh{fh} { }

	async::result<frg::expected<Error, off_t>>
	seek(off_t offset, VfsSeek whence) override {
		std::cout << "FuseFile::seek()" << std::endl;
		auto link = associatedLink();
		auto node = link->getTarget();
		auto maybe_stats = co_await node->getStats();
		if(!maybe_stats) {
			std::cout << "FuseFile::seek(): getStats() failed" << std::endl;
			co_return maybe_stats.error();
		}

		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_lseek_in)),
			.opcode = FUSE_LSEEK,
			.unique = unique,
			.nodeid = maybe_stats.unwrap().inodeNumber,
		};
		fuse_lseek_in data_in = {
			.fh = m_fh,
			.offset = static_cast<uint64_t>(offset),
		};
		if(whence == VfsSeek::absolute) {
			data_in.whence = SEEK_SET;
		}
		else if(whence == VfsSeek::relative) {
			data_in.whence = SEEK_CUR;
		}
		else if(whence == VfsSeek::eof) {
			data_in.whence = SEEK_END;
		}
		else {
			std::cout << "FuseFile::seek() does not support this whence value" << std::endl;
			co_return Error::illegalArguments;
		}

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header) + sizeof(fuse_lseek_out));
		fuse_out_header head_out;
		fuse_lseek_out data_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::illegalArguments; //TODO: map
		}

		copyStructFromSpan(&data_out, out.subspan(sizeof(fuse_out_header)));
		m_offset = data_out.offset;
		co_return static_cast<off_t>(data_out.offset);
	}

	async::result<frg::expected<Error, size_t>>
	readSome(Process *process, void *data, size_t length) override {
		auto result = co_await pread(process, m_offset, data, length);
		if(result) {
			m_offset += result.unwrap();
		}

		co_return result;
	}

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *process, const void *data, size_t length) override {
		auto result = co_await pwrite(process, m_offset, data, length);
		if(result) {
			m_offset += result.unwrap();
		}

		co_return result;
	}

	async::result<frg::expected<Error, size_t>>
	pread(Process *, int64_t offset, void *buffer, size_t length) override {
		std::cout << "FuseFile::pread()" << std::endl;
		auto link = associatedLink();
		auto node = link->getTarget();
		auto maybe_stats = co_await node->getStats();
		if(!maybe_stats) {
			std::cout << "FuseFile::pread(): getStats() failed" << std::endl;
			co_return maybe_stats.error();
		}

		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_read_in)),
			.opcode = FUSE_READ,
			.unique = unique,
			.nodeid = maybe_stats.unwrap().inodeNumber,
		};
		fuse_read_in data_in = {
			.fh = m_fh,
			.offset = static_cast<uint64_t>(offset),
			.size = static_cast<uint32_t>(length),
		};

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, -1);
		fuse_out_header head_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied; //TODO: map
		}

		auto data_length = head_out.len - sizeof(fuse_out_header);
		auto buf = std::span<unsigned char>(static_cast<unsigned char *>(buffer), data_length);
		auto read_data = out.subspan(sizeof(fuse_out_header));
		std::copy(read_data.begin(), read_data.end(), buf.begin());

		std::cout << "FuseFile::pread() read length: " << data_length << std::endl;
		std::cout << "FuseFile::pread() read data  : " << std::string(read_data.begin(), read_data.end()) << std::endl;
		co_return data_length;
	}

	async::result<frg::expected<Error, size_t>>
	pwrite(Process *, int64_t offset, const void *buffer, size_t length) override {
		auto link = associatedLink();
		auto node = link->getTarget();
		auto maybe_stats = co_await node->getStats();
		if(!maybe_stats) {
			std::cout << "FuseFile::pwrite() : getStats() failed" << std::endl;
			co_return maybe_stats.error();
		}

		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + length),
			.opcode = FUSE_WRITE,
			.unique = unique,
			.nodeid = maybe_stats.unwrap().inodeNumber,
		};
		fuse_write_in data_in = {
			.fh = m_fh,
			.offset = static_cast<uint64_t>(offset),
			.size = static_cast<uint32_t>(length),
		};

		auto write_data = std::span(static_cast<const unsigned char*>(buffer), length);
		auto request = requestToVector(&head_in, &data_in);
		std::copy(write_data.begin(), write_data.end(), request.end());
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header) + sizeof(fuse_write_out));
		fuse_out_header head_out;
		fuse_write_out data_out;

		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::illegalArguments;
		}

		copyStructFromSpan(&data_out, out.subspan(sizeof(fuse_out_header)));
		co_return data_out.size;
	}

	FutureMaybe<ReadEntriesResult> readEntries() override {
		auto link = associatedLink();
		auto node = link->getTarget();
		auto maybe_stats = co_await node->getStats();
		if(!maybe_stats) {
			std::cout << "FuseFile::readEntries() : getStats() failed" << std::endl;
			co_return {};
		}

		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_read_in)),
			.opcode = FUSE_READDIRPLUS,
			.unique = unique,
			.nodeid = maybe_stats.unwrap().inodeNumber,
		};
		fuse_read_in data_in = {
			.fh = m_fh,
			.offset = static_cast<uint64_t>(m_offset),
			.size = 4096, //magic constant lifted from linux
		};

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, -1);
		fuse_out_header head_out;
		fuse_direntplus data_out;

		copyStructFromSpan(&head_out, out);

		std::cout << "readEntries() : sizeof(fuse_out_header): " << sizeof(fuse_out_header) << std::endl;
		std::cout << "readEntries() : sizeof(fuse_direntplus): " << sizeof(fuse_direntplus) << std::endl;
		std::cout << "readEntires() : head_out.len           : " << head_out.len << std::endl;

		if(head_out.error) {
			std::cout << "posix: FuseFile::readEntries() encountered an error" << std::endl;
			co_return {};
		}
		else if(head_out.len < sizeof(fuse_out_header) + sizeof(fuse_direntplus)) {
			std::cout << "posix: FuseFile::readEntries(): no more entries to read" << std::endl;
			co_return {};
		}

		m_offset++; //read offset tracks entries, not bytes
		copyStructFromSpan(&data_out, out.subspan(sizeof(fuse_out_header)));
		auto first = out.begin() + sizeof(fuse_out_header) + FUSE_NAME_OFFSET_DIRENTPLUS;
		auto last = out.begin() + sizeof(fuse_out_header) + FUSE_DIRENTPLUS_SIZE(&data_out);
		co_return std::string(first, last);
	}

	async::result<frg::expected<protocols::fs::Error>>
	allocate(int64_t offset, size_t size) override {
		auto link = associatedLink();
		auto node = link->getTarget();
		auto maybe_stats = co_await node->getStats();
		if(!maybe_stats) {
			std::cout << "FuseFile::allocate() : getStats() failed" << std::endl;
			co_return protocols::fs::Error::noSpaceLeft; //TODO: map
		}

		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_fallocate_in)),
			.opcode = FUSE_FALLOCATE,
			.unique = unique,
			.nodeid = maybe_stats.unwrap().inodeNumber,
		};
		fuse_fallocate_in data_in = {
			.fh = m_fh,
			.offset = static_cast<uint64_t>(offset),
			.length = static_cast<uint64_t>(size),
		};

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header));
		fuse_out_header head_out;

		copyStructFromSpan(&head_out, out);
		if(head_out.error) {
			co_return protocols::fs::Error::noSpaceLeft; //TODO: map
		}

		co_return frg::success_tag{};
	}

	expected<PollResult>
	poll(Process *, uint64_t sequence, async::cancellation_token cancellation = {}) override {
	}

	async::result<void> ioctl(Process *, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) override {
	}

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override {
		std::cout << "FuseFile::accessMemory() is not implemented" << std::endl;
		throw std::runtime_error("posix: Object has no File::accessMemory()");
	}
};

struct FuseLink : FsLink {
private:
	std::shared_ptr<FsNode> m_target;
	std::string m_name;
	std::shared_ptr<FsNode> m_owner;
public:
	FuseLink(std::shared_ptr<FuseNode> owner, std::string name, std::shared_ptr<FuseNode> target) {
		m_owner = std::static_pointer_cast<FsNode>(owner);
		m_name = name;
		m_target = std::static_pointer_cast<FsNode>(target);
	}

	std::shared_ptr<FsNode> getOwner() override {
		std::cout << "FuseLink::getOwner()" << std::endl;
		return m_owner;
	}

	std::string getName() override {
		std::cout << "FuseLink::getName()" << std::endl;
		return m_name;
	}

	std::shared_ptr<FsNode> getTarget() override {
		std::cout << "FuseLink::getTarget()" << std::endl;
		return m_target;
	}

	async::result<frg::expected<Error>> obstruct() override {
		std::cout << "FuseLink::obstruct() not implemented" << std::endl;
		co_return Error::illegalArguments;
	}

	async::result<frg::expected<Error>> deobstruct() override {
		std::cout << "FuseLink::deobstruct() not implemented" << std::endl;
		co_return Error::illegalArguments;
	}
};

struct FuseNode : FsNode, std::enable_shared_from_this<FuseNode> {
private:
	smarter::shared_ptr<FuseDeviceFile, FileHandle> m_fuse_file;
	uint64_t m_node_id = 0;

	async::result<frg::expected<Error, FileStats>> getStatsCommon() {
		std::cout << "FuseNode::getStatsCommon()" << std::endl;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = sizeof(fuse_in_header) + sizeof(fuse_getattr_in),
			.opcode = FUSE_GETATTR,
			.unique = unique,
			.nodeid = m_node_id,
		};
		fuse_getattr_in data_in = {};

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header) + sizeof(fuse_attr_out));
		fuse_out_header head_out;
		fuse_attr_out data_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			//TODO: map errno to Error
			co_return Error::accessDenied;
		}

		//TODO: deal with attr_valid and attr_valid_nsec
		copyStructFromSpan(&data_out, out.subspan(sizeof(fuse_out_header)));
		FileStats stats = {
			.inodeNumber = data_out.attr.ino,
			.numLinks = static_cast<int>(data_out.attr.nlink),
			.fileSize = data_out.attr.size,
			.mode = data_out.attr.mode, //TODO: filter mode bits more cleanly/accurately?
			.uid = static_cast<int>(data_out.attr.uid),
			.gid = static_cast<int>(data_out.attr.gid),
			.atimeNanos = data_out.attr.atimensec,
			.mtimeNanos = data_out.attr.mtimensec,
			.ctimeNanos = data_out.attr.ctimensec
		};

		co_return stats;
	}

public:
	async::result<VfsType> getType() override {
		std::cout << "FuseNode::getType()" << std::endl;
		auto maybe_stats = co_await getStatsCommon();
		if(!maybe_stats)
			co_return VfsType::null; //error is more suitable

		auto stats = maybe_stats.unwrap();
		if(S_ISBLK(stats.mode))
			co_return VfsType::blockDevice;
		else if(S_ISCHR(stats.mode))
			co_return VfsType::charDevice;
		else if(S_ISDIR(stats.mode))
			co_return VfsType::directory;
		else if(S_ISFIFO(stats.mode))
			co_return VfsType::fifo;
		else if(S_ISREG(stats.mode))
			co_return VfsType::regular;
		else if(S_ISLNK(stats.mode))
			co_return VfsType::symlink;

		std::cout << "FuseNode::getType(): file type undefined" << std::endl;
		co_return VfsType::null; //appropriate here; type undefined
		//TODO: maybe add semaphore etc. to VfsType?
	}

	async::result<frg::expected<Error, FileStats>> getStats() override {
		std::cout << "FuseNode::getStats()" << std::endl;
		auto maybe_stats = co_await getStatsCommon();
		if(!maybe_stats)
			co_return maybe_stats.error();

		auto stats = maybe_stats.unwrap();
		stats.mode &= 0777;
		co_return stats;
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
	getLink(std::string name) override {
		std::cout << "FuseNode::getLink()" << std::endl;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + name.size() + 1),
			.opcode = FUSE_LOOKUP,
			.unique = unique,
			.nodeid = m_node_id,
		};

		auto request = requestToVector(&head_in, &name);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header) + sizeof(fuse_entry_out));
		fuse_out_header head_out;
		fuse_entry_out data_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied; //TODO: mapp errno to Error
		}

		copyStructFromSpan(&data_out, out.subspan(sizeof(fuse_out_header)));
		auto target = std::make_shared<FuseNode>(m_fuse_file, data_out.nodeid);
		co_return std::make_shared<FuseLink>(shared_from_this(), name, target);
	}

	async::result<frg::expected<Error, std::shared_ptr<FsLink>>>
	link(std::string name, std::shared_ptr<FsNode> target) override {
		std::cout << "FuseNode::link()" << std::endl;
		auto maybe_stats = co_await target->getStats();
		if(!maybe_stats)
			co_return maybe_stats.error();
		uint64_t target_inode = maybe_stats.unwrap().inodeNumber;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_link_in) + name.size() + 1),
			.opcode = FUSE_LINK,
			.unique = unique,
			.nodeid = target_inode,
		};
		fuse_link_in data_in = {
			.oldnodeid = m_node_id,
		};

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header));
		fuse_out_header head_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied;
		}

		co_return co_await target->getLink(name);
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	mkdir(std::string name) override {
		std::cout << "FuseNode::mkdir()" << std::endl;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_mkdir_in) + name.size() + 1),
			.opcode = FUSE_MKDIR,
			.unique = unique,
			.nodeid = m_node_id,
		};
		fuse_mkdir_in data_in = {};
		std::cout << "FUSE mkdir package size: " << head_in.len << std::endl;

		auto request = requestToVector(&head_in, &data_in, &name);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header));
		fuse_out_header head_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied; //TODO: map
		}

		auto maybe_link = co_await getLink(name);
		if(maybe_link) {
			co_return maybe_link.unwrap();
		}
		co_return maybe_link.error();
	}

	async::result<std::variant<Error, std::shared_ptr<FsLink>>>
	symlink(std::string name, std::string path) override {
		std::cout << "FuseNode::symlink()" << std::endl;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + name.size() + path.size() + 2),
			.opcode = FUSE_SYMLINK,
			.unique = unique,
			.nodeid = m_node_id,
		};

		auto request = requestToVector(&head_in, &name, &path);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header));
		fuse_out_header head_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied; //TODO: map
		}

		auto maybe_link = co_await getLink(name);
		if(maybe_link) {
			co_return maybe_link.unwrap();
		}
		co_return maybe_link.error();
	}

	async::result<frg::expected<Error>> unlink(std::string name) override {
		std::cout << "FuseNode::unlink()" << std::endl;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + name.size() + 1),
			.opcode = FUSE_UNLINK,
			.unique = unique,
			.nodeid = m_node_id,
		};

		auto request = requestToVector(&head_in, &name);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header));
		fuse_out_header head_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied; //TODO: map
		}

		co_return frg::success_tag{};
	}

	async::result<frg::expected<Error>> rmdir(std::string name) override {
		std::cout << "FuseNode::rmdir()" << std::endl;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + name.size() + 1),
			.opcode = FUSE_RMDIR,
			.unique = unique,
			.nodeid = m_node_id,
		};

		auto request = requestToVector(&head_in, &name);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header));
		fuse_out_header head_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied; //TODO: map
		}

		co_return frg::success_tag{};
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, SemanticFlags semantic_flags) override {
		std::cout << "FuseNode::open()" << std::endl;
		auto type = co_await getType();

		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			 .len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_open_in)),
			 .opcode = FUSE_OPEN,
			 .unique = unique,
			 .nodeid = m_node_id,
		};
		if(type == VfsType::directory)
			head_in.opcode = FUSE_OPENDIR;
		fuse_open_in data_in = {};
		if(semantic_flags & semanticRead && semantic_flags & semanticWrite) {
			data_in.flags = O_RDWR;
		}
		else if(semantic_flags & semanticRead) {
			data_in.flags = O_RDONLY;
		}
		else if(semantic_flags & semanticWrite) {
			data_in.flags = O_WRONLY;
		}

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header) + sizeof(fuse_open_out));
		fuse_out_header head_out;
		fuse_open_out data_out;
		copyStructFromSpan(&head_out, out);

		//TODO: map to errno
		if(head_out.error) {
			co_return Error::accessDenied;
		}

		copyStructFromSpan(&data_out, out.subspan(sizeof(fuse_out_header)));

		auto fuse_file = smarter::make_shared<FuseFile>(StructName::get("fusefs.file"), mount,
			link, m_fuse_file, data_out.fh);
		fuse_file->setupWeakFile(fuse_file);
		FuseFile::serve(fuse_file);
		co_return File::constructHandle(std::move(fuse_file));
	}

private:
	async::result<Error> setattr(fuse_setattr_in data_in) {
		std::cout << "FuseNode::setattr()" << std::endl;
		uint64_t unique = m_fuse_file->m_queue.get_unique();
		fuse_in_header head_in = {
			.len = static_cast<uint32_t>(sizeof(fuse_in_header) + sizeof(fuse_setattr_in)),
			.opcode = FUSE_SETATTR,
			.unique = unique,
			.nodeid = m_node_id,
		};

		auto request = requestToVector(&head_in, &data_in);
		auto out = co_await m_fuse_file->performRequest(request, unique, sizeof(fuse_out_header));
		fuse_out_header head_out;
		copyStructFromSpan(&head_out, out);

		if(head_out.error) {
			co_return Error::accessDenied; //TODO: map
		}

		co_return Error::success;
	}

public:
	async::result<Error> chmod(int mode) override {
		std::cout << "FuseNode::chmod()" << std::endl;
		fuse_setattr_in data_in = {
			.valid = FATTR_MODE,
			.mode = static_cast<uint32_t>(mode),
		};
		co_return co_await setattr(data_in);
	}

	async::result<Error> utimensat(uint64_t atime_sec, uint64_t atime_nsec, uint64_t mtime_sec,
		uint64_t mtime_nsec) override {
		std::cout << "FuseNode::utimensat()" << std::endl;
		fuse_setattr_in data_in = {
			.valid = FATTR_ATIME | FATTR_MTIME,
			.atime = atime_sec,
			.mtime = mtime_sec,
			.atimensec = static_cast<uint32_t>(atime_nsec),
			.mtimensec = static_cast<uint32_t>(mtime_nsec),
		};
		co_return co_await setattr(data_in);
	}

	FuseNode(smarter::shared_ptr<FuseDeviceFile, FileHandle> fuse_file, uint64_t node_id){
		m_fuse_file = fuse_file;
		m_node_id = node_id;
	}
};

struct FuseRootNode final : FuseNode {
	using FuseNode::FuseNode;

	async::result<VfsType> getType() override {
		co_return VfsType::directory;
	}
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
	auto file = smarter::static_pointer_cast<FuseDeviceFile>(proc->fileContext()->getFile(settings.fd()));
	if(file.get() == nullptr || file->structName().type() != "fuse-file") {
		return {};
	}
	file->set_mounted();
	return std::make_shared<FuseRootLink>(std::make_shared<FuseRootNode>(file, FUSE_ROOT_ID));
}

} // namespace fuse
