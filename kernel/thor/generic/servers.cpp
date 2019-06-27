
#include <algorithm>

#include <frg/string.hpp>
#include <frigg/debug.hpp>
#include <frigg/elf.hpp>
#include "descriptor.hpp"
#include "fiber.hpp"
#include "mbus.frigg_pb.hpp"
#include "module.hpp"
#include "service_helpers.hpp"
#include "stream.hpp"
#include "svrctl.frigg_pb.hpp"

namespace thor {

static bool debugLaunch = true;

frigg::LazyInitializer<LaneHandle> mbusClient;

frigg::TicketLock globalMfsMutex;

extern MfsDirectory *mfsRoot;

// TODO: move this declaration to a header file
void runService(frg::string<KernelAlloc> desc, LaneHandle control_lane,
		frigg::SharedPtr<Thread> thread);

// ------------------------------------------------------------------------
// File management.
// ------------------------------------------------------------------------

void createMfsFile(frigg::StringView path, const void *buffer, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&globalMfsMutex);

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

		auto component = path.subString(it - begin, slash - it);
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
				node = frigg::construct<MfsDirectory>(*kernelAlloc);
				directory->link(frigg::String<KernelAlloc>{*kernelAlloc, component}, node);
			}
		}

		// Finally we need to skip the slash we found.
		it = slash;
		if(it != end)
			++it;
	}

	// Now, insert the file into its parent directory.
	auto directory = static_cast<MfsDirectory *>(node);

	auto memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
			(size + (kPageSize - 1)) & ~size_t{kPageSize - 1});
	fiberCopyToBundle(memory.get(), 0, buffer, size);

	auto name = path.subString(it - begin, end - it);
	auto file = frigg::construct<MfsRegular>(*kernelAlloc, std::move(memory));
	directory->link(frigg::String<KernelAlloc>{*kernelAlloc, name}, file);
}

MfsNode *resolveModule(frigg::StringView path) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&globalMfsMutex);

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

		auto component = path.subString(it - begin, slash - it);
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
	frigg::String<KernelAlloc> interpreter;
};

ImageInfo loadModuleImage(smarter::shared_ptr<AddressSpace, BindableHandle> space,
		VirtualAddr base, frigg::SharedPtr<Memory> image) {
	ImageInfo info;

	// parse the ELf file format
	Elf64_Ehdr ehdr;
	fiberCopyFromBundle(image.get(), 0, &ehdr, sizeof(Elf64_Ehdr));
	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');

	info.entryIp = (void *)(base + ehdr.e_entry);
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	for(int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		fiberCopyFromBundle(image.get(), ehdr.e_phoff + i * ehdr.e_phentsize,
				&phdr, sizeof(Elf64_Phdr));
		
		if(phdr.p_type == PT_LOAD) {
			assert(phdr.p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr.p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr.p_vaddr + phdr.p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			auto memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, virt_length);
			TransferNode copy;
			copy.setup(memory.get(), phdr.p_vaddr - virt_address,
					image.get(), phdr.p_offset, phdr.p_filesz, nullptr);
			if(!Memory::transfer(&copy))
				assert(!"Fix the asynchronous case");

			auto view = frigg::makeShared<MemorySlice>(*kernelAlloc,
					frigg::move(memory), 0, virt_length);

			VirtualAddr actual_address;
			if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				auto irq_lock = frigg::guard(&irqMutex());
				AddressSpace::Guard space_guard(&space->lock);

				auto error = space->map(space_guard, frigg::move(view),
						base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapProtRead
							| AddressSpace::kMapProtWrite,
						&actual_address);
				assert(!error);
			}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				auto irq_lock = frigg::guard(&irqMutex());
				AddressSpace::Guard space_guard(&space->lock);

				auto error = space->map(space_guard, frigg::move(view),
						base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapProtRead
							| AddressSpace::kMapProtExecute,
						&actual_address);
				assert(!error);
			}else{
				frigg::panicLogger() << "Illegal combination of segment permissions"
						<< frigg::endLog;
			}
		}else if(phdr.p_type == PT_INTERP) {
			info.interpreter.resize(phdr.p_filesz);
			fiberCopyFromBundle(image.get(), phdr.p_offset,
					info.interpreter.data(), phdr.p_filesz);
		}else if(phdr.p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr.p_vaddr;
		}else if(phdr.p_type == PT_DYNAMIC
				|| phdr.p_type == PT_TLS
				|| phdr.p_type == PT_GNU_EH_FRAME
				|| phdr.p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			assert(!"Unexpected PHDR");
		}
	}

	return info;
}

template<typename T>
uintptr_t copyToStack(frigg::String<KernelAlloc> &stack_image, const T &data) {
	uintptr_t misalign = stack_image.size() % alignof(data);
	if(misalign)
		stack_image.resize(alignof(data) - misalign);
	uintptr_t offset = stack_image.size();
	stack_image.resize(stack_image.size() + sizeof(data));
	memcpy(&stack_image[offset], &data, sizeof(data));
	return offset;
}

