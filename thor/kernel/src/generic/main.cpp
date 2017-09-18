
#include <algorithm>

#include "kernel.hpp"
#include "module.hpp"
#include "irq.hpp"
#include "fiber.hpp"
#include <frigg/elf.hpp>
#include <eir/interface.hpp>

namespace thor {

static constexpr bool logInitialization = false;
static constexpr bool logEveryIrq = false;
static constexpr bool logEverySyscall = false;

bool debugToVga = false;
bool debugToSerial = false;
bool debugToBochs = false;

// TODO: get rid of the rootUniverse global variable.
frigg::LazyInitializer<frigg::SharedPtr<Universe>> rootUniverse;

frigg::LazyInitializer<IrqSlot> globalIrqSlots[24];

frigg::LazyInitializer<LaneHandle> mbusClient;

MfsDirectory *mfsRoot;

// TODO: move this declaration to a header file
void runService(frigg::SharedPtr<Thread> thread);

MfsNode *resolveModule(frigg::StringView path) {
	const char *begin = path.data();
	const char *end = path.data() + path.size();
	auto it = begin;

	// We have no VFS. Ignore absolute paths.
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
/*
			if(components.empty() || components.back() == "..") {
				components.push_back("..");
			}else{
				components.pop_back();
			}
*/
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

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr), interpreter(*kernelAlloc) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	frigg::String<KernelAlloc> interpreter;
};

