
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

#include <cofiber.hpp>

#include "common.hpp"
//FIXME #include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "exec.hpp"
//FIXME #include "dev_fs.hpp"
//FIXME #include "pts_fs.hpp"
//FIXME #include "sysfile_fs.hpp"
//FIXME #include "extern_fs.hpp"
#include <posix.pb.h>

bool traceRequests = false;

//FIXME: helx::EventHub eventHub = helx::EventHub::create();
//FIXME: helx::Client mbusConnect;
//FIXME: helx::Pipe ldServerPipe;
//FIXME: helx::Pipe mbusPipe;

HelHandle ringBuffer;
HelRingBuffer *ringItem;

vfs::SharedEntry rootEntry;

COFIBER_ROUTINE(cofiber::no_future, serve(SharedProcess self,
		helix::UniqueDescriptor p), ([self, lane = std::move(p)] {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Accept<M> accept;
		helix::RecvBuffer<M> recv_req;

		char buffer[256];
		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req, buffer, 256)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::posix::ClientRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.request_type() == managarm::posix::ClientRequestType::OPEN) {
			auto entry = COFIBER_AWAIT rootEntry.getTarget().getChild(req.path());
			auto file = COFIBER_AWAIT entry.getTarget().open(entry);
			int fd = self.attachFile(file);
			(void)fd;

			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_passthrough;

			managarm::posix::ServerResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_passthrough, file.getPassthroughLane())
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_passthrough.future();
			
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_passthrough.error());
		}else{
			std::cout << "posix: Illegal request" << std::endl;
			helix::SendBuffer<M> send_resp;

			managarm::posix::ServerResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size())
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}
	}
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

#include <unistd.h>
#include <fcntl.h>

HelHandle __mlibc_getPassthrough(int fd);

namespace super_fs {

struct OpenFile {
	OpenFile(int fd)
	: _fd(fd) { }

	helix::BorrowedDescriptor getPassthroughLane() {
		return helix::BorrowedDescriptor(__mlibc_getPassthrough(_fd));
	}

private:
	int _fd;
};

struct Regular {
	Regular(int fd)
	: _fd(fd) { }

	COFIBER_ROUTINE(vfs::FutureMaybe<vfs::SharedFile>, open(vfs::SharedEntry entry), ([=] {
		COFIBER_RETURN(vfs::SharedFile::create<OpenFile>(std::move(entry), _fd));
	}))

private:
	int _fd;
};

struct Directory {
	COFIBER_ROUTINE(vfs::FutureMaybe<vfs::SharedNode>, resolveChild(std::string name), ([=] {
		int fd = open(name.c_str(), O_RDONLY);
		COFIBER_RETURN(vfs::SharedNode::createRegular<Regular>(fd));
	}))
};

} // namespace super_fs

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;
	
	ringItem = (HelRingBuffer *)malloc(sizeof(HelRingBuffer) + 0x10000);
	
	// initialize our string queue
	HEL_CHECK(helCreateRing(0x1000, &ringBuffer));
	int64_t async_id;
	HEL_CHECK(helSubmitRing(ringBuffer, helix::Dispatcher::global().getHub().getHandle(),
			ringItem, 0x10000, 0, 0, &async_id));

	auto root_node = vfs::SharedNode::createDirectory<super_fs::Directory>();
	rootEntry = vfs::SharedEntry::attach(vfs::SharedNode(),
			std::string(), std::move(root_node));

	execute(SharedProcess::createInit(), "posix-init");

	while(true)
		helix::Dispatcher::global().dispatch();
}