void executeModule(frigg::StringView name, MfsRegular *module,
		LaneHandle control_lane,
		LaneHandle xpipe_lane, LaneHandle mbus_lane,
		Scheduler *scheduler) {
	auto space = AddressSpace::create();
	space->setupDefaultMappings();

	ImageInfo exec_info = loadModuleImage(space, 0, module->getMemory());

	// FIXME: use actual interpreter name here
	auto rtdl_module = resolveModule("lib/ld-init.so");
	assert(rtdl_module && rtdl_module->type == MfsType::regular);
	ImageInfo interp_info = loadModuleImage(space, 0x40000000,
			static_cast<MfsRegular *>(rtdl_module)->getMemory());

	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stack_memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, stack_size);
	auto stack_view = frigg::makeShared<MemorySlice>(*kernelAlloc,
			stack_memory, 0, stack_size);

	VirtualAddr stack_base;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		space->map(space_guard, frigg::move(stack_view), 0, 0, stack_size,
				AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead
					| AddressSpace::kMapProtWrite,
				&stack_base);
	}

	// build the stack data area (containing program arguments,
	// environment strings and related data).
	// TODO: do we actually need this buffer?
	frigg::String<KernelAlloc> data_area(*kernelAlloc);

	uintptr_t data_disp = stack_size - data_area.size();
	fiberCopyToBundle(stack_memory.get(), data_disp, data_area.data(), data_area.size());

	// build the stack tail area (containing the aux vector).
	auto universe = frigg::makeShared<Universe>(*kernelAlloc);

	Handle xpipe_handle = 0;
	Handle mbus_handle = 0;
	if(xpipe_lane) {
		auto lock = frigg::guard(&universe->lock);
		xpipe_handle = universe->attachDescriptor(lock,
				LaneDescriptor(xpipe_lane));
	}
	if(mbus_lane) {
		auto lock = frigg::guard(&universe->lock);
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

	frigg::String<KernelAlloc> tail_area(*kernelAlloc);
	
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
	fiberCopyToBundle(stack_memory.get(), tail_disp, tail_area.data(), tail_area.size());

	// create a thread for the module
	AbiParameters params;
	params.ip = (uintptr_t)interp_info.entryIp;
	params.sp = stack_base + tail_disp;
	params.argument = 0;

	auto thread = Thread::create(std::move(universe), frigg::move(space), params);
	thread->self = thread;
	thread->flags |= Thread::kFlagServer;
	
	// listen to POSIX calls from the thread.
	runService(frg::string<KernelAlloc>{*kernelAlloc, name.data(), name.size()},
			control_lane,
			thread);

	// see helCreateThread for the reasoning here
	thread.control().increment();
	thread.control().increment();

	Scheduler::associate(thread.get(), scheduler);
	Thread::resumeOther(thread);
}

void runMbus() {
	if(debugLaunch)
		frigg::infoLogger() << "thor: Launching mbus" << frigg::endLog;

	auto stream = createStream();
	mbusClient.initialize(stream.get<1>());

	auto module = resolveModule("sbin/mbus");
	assert(module && module->type == MfsType::regular);
	executeModule("sbin/mbus", static_cast<MfsRegular *>(module),
			LaneHandle{},
			stream.get<0>(), LaneHandle{}, localScheduler());
}

LaneHandle runServer(frigg::StringView name) {
	if(debugLaunch)
		frigg::infoLogger() << "thor: Launching server " << name << frigg::endLog;

	auto control_stream = createStream();

	auto module = resolveModule(name);
	if(!module)
		frigg::panicLogger() << "thor: Could not find module " << name << frigg::endLog;
	assert(module->type == MfsType::regular);
	executeModule(name, static_cast<MfsRegular *>(module),
			control_stream.get<0>(),
			LaneHandle{}, *mbusClient, localScheduler());

	return control_stream.get<1>();
}

// ------------------------------------------------------------------------
// svrctl interface to user space.
// ------------------------------------------------------------------------

namespace {

bool handleReq(LaneHandle lane) {
	auto branch = fiberAccept(lane);
	if(!branch)
		return false;

	auto buffer = fiberRecv(branch);
	managarm::svrctl::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(buffer.data(), buffer.size());

	if(req.req_type() == managarm::svrctl::CntReqType::FILE_UPLOAD) {
		auto buffer = fiberRecv(branch);
		createMfsFile(req.name(), buffer.data(), buffer.size());

		managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::svrctl::Error::SUCCESS);
	
		frigg::String<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());
	}else if(req.req_type() == managarm::svrctl::CntReqType::SVR_RUN) {
		auto control_lane = runServer(req.name());

		managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::svrctl::Error::SUCCESS);

		frigg::String<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());
		fiberPushDescriptor(branch, LaneDescriptor{control_lane});
	}else{
		managarm::svrctl::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::svrctl::Error::ILLEGAL_REQUEST);
		
		frigg::String<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());
	}

	return true;
}

} // anonymous namespace

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

namespace {

LaneHandle createObject(LaneHandle mbus_lane) {
	auto branch = fiberOffer(mbus_lane);
	
	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frigg::String<KernelAlloc>(*kernelAlloc, "svrctl"));
	
	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frigg::String<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	fiberSend(branch, ser.data(), ser.size());

	auto buffer = fiberRecv(branch);
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(buffer.data(), buffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	auto descriptor = fiberPullDescriptor(branch);
	assert(descriptor.is<LaneDescriptor>());
	return descriptor.get<LaneDescriptor>().handle;
}

void handleBind(LaneHandle object_lane) {
	auto branch = fiberAccept(object_lane);
	assert(branch);

	auto buffer = fiberRecv(branch);
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(buffer.data(), buffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);
	
	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frigg::String<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	fiberSend(branch, ser.data(), ser.size());

	auto stream = createStream();
	fiberPushDescriptor(branch, LaneDescriptor{stream.get<1>()});

	// TODO: Do this in an own fiber.
	KernelFiber::run([lane = stream.get<0>()] () {
		while(true) {
			if(!handleReq(lane))
				break;
		}
	});
}

} // anonymous namespace

void initializeSvrctl() {
	// Create a fiber to manage requests to the svrctl mbus object.
	KernelFiber::run([=] {
		auto object_lane = createObject(*mbusClient);
		while(true)
			handleBind(object_lane);
	});
}

} // namespace thor