ImageInfo loadModuleImage(frigg::SharedPtr<AddressSpace> space,
		VirtualAddr base, frigg::SharedPtr<Memory> image) {
	ImageInfo info;

	// parse the ELf file format
	Elf64_Ehdr ehdr;
	image->load(0, &ehdr, sizeof(Elf64_Ehdr));
	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');

	info.entryIp = (void *)(base + ehdr.e_entry);
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	for(int i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		image->load(ehdr.e_phoff + i * ehdr.e_phentsize, &phdr, sizeof(Elf64_Phdr));
		
		if(phdr.p_type == PT_LOAD) {
			assert(phdr.p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr.p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr.p_vaddr + phdr.p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			auto memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, virt_length);
			Memory::transfer(memory, phdr.p_vaddr - virt_address,
					image, phdr.p_offset, phdr.p_filesz);

			VirtualAddr actual_address;
			if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				auto irq_lock = frigg::guard(&irqMutex());
				AddressSpace::Guard space_guard(&space->lock);

				space->map(space_guard, memory, base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadWrite,
						&actual_address);
			}else if((phdr.p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				auto irq_lock = frigg::guard(&irqMutex());
				AddressSpace::Guard space_guard(&space->lock);

				space->map(space_guard, memory, base + virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadExecute,
						&actual_address);
			}else{
				frigg::panicLogger() << "Illegal combination of segment permissions"
						<< frigg::endLog;
			}
			thorRtInvalidateSpace();
		}else if(phdr.p_type == PT_INTERP) {
			info.interpreter.resize(phdr.p_filesz);
			image->load(phdr.p_offset, info.interpreter.data(), phdr.p_filesz);
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

void executeModule(MfsRegular *module, LaneHandle xpipe_lane, LaneHandle mbus_lane) {
	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc);
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

	VirtualAddr stack_base;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		space->map(space_guard, stack_memory, 0, 0, stack_size,
				AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite, &stack_base);
	}
	thorRtInvalidateSpace();

	// build the stack data area (containing program arguments,
	// environment strings and related data).
	// TODO: do we actually need this buffer?
	frigg::String<KernelAlloc> data_area(*kernelAlloc);

	uintptr_t data_disp = stack_size - data_area.size();
	stack_memory->copyFrom(data_disp, data_area.data(), data_area.size());

	// build the stack tail area (containing the aux vector).
	Handle xpipe_handle;
	Handle mbus_handle;
	if(xpipe_lane) {
		auto lock = frigg::guard(&(*rootUniverse)->lock);
		xpipe_handle = (*rootUniverse)->attachDescriptor(lock,
				LaneDescriptor(xpipe_lane));
	}
	if(mbus_lane) {
		auto lock = frigg::guard(&(*rootUniverse)->lock);
		mbus_handle = (*rootUniverse)->attachDescriptor(lock,
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

	uintptr_t tail_disp = data_disp - tail_area.size();
	assert(!(tail_disp % 16));
	stack_memory->copyFrom(tail_disp, tail_area.data(), tail_area.size());

	// create a thread for the module
	AbiParameters params;
	params.ip = (uintptr_t)interp_info.entryIp;
	params.sp = stack_base + tail_disp;

	auto thread = Thread::create(*rootUniverse, frigg::move(space), params);
	thread->self = thread;
	thread->flags |= Thread::kFlagExclusive | Thread::kFlagTrapsAreFatal;
	
	// listen to POSIX calls from the thread.
	runService(thread);

	// see helCreateThread for the reasoning here
	thread.control().increment();
	thread.control().increment();

	globalScheduler().attach(thread.get());
	Thread::resumeOther(thread);
}

void setupDebugging();

extern "C" void frg_panic(const char *cstring) {
	frigg::panicLogger() << "frg: Panic! " << cstring << frigg::endLog;
}

extern "C" void thorMain(PhysicalAddr info_paddr) {
	auto info = reinterpret_cast<EirInfo *>(0x40000000);
	auto cmd_line = frigg::StringView{reinterpret_cast<char *>(info->commandLine)};
	if(cmd_line == "vga") {
		debugToVga = true;
	}else if(cmd_line == "serial") {
		debugToSerial = true;
	}else if(cmd_line == "bochs") {
		debugToBochs = true;
	}
	setupDebugging();
	
	frigg::infoLogger() << "Starting Thor" << frigg::endLog;
	
	initializeProcessorEarly();
	installBootCpuContext();

	if(info->signature == eirSignatureValue) {
		frigg::infoLogger() << "\e[37mthor: Bootstrap information signature matches\e[39m"
				<< frigg::endLog;
	}else{
		frigg::panicLogger() << "\e[31mthor: Bootstrap information signature mismatch!\e[39m"
				<< frigg::endLog;
	}

	// TODO: Move this to an architecture specific file.
	PhysicalAddr pml4_ptr;
	asm volatile ( "mov %%cr3, %%rax" : "=a" (pml4_ptr) );
	KernelPageSpace::initialize(pml4_ptr);

	SkeletalRegion::initialize(info->skeletalRegion.address,
			info->skeletalRegion.order, info->skeletalRegion.numRoots,
			reinterpret_cast<int8_t *>(info->skeletalRegion.buddyTree));

	physicalAllocator.initialize();
	physicalAllocator->bootstrap(info->coreRegion.address, info->coreRegion.order,
			info->coreRegion.numRoots, reinterpret_cast<int8_t *>(info->coreRegion.buddyTree));
	
	kernelVirtualAlloc.initialize();
	kernelAlloc.initialize(*kernelVirtualAlloc);

	initializePhysicalAccess();

	frigg::infoLogger() << "\e[37mthor: Basic memory management is ready\e[39m" << frigg::endLog;

	for(int i = 0; i < 24; i++)
		globalIrqSlots[i].initialize();

	initializeTheSystemEarly();
	initializeThisProcessor();
	
	if(logInitialization)
		frigg::infoLogger() << "thor: Bootstrap processor initialized successfully."
				<< frigg::endLog;

	// Parse the initrd image.
	auto modules = reinterpret_cast<EirModule *>(info->moduleInfo);
	assert(info->numModules == 1);
	
	mfsRoot = frigg::construct<MfsDirectory>(*kernelAlloc);
	{
		assert(modules[0].physicalBase % kPageSize == 0);
		assert(modules[0].length <= 0x1000000);
		auto base = static_cast<const char *>(KernelVirtualMemory::global().allocate(0x1000000));
		for(size_t pg = 0; pg < modules[0].length; pg += kPageSize)
			KernelPageSpace::global().mapSingle4k(reinterpret_cast<VirtualAddr>(base) + pg,
					modules[0].physicalBase + pg, 0);

		struct Header {
			char magic[6];
			char inode[8];
			char mode[8];
			char uid[8];
			char gid[8];
			char numLinks[8];
			char mtime[8];
			char fileSize[8];
			char devMajor[8];
			char devMinor[8];
			char rdevMajor[8];
			char rdevMinor[8];
			char nameSize[8];
			char check[8];
		};

		constexpr uint32_t type_mask = 0170000;
		constexpr uint32_t regular_type = 0100000;
		constexpr uint32_t directory_type = 0040000;

		auto parseHex = [] (const char *c, int n) {
			uint32_t v = 0;
			for(int i = 0; i < n; i++) {
				uint32_t d;
				if(*c >= 'a' && *c <= 'f') {
					d = *c++ - 'a' + 10;
				}else if(*c >= 'A' && *c <= 'F') {
					d = *c++ - 'A' + 10;
				}else if(*c >= '0' && *c <= '9') {
					d = *c++ - '0';
				}else{
					frigg::panicLogger() << "Unexpected character 0x" << frigg::logHex(*c)
							<< " in CPIO header" << frigg::endLog;
					__builtin_unreachable();
				}
				v = (v << 4) | d;
			}
			return v;
		};

		auto p = base;
		auto limit = base + modules[0].length;
		while(true) {
			Header header;
			assert(p + sizeof(Header) <= limit);
			memcpy(&header, p, sizeof(Header));

			auto magic = parseHex(header.magic, 6);
			assert(magic == 0x070701 || magic == 0x070702);

			auto mode = parseHex(header.mode, 8);
			auto name_size = parseHex(header.nameSize, 8);
			auto file_size = parseHex(header.fileSize, 8);
			auto data = p + ((sizeof(Header) + name_size + 3) & ~uint32_t{3});
			
			frigg::StringView path{p + sizeof(Header), name_size - 1};
			if(path == "TRAILER!!!")
				break;

			if((mode & type_mask) == directory_type) {
				frigg::infoLogger() << "thor: initrd directory " << path << frigg::endLog;

				mfsRoot->link(frigg::String<KernelAlloc>{*kernelAlloc, path},
						frigg::construct<MfsDirectory>(*kernelAlloc));
			}else{
				assert((mode & type_mask) == regular_type);
//				if(logInitialization)
					frigg::infoLogger() << "thor: initrd file " << path << frigg::endLog;

				auto memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
						(file_size + (kPageSize - 1)) & ~size_t{kPageSize - 1});
				memory->copyFrom(0, data, file_size);
	
				const char *begin = path.data();
				const char *end = path.data() + path.size();
				auto slash = std::find(begin, end, '/');
				assert(slash != end);
				assert(std::find(slash + 1, end, '/') == end);

				auto parent = mfsRoot->getTarget(path.subString(0, slash - begin));
				assert(parent && parent->type == MfsType::directory);
				auto directory = static_cast<MfsDirectory *>(parent);

				auto name = frigg::String<KernelAlloc>{*kernelAlloc,
						path.subString((slash - begin) + 1, end - (slash + 1))};
				directory->link(std::move(name), frigg::construct<MfsRegular>(*kernelAlloc,
						std::move(memory)));
			}

			p = data + ((file_size + 3) & ~uint32_t{3});
		}
	}

	if(logInitialization)
		frigg::infoLogger() << "thor: Modules are set up successfully."
				<< frigg::endLog;
	
	// create a root universe and run a kernel thread to communicate with the universe 
	rootUniverse.initialize(frigg::makeShared<Universe>(*kernelAlloc));

	auto mbus_stream = createStream();
	mbusClient.initialize(mbus_stream.get<1>());

	// Continue the system initialization.
	initializeBasicSystem();

	KernelFiber::run([=] () mutable {
		// Complete the system initialization.
		initializeExtendedSystem();

		// Launch initial user space programs.
		frigg::infoLogger() << "thor: Launching user space." << frigg::endLog;
		auto mbus_module = resolveModule("sbin/mbus");
		auto posix_module = resolveModule("sbin/posix-subsystem");
		assert(mbus_module && mbus_module->type == MfsType::regular);
		assert(posix_module && posix_module->type == MfsType::regular);
		executeModule(static_cast<MfsRegular *>(mbus_module), mbus_stream.get<0>(), LaneHandle{});
		executeModule(static_cast<MfsRegular *>(posix_module), LaneHandle{}, mbus_stream.get<1>());

		while(true)
			KernelFiber::blockCurrent(frigg::CallbackPtr<bool()>{nullptr,
					[] (void *) { return true; }});
	});

	frigg::infoLogger() << "thor: Entering initilization fiber." << frigg::endLog;
	globalScheduler().reschedule();
}

extern "C" void handleStubInterrupt() {
	frigg::panicLogger() << "Fault or IRQ from stub" << frigg::endLog;
}
extern "C" void handleBadDomain() {
	frigg::panicLogger() << "Fault or IRQ from bad domain" << frigg::endLog;
}

extern "C" void handleDivideByZeroFault(FaultImageAccessor image) {
	(void)image;

	frigg::panicLogger() << "Divide by zero" << frigg::endLog;
}

extern "C" void handleDebugFault(FaultImageAccessor image) {
	frigg::infoLogger() << "Debug fault at "
			<< (void *)*image.ip() << frigg::endLog;
}

extern "C" void handleOpcodeFault(FaultImageAccessor image) {
	(void)image;

	frigg::panicLogger() << "Invalid opcode" << frigg::endLog;
}

extern "C" void handleNoFpuFault(FaultImageAccessor image) {
	frigg::panicLogger() << "FPU invoked at "
			<< (void *)*image.ip() << frigg::endLog;
}

extern "C" void handleDoubleFault(FaultImageAccessor image) {
	frigg::panicLogger() << "Double fault at "
			<< (void *)*image.ip() << frigg::endLog;
}

extern "C" void handleProtectionFault(FaultImageAccessor image) {
	frigg::panicLogger() << "General protection fault\n"
			<< "    Faulting IP: " << (void *)*image.ip() << "\n"
			<< "    Faulting segment: " << (void *)*image.code() << frigg::endLog;
}

void handlePageFault(FaultImageAccessor image, uintptr_t address) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
	frigg::UnsafePtr<AddressSpace> address_space = this_thread->getAddressSpace();

	const Word kPfAccess = 1;
	const Word kPfWrite = 2;
	const Word kPfUser = 4;
	const Word kPfBadTable = 8;
	const Word kPfInstruction = 16;
	assert(!(*image.code() & kPfBadTable));

	uint32_t flags = 0;
	if(*image.code() & kPfWrite)
		flags |= AddressSpace::kFaultWrite;

	bool handled = false;
	if(image.inKernelDomain() && !image.allowUserPages()) {
		frigg::infoLogger() << "\e[31mthor: SMAP fault.\e[39m" << frigg::endLog;
	}else{
		handled = address_space->handleFault(address, flags);
	}
	
	if(handled)
		return;
	
	if(!(*image.code() & kPfUser)
			|| this_thread->flags & Thread::kFlagTrapsAreFatal) {
		auto msg = frigg::panicLogger();
		msg << "Page fault"
				<< " at " << (void *)address
				<< ", faulting ip: " << (void *)*image.ip() << "\n";
		msg << "Errors:";
		if(*image.code() & kPfUser) {
			msg << " (User)";
		}else{
			msg << " (Supervisor)";
		}
		if(*image.code() & kPfAccess) {
			msg << " (Access violation)";
		}else{
			msg << " (Page not present)";
		}
		if(*image.code() & kPfWrite) {
			msg << " (Write)";
		}else if(*image.code() & kPfInstruction) {
			msg << " (Instruction fetch)";
		}else{
			msg << " (Read)";
		}
		msg << frigg::endLog;
	}else{
		Thread::interruptCurrent(Interrupt::kIntrPageFault, image);
	}
}

void handleOtherFault(FaultImageAccessor image, Interrupt fault) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();

	const char *name;
	switch(fault) {
	case kIntrBreakpoint: name = "breakpoint"; break;
	default:
		frigg::panicLogger() << "Unexpected fault code" << frigg::endLog;
	}

	if(this_thread->flags & Thread::kFlagTrapsAreFatal) {
		frigg::infoLogger() << "traps-are-fatal thread killed by " << name << " fault.\n"
				<< "Last ip: " << (void *)*image.ip() << frigg::endLog;
	}else{
		Thread::interruptCurrent(fault, image);
	}
}

