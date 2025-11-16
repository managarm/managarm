#include "common.hpp"
#include "../memfd.hpp"
#include <sys/mman.h>
#include <linux/memfd.h>

namespace requests {

// VM_MAP handler
async::result<void> handleVmMap(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::VmMapRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "VM_MAP", "size={:#x}", req->size());

	// TODO: Validate req->flags().

	if(req->mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	if(req->rel_offset() & 0xFFF) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	uint32_t nativeFlags = 0;

	if(req->mode() & PROT_READ)
		nativeFlags |= kHelMapProtRead;
	if(req->mode() & PROT_WRITE)
		nativeFlags |= kHelMapProtWrite;
	if(req->mode() & PROT_EXEC)
		nativeFlags |= kHelMapProtExecute;

	if(req->flags() & MAP_FIXED_NOREPLACE)
		nativeFlags |= kHelMapFixedNoReplace;
	else if(req->flags() & MAP_FIXED)
		nativeFlags |= kHelMapFixed;

	bool copyOnWrite;
	if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_PRIVATE) {
		copyOnWrite = true;
	}else if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_SHARED) {
		copyOnWrite = false;
	}else{
		throw std::runtime_error("posix: Handle illegal flags in VM_MAP");
	}

	uintptr_t hint = req->address_hint();

	frg::expected<Error, void *> result;
	if(req->flags() & MAP_ANONYMOUS) {
		assert(!req->rel_offset());

		if(req->size() == 0) {
			std::cout << "posix: VM_MAP with size 0 is not allowed" << std::endl;
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}

		// We round up to page size
		size_t size = req->size();
		if(size & 0xFFF) {
			size = (req->size() + 0xFFF) & ~0xFFF;
		}

		if(copyOnWrite) {
			result = co_await ctx.self->vmContext()->mapFile(hint,
					{}, nullptr,
					0, size, true, nativeFlags);
		}else{
			HelHandle handle;
			HEL_CHECK(helAllocateMemory(size, 0, nullptr, &handle));

			result = co_await ctx.self->vmContext()->mapFile(hint,
					helix::UniqueDescriptor{handle}, nullptr,
					0, size, false, nativeFlags);
		}
	}else{
		auto file = ctx.self->fileContext()->getFile(req->fd());
		assert(file && "Illegal FD for VM_MAP");
		auto memory = co_await file->accessMemory();
		assert(memory);
		result = co_await ctx.self->vmContext()->mapFile(hint,
				std::move(memory), std::move(file),
				req->rel_offset(), req->size(), copyOnWrite, nativeFlags);
	}

	if(!result) {
		assert(result.error() == Error::alreadyExists || result.error() == Error::noMemory);
		if(result.error() == Error::alreadyExists)
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
		else if(result.error() == Error::noMemory)
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_MEMORY);
		co_return;
	}

	void *address = result.unwrap();

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_offset(reinterpret_cast<uintptr_t>(address));

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// MEMFD_CREATE handler
async::result<void> handleMemFdCreate(RequestContext& ctx) {
	managarm::posix::SvrResponse resp;
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::MemFdCreateRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "MEMFD_CREATE", "'{}'", req->name());

	if(req->flags() & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto link = SpecialLink::makeSpecialLink(VfsType::regular, 0777);
	auto memFile = smarter::make_shared<MemoryFile>(nullptr, link, (req->flags() & MFD_ALLOW_SEALING) == true);
	MemoryFile::serve(memFile);
	auto file = File::constructHandle(std::move(memFile));

	int flags = 0;

	if(req->flags() & MFD_CLOEXEC) {
		flags |= managarm::posix::OpenFlags::OF_CLOEXEC;
	}

	auto fd = ctx.self->fileContext()->attachFile(file, flags);

	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
        ctx.conversation,
        helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
    );
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

} // namespace requests
