#include <algorithm>
#include <frg/hash_map.hpp>
#include <frg/string.hpp>
#include <elf.h>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/universe.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/module.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/thread.hpp>
#include "mbus.frigg_pb.hpp"
#include "svrctl.frigg_pb.hpp"

namespace thor {

static bool debugLaunch = true;

frg::manual_box<LaneHandle> mbusClient;
static frg::manual_box<LaneHandle> futureMbusServer;

frg::ticket_spinlock globalMfsMutex;

extern MfsDirectory *mfsRoot;

static frg::manual_box<
	frg::hash_map<
		frg::string<KernelAlloc>,
		LaneHandle,
		frg::hash<frg::string<KernelAlloc>>,
		KernelAlloc
	>
> allServers;

// TODO: move this declaration to a header file
void runService(frg::string<KernelAlloc> desc, LaneHandle control_lane,
		smarter::shared_ptr<Thread, ActiveHandle> thread);

// ------------------------------------------------------------------------
// File management.
// ------------------------------------------------------------------------

coroutine<bool> createMfsFile(frg::string_view path, const void *buffer, size_t size,
		MfsRegular **out) {
	// Copy to the memory object before taking locks below.
	auto memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc,
			(size + (kPageSize - 1)) & ~size_t{kPageSize - 1});
	memory->selfPtr = memory;
	co_await copyToView(memory.get(), 0, buffer, size,
			WorkQueue::generalQueue()->take());

	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&globalMfsMutex);

	const char *begin = path.data();
	const char *end = path.data() + path.size();
	auto it = begin;

	// We have no VFS. Relative paths are absolute.
	if(it != end && *it == '/')
		++it;

	// Parse each individual component.
	MfsNode *node = mfsRoot;
	while(it != end) {
		auto slash = std::find(it, end, '/');
		if(slash == end)
			break;

		auto component = path.sub_string(it - begin, slash - it);
		if(component == "..") {
			// We resolve double-dots unless they are at the beginning of the path.
			assert(!"Fix double-dots");
		}else if(component.size() && component != ".") {
			// We discard multiple slashes and single-dots.
			assert(node->type == MfsType::directory);
			auto directory = static_cast<MfsDirectory *>(node);
			auto target = directory->getTarget(component);
			if(target) {
				node = target;
			}else{
				node = frg::construct<MfsDirectory>(*kernelAlloc);
				directory->link(frg::string<KernelAlloc>{*kernelAlloc, component}, node);
			}
		}

		// Finally we need to skip the slash we found.
		it = slash;
		if(it != end)
			++it;
	}

	// Now, insert the file into its parent directory.
	auto directory = static_cast<MfsDirectory *>(node);
	auto name = path.sub_string(it - begin, end - it);
	if(auto file = directory->getTarget(name); file) {
		assert(file->type == MfsType::regular);
		*out = static_cast<MfsRegular *>(file);
		co_return false;
	}

	auto file = frg::construct<MfsRegular>(*kernelAlloc, std::move(memory), size);
	directory->link(frg::string<KernelAlloc>{*kernelAlloc, name}, file);
	*out = file;
	co_return true;
}

MfsNode *resolveModule(frg::string_view path) {
	auto irqLock = frg::guard(&irqMutex());
	auto lock = frg::guard(&globalMfsMutex);

	const char *begin = path.data();
	const char *end = path.data() + path.size();
	auto it = begin;

	// We have no VFS. Relative paths are absolute.
	if(it != end && *it == '/')
		++it;

	// Parse each individual component.
	MfsNode *node = mfsRoot;
	while(it != end) {
		auto slash = std::find(it, end, '/');

		auto component = path.sub_string(it - begin, slash - it);
		if(component == "..") {
			// We resolve double-dots unless they are at the beginning of the path.
			assert(!"Fix double-dots");
		}else if(component.size() && component != ".") {
			// We discard multiple slashes and single-dots.
			assert(node->type == MfsType::directory);
			auto directory = static_cast<MfsDirectory *>(node);
			auto target = directory->getTarget(component);
			if(!target)
				return nullptr;
			node = target;
		}

		// Finally we need to skip the slash we found.
		it = slash;
		if(it != end)
			++it;
	}

	return node;
}

// ------------------------------------------------------------------------
// ELF parsing and execution.
// ------------------------------------------------------------------------

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr), interpreter(*kernelAlloc) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	frg::string<KernelAlloc> interpreter;
};