void handleIrq(IrqImageAccessor image, int number) {
	assert(!intsAreEnabled());

	if(logEveryIrq)
		frigg::infoLogger() << "IRQ #" << number << frigg::endLog;

	globalIrqSlots[number]->raise();

	if(image.inPreemptibleDomain() && globalScheduler().wantSchedule()) {
		if(image.inThreadDomain()) {
			Thread::deferCurrent(image);
		}else if(image.inFiberDomain()) {
			// TODO: For now we do not defer kernel fibers.
		}else{
			assert(image.inIdleDomain());
			runDetached([] {
				globalScheduler().reschedule();
			});
		}
	}
}

extern "C" void thorImplementNoThreadIrqs() {
	assert(!"Implement no-thread IRQ stubs");
}

void handleSyscall(SyscallImageAccessor image) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
	if(logEverySyscall && *image.number() != kHelCallLog)
		frigg::infoLogger() << this_thread.get()
				<< " syscall #" << *image.number() << frigg::endLog;

	// TODO: The return in this code path prevents us from checking for signals!
	if(*image.number() >= kHelCallSuper) {
		Thread::interruptCurrent(static_cast<Interrupt>(kIntrSuperCall
				+ (*image.number() - kHelCallSuper)), image);
		return;
	}

	Word arg0 = *image.in0();
	Word arg1 = *image.in1();
	Word arg2 = *image.in2();
	Word arg3 = *image.in3();
	Word arg4 = *image.in4();
	Word arg5 = *image.in5();
	Word arg6 = *image.in6();
	Word arg7 = *image.in7();
	Word arg8 = *image.in8();

	switch(*image.number()) {
	case kHelCallLog: {
		*image.error() = helLog((const char *)arg0, (size_t)arg1);
	} break;
	case kHelCallPanic: {
		if(this_thread->flags & Thread::kFlagTrapsAreFatal) {
			frigg::infoLogger() << "User space panic:" << frigg::endLog;
			helLog((const char *)arg0, (size_t)arg1);
		}else{
			Thread::interruptCurrent(kIntrPanic, image);
		}
	} break;

	case kHelCallCreateUniverse: {
		HelHandle handle;
		*image.error() = helCreateUniverse(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallTransferDescriptor: {
		HelHandle out_handle;
		*image.error() = helTransferDescriptor((HelHandle)arg0, (HelHandle)arg1,
				&out_handle);
		*image.out0() = out_handle;
	} break;
	case kHelCallDescriptorInfo: {
		*image.error() = helDescriptorInfo((HelHandle)arg0, (HelDescriptorInfo *)arg1);
	} break;
	case kHelCallCloseDescriptor: {
//		frigg::infoLogger() << "helCloseDescriptor(" << (HelHandle)arg0 << ")" << frigg::endLog;
		*image.error() = helCloseDescriptor((HelHandle)arg0);
	} break;

	case kHelCallAllocateMemory: {
		HelHandle handle;
		*image.error() = helAllocateMemory((size_t)arg0, (uint32_t)arg1, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallCreateManagedMemory: {
		HelHandle backing_handle, frontal_handle;
		*image.error() = helCreateManagedMemory((size_t)arg0, (uint32_t)arg1,
				&backing_handle, &frontal_handle);
		*image.out0() = backing_handle;
		*image.out1() = frontal_handle;
	} break;
	case kHelCallAccessPhysical: {
		HelHandle handle;
		*image.error() = helAccessPhysical((uintptr_t)arg0, (size_t)arg1, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallCreateSpace: {
		HelHandle handle;
		*image.error() = helCreateSpace(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallForkSpace: {
		HelHandle forked;
		*image.error() = helForkSpace((HelHandle)arg0, &forked);
		*image.out0() = forked;
	} break;
	case kHelCallMapMemory: {
		void *actual_pointer;
		*image.error() = helMapMemory((HelHandle)arg0, (HelHandle)arg1,
				(void *)arg2, (uintptr_t)arg3, (size_t)arg4, (uint32_t)arg5, &actual_pointer);
		*image.out0() = (Word)actual_pointer;
	} break;
	case kHelCallUnmapMemory: {
		*image.error() = helUnmapMemory((HelHandle)arg0, (void *)arg1, (size_t)arg2);
	} break;
	case kHelCallPointerPhysical: {
		uintptr_t physical;
		*image.error() = helPointerPhysical((void *)arg0, &physical);
		*image.out0() = physical;
	} break;
	case kHelCallLoadForeign: {
		*image.error() = helLoadForeign((HelHandle)arg0, (uintptr_t)arg1,
				(size_t)arg2, (void *)arg3);
	} break;
	case kHelCallMemoryInfo: {
		size_t size;
		*image.error() = helMemoryInfo((HelHandle)arg0, &size);
		*image.out0() = size;
	} break;
	case kHelCallSubmitManageMemory: {
		*image.error() = helSubmitManageMemory((HelHandle)arg0,
				(HelQueue *)arg1, (uintptr_t)arg2);
	} break;
	case kHelCallCompleteLoad: {
		*image.error() = helCompleteLoad((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2);
	} break;
	case kHelCallSubmitLockMemory: {
		*image.error() = helSubmitLockMemory((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2,
				(HelQueue *)arg3, (uintptr_t)arg4);
	} break;
	case kHelCallLoadahead: {
		*image.error() = helLoadahead((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2);
	} break;

	case kHelCallCreateThread: {
//		frigg::infoLogger() << "[" << this_thread->globalThreadId << "]"
//				<< " helCreateThread()"
//				<< frigg::endLog;
		HelHandle handle;
		*image.error() = helCreateThread((HelHandle)arg0, (HelHandle)arg1,
				(int)arg2, (void *)arg3, (void *)arg4, (uint32_t)arg5, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallYield: {
		*image.error() = helYield();
	} break;
	case kHelCallSubmitObserve: {
		*image.error() = helSubmitObserve((HelHandle)arg0,
				(HelQueue *)arg1, (uintptr_t)arg2);
	} break;
	case kHelCallResume: {
		*image.error() = helResume((HelHandle)arg0);
	} break;
	case kHelCallLoadRegisters: {
		*image.error() = helLoadRegisters((HelHandle)arg0, (int)arg1, (void *)arg2);
	} break;
	case kHelCallStoreRegisters: {
		*image.error() = helStoreRegisters((HelHandle)arg0, (int)arg1, (const void *)arg2);
	} break;
	case kHelCallWriteFsBase: {
		*image.error() = helWriteFsBase((void *)arg0);
	} break;
	case kHelCallGetClock: {
		uint64_t counter;
		*image.error() = helGetClock(&counter);
		*image.out0() = counter;
	} break;
	case kHelCallSubmitAwaitClock: {
		*image.error() = helSubmitAwaitClock((uint64_t)arg0,
				(HelQueue *)arg1, (uintptr_t)arg2);
	} break;

	case kHelCallCreateStream: {
		HelHandle lane1;
		HelHandle lane2;
		*image.error() = helCreateStream(&lane1, &lane2);
		*image.out0() = lane1;
		*image.out1() = lane2;
	} break;
	case kHelCallSubmitAsync: {
		*image.error() = helSubmitAsync((HelHandle)arg0, (HelAction *)arg1,
				(size_t)arg2, (HelQueue *)arg3, (uintptr_t)arg4, (uint32_t)arg5);
	} break;
	
	case kHelCallFutexWait: {
		*image.error() = helFutexWait((int *)arg0, (int)arg1);
	} break;
	case kHelCallFutexWake: {
		*image.error() = helFutexWake((int *)arg0);
	} break;

	case kHelCallAccessIrq: {
		HelHandle handle;
		*image.error() = helAccessIrq((int)arg0, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallAcknowledgeIrq: {
		*image.error() = helAcknowledgeIrq((HelHandle)arg0, (uint32_t)arg1, (uint64_t)arg2);
	} break;
	case kHelCallSubmitAwaitEvent: {
		*image.error() = helSubmitAwaitEvent((HelHandle)arg0, (uint64_t)arg1,
				(HelQueue *)arg2, (uintptr_t)arg3);
	} break;

	case kHelCallAccessIo: {
		HelHandle handle;
		*image.error() = helAccessIo((uintptr_t *)arg0, (size_t)arg1, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallEnableIo: {
		*image.error() = helEnableIo((HelHandle)arg0);
	} break;
	case kHelCallEnableFullIo: {
		*image.error() = helEnableFullIo();
	} break;
	
	default:
		*image.error() = kHelErrIllegalSyscall;
	}

	Thread::raiseSignals(image);

//	frigg::infoLogger() << "exit syscall" << frigg::endLog;
}

} // namespace thor