coroutine<ImageInfo> loadModuleImage(smarter::shared_ptr<AddressSpace, BindableHandle> space,
		VirtualAddr base, smarter::shared_ptr<MemoryView> image) {
	ImageInfo info;

	// parse the ELf file format
	Elf64_Ehdr ehdr;
	co_await copyFromView(image.get(), 0, &ehdr, sizeof(Elf64_Ehdr),
			WorkQueue::generalQueue()->take());
	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');

	info.entryIp = reinterpret_cast<void *>(base + ehdr.e_entry);
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	for(int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		co_await copyFromView(image.get(), ehdr.e_phoff + i * ehdr.e_phentsize,
				&phdr, sizeof(Elf64_Phdr), WorkQueue::generalQueue()->take());
		switch(phdr.p_type) {
		case PT_LOAD: {
			assert(phdr.p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr.p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr.p_vaddr + phdr.p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			auto memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, virt_length);
			memory->selfPtr = memory;
			co_await copyBetweenViews(memory.get(), phdr.p_vaddr - virt_address,
					image.get(), phdr.p_offset, phdr.p_filesz,
					WorkQueue::generalQueue()->take());

			auto view = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
					std::move(memory), 0, virt_length);

			if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				auto mapResult = co_await space->map(std::move(view),
						base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapProtRead
							| AddressSpace::kMapProtWrite);
				assert(mapResult);
			}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				auto mapResult = co_await space->map(std::move(view),
						base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapProtRead
							| AddressSpace::kMapProtExecute);
				assert(mapResult);
			}else{
				panicLogger() << "Illegal combination of segment permissions"
						<< frg::endlog;
			}
			break;
		}
		case PT_INTERP: {
			info.interpreter.resize(phdr.p_filesz);
			co_await copyFromView(image.get(), phdr.p_offset,
					info.interpreter.data(), phdr.p_filesz, WorkQueue::generalQueue()->take());
			break;
		}
		case PT_PHDR: {
			info.phdrPtr = reinterpret_cast<void *>(base + phdr.p_vaddr);
			break;
		}
		case PT_DYNAMIC:
		case PT_TLS:
		case PT_GNU_EH_FRAME:
		case PT_GNU_STACK: {
			// ignore the phdr
			break;
		}
		default:
			assert(!"Unexpected PHDR");
		}
	}

	co_return info;
}

template<typename T>
uintptr_t copyToStack(frg::string<KernelAlloc> &stack_image, const T &data) {
	uintptr_t misalign = stack_image.size() % alignof(T);
	if(misalign)
		stack_image.resize(alignof(T) - misalign);
	uintptr_t offset = stack_image.size();
	stack_image.resize(stack_image.size() + sizeof(data));
	memcpy(&stack_image[offset], &data, sizeof(data));
	return offset;
}

coroutine<void> executeModule(frg::string_view name, MfsRegular *module,
		LaneHandle control_lane,
		LaneHandle xpipe_lane, LaneHandle mbus_lane,
		Scheduler *scheduler) {
	auto space = AddressSpace::create();

	ImageInfo exec_info = co_await loadModuleImage(space, 0, module->getMemory());

	// FIXME: use actual interpreter name here
	auto rtdl_module = resolveModule("lib/ld-init.so");
	assert(rtdl_module && rtdl_module->type == MfsType::regular);
	ImageInfo interp_info = co_await loadModuleImage(space, 0x40000000,
			static_cast<MfsRegular *>(rtdl_module)->getMemory());

	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stack_memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, stack_size);
	stack_memory->selfPtr = stack_memory;
	auto stack_view = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
			stack_memory, 0, stack_size);

	auto mapResult = co_await space->map(std::move(stack_view), 0, 0, stack_size,
			AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead
				| AddressSpace::kMapProtWrite);
	assert(mapResult);

	// build the stack data area (containing program arguments,
	// environment strings and related data).
	// TODO: do we actually need this buffer?
	frg::string<KernelAlloc> data_area(*kernelAlloc);

	uintptr_t data_disp = stack_size - data_area.size();
	co_await copyToView(stack_memory.get(), data_disp, data_area.data(), data_area.size(),
			WorkQueue::generalQueue()->take());

	// build the stack tail area (containing the aux vector).
	auto universe = smarter::allocate_shared<Universe>(*kernelAlloc);

	Handle xpipe_handle = 0;
	Handle mbus_handle = 0;
	if(xpipe_lane) {
		auto lock = frg::guard(&universe->lock);
		xpipe_handle = universe->attachDescriptor(lock,
				LaneDescriptor(xpipe_lane));
	}
	if(mbus_lane) {
		auto lock = frg::guard(&universe->lock);
		mbus_handle = universe->attachDescriptor(lock,
				LaneDescriptor(mbus_lane));
	}

	enum {
		AT_NULL = 0,
		AT_PHDR = 3,
		AT_PHENT = 4,
		AT_PHNUM = 5,
		AT_ENTRY = 9,
		
		AT_XPIPE = 0x1000,
		AT_MBUS_SERVER = 0x1103
	};

	frg::string<KernelAlloc> tail_area(*kernelAlloc);
	
	// Setup the stack with argc, argv and environment.
	copyToStack<uintptr_t>(tail_area, 0); // argc.
	copyToStack<uintptr_t>(tail_area, 0); // End of args.
	copyToStack<uintptr_t>(tail_area, 0); // End of environment.

	// This is the auxiliary vector.
	copyToStack<uintptr_t>(tail_area, AT_ENTRY);
	copyToStack<uintptr_t>(tail_area, (uintptr_t)exec_info.entryIp);
	copyToStack<uintptr_t>(tail_area, AT_PHDR);
	copyToStack<uintptr_t>(tail_area, (uintptr_t)exec_info.phdrPtr);
	copyToStack<uintptr_t>(tail_area, AT_PHENT);
	copyToStack<uintptr_t>(tail_area, exec_info.phdrEntrySize);
	copyToStack<uintptr_t>(tail_area, AT_PHNUM);
	copyToStack<uintptr_t>(tail_area, exec_info.phdrCount);
	if(xpipe_lane) {
		copyToStack<uintptr_t>(tail_area, AT_XPIPE);
		copyToStack<uintptr_t>(tail_area, xpipe_handle);
	}
	if(mbus_lane) {
		copyToStack<uintptr_t>(tail_area, AT_MBUS_SERVER);
		copyToStack<uintptr_t>(tail_area, mbus_handle);
	}
	copyToStack<uintptr_t>(tail_area, AT_NULL);
	copyToStack<uintptr_t>(tail_area, 0);

	// Padding to ensure the stack alignment.
	copyToStack<uintptr_t>(tail_area, 0);
	
	uintptr_t tail_disp = data_disp - tail_area.size();
	assert(!(tail_disp % 16));
	co_await copyToView(stack_memory.get(), tail_disp, tail_area.data(), tail_area.size(),
			WorkQueue::generalQueue()->take());

	// create a thread for the module
	AbiParameters params;
	params.ip = (uintptr_t)interp_info.entryIp;
	params.sp = mapResult.value() + tail_disp;
	params.argument = 0;

	auto thread = Thread::create(std::move(universe), std::move(space), params);
	thread->self = remove_tag_cast(thread);
	thread->flags |= Thread::kFlagServer;
	
	// listen to POSIX calls from the thread.
	runService(frg::string<KernelAlloc>{*kernelAlloc, name.data(), name.size()},
			control_lane,
			thread);

	// see helCreateThread for the reasoning here
	thread.ctr()->increment();
	thread.ctr()->increment();

	Scheduler::associate(thread.get(), scheduler);
	Thread::resumeOther(remove_tag_cast(thread));
}

void initializeMbusStream() {
	auto mbusStream = createStream();
	mbusClient.initialize(std::move(mbusStream.get<1>()));
	futureMbusServer.initialize(std::move(mbusStream.get<0>()));
}

coroutine<void> runMbus() {
	if(debugLaunch)
		infoLogger() << "thor: Launching mbus" << frg::endlog;

	frg::string<KernelAlloc> nameStr{*kernelAlloc, "/sbin/mbus"};
	assert(!allServers->get(nameStr));

	auto controlStream = createStream();
	allServers->insert(nameStr, controlStream.get<1>());

	auto module = resolveModule("/sbin/mbus");
	assert(module && module->type == MfsType::regular);
	co_await executeModule("/sbin/mbus", static_cast<MfsRegular *>(module),
			controlStream.get<0>(),
			std::move(*futureMbusServer), LaneHandle{}, localScheduler());
}

coroutine<LaneHandle> runServer(frg::string_view name) {
	if(debugLaunch)
		infoLogger() << "thor: Launching server " << name << frg::endlog;

	frg::string<KernelAlloc> nameStr{*kernelAlloc, name.data(), name.size()};
	if(auto server = allServers->get(nameStr); server) {
		if(debugLaunch)
			infoLogger() << "thor: Server "
					<< name << " is already running" << frg::endlog;
		co_return *server;
	}

	auto module = resolveModule(name);
	if(!module)
		panicLogger() << "thor: Could not find module " << name << frg::endlog;
	assert(module->type == MfsType::regular);

	auto controlStream = createStream();
	allServers->insert(nameStr, controlStream.get<1>());

	co_await executeModule(name, static_cast<MfsRegular *>(module),
			controlStream.get<0>(),
			LaneHandle{}, *mbusClient, localScheduler());

	co_return controlStream.get<1>();
}

// ------------------------------------------------------------------------
// svrctl interface to user space.
// ------------------------------------------------------------------------

namespace {

coroutine<Error> handleReq(LaneHandle boundLane) {
	auto [acceptError, lane] = co_await AcceptSender{boundLane};
	if(acceptError != Error::success)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	if(reqError != Error::success)
		co_return reqError;
	managarm::svrctl::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::svrctl::CntReqType::FILE_UPLOAD) {
		auto file = resolveModule(req.name());
		if(file) {
			// The file data is already known to us.
			managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::svrctl::Error::SUCCESS);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
			if(respError != Error::success)
				co_return respError;
		}else{
			// Ask user space for the file data.
			managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::svrctl::Error::DATA_REQUIRED);

			frg::string<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
			memcpy(respBuffer.data(), ser.data(), ser.size());
			auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
			if(respError != Error::success)
				co_return respError;
		}
	}else if(req.req_type() == managarm::svrctl::CntReqType::FILE_UPLOAD_DATA) {
		auto [dataError, dataBuffer] = co_await RecvBufferSender{lane};
		if(dataError != Error::success)
			co_return dataError;
		MfsRegular *file;
		if(!(co_await createMfsFile(req.name(), dataBuffer.data(), dataBuffer.size(), &file))) {
			// TODO: Verify that the file data matches. This is somewhat expensive because
			//       we would have to map the file's memory. Hence, we do not implement
			//       it for now.
			if(file->size() != dataBuffer.size()) {
				managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::svrctl::Error::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
				if(respError != Error::success)
					co_return respError;
				co_return Error::success;
			}
		}

		managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::svrctl::Error::SUCCESS);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success)
			co_return respError;
	}else if(req.req_type() == managarm::svrctl::CntReqType::SVR_RUN) {
		auto controlLane = co_await runServer(req.name());

		managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::svrctl::Error::SUCCESS);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success)
			co_return respError;
		auto controlError = co_await PushDescriptorSender{lane, LaneDescriptor{controlLane}};
		if(controlError != Error::success)
			co_return controlError;
	}else{
		managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::svrctl::Error::ILLEGAL_REQUEST);
		
		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success)
			co_return respError;
	}

	co_return Error::success;
}

} // anonymous namespace

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

namespace {

coroutine<void> handleBind(LaneHandle objectLane);

coroutine<void> createObject(LaneHandle mbusLane) {
	auto [offerError, lane] = co_await OfferSender{mbusLane};
	assert(offerError == Error::success && "Unexpected mbus transaction");

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "svrctl"));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(reqError == Error::success && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(respError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(objectError == Error::success && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>());
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;
	while(true)
		co_await handleBind(objectLane);
}

coroutine<void> handleBind(LaneHandle objectLane) {
	auto [acceptError, lane] = co_await AcceptSender{objectLane};
	assert(acceptError == Error::success && "Unexpected mbus transaction");

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(reqError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
	assert(respError == Error::success && "Unexpected mbus transaction");

	auto stream = createStream();
	auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
	assert(boundError == Error::success && "Unexpected mbus transaction");
	auto boundLane = stream.get<0>();

	async::detach_with_allocator(*kernelAlloc, ([] (LaneHandle boundLane) -> coroutine<void> {
		while(true) {
			auto error = co_await handleReq(boundLane);
			if(error == Error::endOfLane)
				break;
			if(isRemoteIpcError(error))
				infoLogger() << "thor: Aborting svrctl request"
						" after remote violated the protocol" << frg::endlog;
			assert(error == Error::success);
		}
	})(boundLane));
}

} // anonymous namespace

void initializeSvrctl() {
	allServers.initialize(frg::hash<frg::string<KernelAlloc>>{}, *kernelAlloc);

	// Create a fiber to manage requests to the svrctl mbus object.
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, createObject(*mbusClient));
	});
}

} // namespace thor
