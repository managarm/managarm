
#include <algorithm>

#include "kernel.hpp"
#include "module.hpp"
#include "irq.hpp"
#include "fiber.hpp"
#include "kerncfg.hpp"
#include "kernlet.hpp"
#include "profile.hpp"
#include "servers.hpp"
#include "service_helpers.hpp"
#include <frg/string.hpp>
#include <frigg/elf.hpp>
#include <eir/interface.hpp>
#include "../system/pci/pci.hpp"
#include "../system/fb.hpp"
#include <arch/x86/ept.hpp>

namespace thor {

static constexpr bool logInitialization = false;
static constexpr bool logEveryPageFault = false;
static constexpr bool logEveryIrq = false;
static constexpr bool logPreemptionIrq = false;
static constexpr bool logEverySyscall = false;

static constexpr bool noScheduleOnIrq = false;

bool debugToVga = false;
bool debugToSerial = false;
bool debugToBochs = false;

frigg::LazyInitializer<IrqSlot> globalIrqSlots[64];

MfsDirectory *mfsRoot;
frigg::LazyInitializer<frg::string<KernelAlloc>> kernelCommandLine;

void setupDebugging();

extern "C" void frg_panic(const char *cstring) {
	frigg::panicLogger() << "frg: Panic! " << cstring << frigg::endLog;
}

frigg::LazyInitializer<frigg::Vector<KernelFiber *, KernelAlloc>> earlyFibers;

extern "C" void thorMain(PhysicalAddr info_paddr) {
	earlyInitializeBootProcessor();

	auto info = reinterpret_cast<EirInfo *>(0x40000000);
	if(info->debugFlags & eirDebugSerial)
		debugToSerial = true;
	if(info->debugFlags & eirDebugBochs)
		debugToBochs = true;
	setupDebugging();

	initializeBootFb(info->frameBuffer.fbAddress, info->frameBuffer.fbPitch,
			info->frameBuffer.fbWidth, info->frameBuffer.fbHeight,
			info->frameBuffer.fbBpp, info->frameBuffer.fbType,
			reinterpret_cast<void *>(info->frameBuffer.fbEarlyWindow));
	
	frigg::infoLogger() << "Starting Thor" << frigg::endLog;

	initializeProcessorEarly();

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
	getCpuData()->globalBinding.bind();

	SkeletalRegion::initialize();
	physicalAllocator.initialize();
	auto region = reinterpret_cast<EirRegion *>(info->regionInfo);
	for(int i = 0; i < info->numRegions; i++)
		physicalAllocator->bootstrapRegion(region[i].address, region[i].order,
				region[i].numRoots, reinterpret_cast<int8_t *>(region[i].buddyTree));
	frigg::infoLogger() << "thor: Number of available pages: "
			<< physicalAllocator->numFreePages() << frigg::endLog;
	
	kernelVirtualAlloc.initialize();
	kernelHeap.initialize(*kernelVirtualAlloc);
	kernelAlloc.initialize(kernelHeap.get());

	initializePhysicalAccess();

	frigg::infoLogger() << "\e[37mthor: Basic memory management is ready\e[39m" << frigg::endLog;

	kernelCommandLine.initialize(*kernelAlloc, reinterpret_cast<const char *>(info->commandLine));
	earlyFibers.initialize(*kernelAlloc);

	for(int i = 0; i < 64; i++)
		globalIrqSlots[i].initialize();

	initializeTheSystemEarly();
	initializeBootProcessor();
	initializeThisProcessor();

	initializeReclaim();

	if(logInitialization)
		frigg::infoLogger() << "thor: Bootstrap processor initialized successfully."
				<< frigg::endLog;

	// Continue the system initialization.
	initializeBasicSystem();

	for(auto it = earlyFibers->begin(); it != earlyFibers->end(); ++it)
		Scheduler::resume(*it);

	// This has to be done after the scheduler is available.
	if(info->debugFlags & eirDebugKernelProfile)
		wantKernelProfile = true;
	initializeProfile();

	KernelFiber::run([=] () mutable {
		// Complete the system initialization.
		initializeExtendedSystem();

		transitionBootFb();

		pci::runAllDevices();

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
						modules[0].physicalBase + pg, 0, CachingMode::null);

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
				
				frg::string_view path{p + sizeof(Header), name_size - 1};
				if(path == "TRAILER!!!")
					break;

				MfsDirectory *dir = mfsRoot;
				const char *it = path.data();
				const char *end = path.data() + path.size();
				while(true) {
					auto slash = std::find(it, end, '/');
					if(slash == end)
						break;

					auto segment = path.sub_string(it - path.data(), slash - it);
					auto child = dir->getTarget(segment);
					assert(child);
					assert(child->type == MfsType::directory);
					it = slash + 1;
					dir = static_cast<MfsDirectory *>(child);
				}

				if((mode & type_mask) == directory_type) {
					// TODO: Get rid of the explicit frigg::String constructor call here.
					frigg::infoLogger() << "thor: initrd directory "
							<< frigg::String<KernelAlloc>{*kernelAlloc, path.data(), path.size()}
							<< frigg::endLog;

					auto name = frg::string<KernelAlloc>{*kernelAlloc,
							path.sub_string(it - path.data(), end - it)};
					dir->link(frg::string<KernelAlloc>{*kernelAlloc, std::move(name)},
							frigg::construct<MfsDirectory>(*kernelAlloc));
				}else{
					assert((mode & type_mask) == regular_type);
	//				if(logInitialization)
						// TODO: Get rid of the explicit frigg::String constructor call here.
						frigg::infoLogger() << "thor: initrd file "
							<< frigg::String<KernelAlloc>{*kernelAlloc, path.data(), path.size()}
							<< frigg::endLog;

					auto memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc,
							(file_size + (kPageSize - 1)) & ~size_t{kPageSize - 1});
					fiberCopyToBundle(memory.get(), 0, data, file_size);
		
					auto name = frg::string<KernelAlloc>{*kernelAlloc,
							path.sub_string(it - path.data(), end - it)};
					dir->link(std::move(name), frigg::construct<MfsRegular>(*kernelAlloc,
							std::move(memory), file_size));
				}

				p = data + ((file_size + 3) & ~uint32_t{3});
			}
		}

		if(logInitialization)
			frigg::infoLogger() << "thor: Modules are set up successfully."
					<< frigg::endLog;
	

		// Launch initial user space programs.
		initializeKerncfg();
		initializeSvrctl();
		frigg::infoLogger() << "thor: Launching user space." << frigg::endLog;
		runMbus();
		initializeKernletCtl();
		runServer("sbin/kernletcc");
		runServer("sbin/clocktracker");
		runServer("sbin/posix-subsystem");
		runServer("sbin/virtio-console");
	});

	frigg::infoLogger() << "thor: Entering initilization fiber." << frigg::endLog;
	localScheduler()->reschedule();
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
	auto address_space = this_thread->getAddressSpace();

	const Word kPfAccess = 1;
	const Word kPfWrite = 2;
	const Word kPfUser = 4;
	const Word kPfBadTable = 8;
	const Word kPfInstruction = 16;
	assert(!(*image.code() & kPfBadTable));

	if(logEveryPageFault) {
		auto msg = frigg::infoLogger();
		msg << "thor: Page fault at " << (void *)address
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
	}

	uint32_t flags = 0;
	if(*image.code() & kPfWrite)
		flags |= AddressSpace::kFaultWrite;
	if(*image.code() & kPfInstruction)
		flags |= AddressSpace::kFaultExecute;

	bool handled = false;
	if(image.inKernelDomain() && !image.allowUserPages()) {
		frigg::infoLogger() << "\e[31mthor: SMAP fault.\e[39m" << frigg::endLog;
	}else{
		// TODO: Make sure that we're in a thread domain.
		WorkScope wqs{this_thread->pagingWorkQueue()};

		struct Closure {
			ThreadBlocker blocker;
			Worklet worklet;
			FaultNode fault;
		} closure;

		// TODO: It is safe to use the thread's WQ here (as PFs never interrupt WQ dequeue).
		// However, it might be desirable to handle PFs on their own WQ.
		closure.worklet.setup([] (Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);
			Thread::unblockOther(&closure->blocker);
		});
		closure.fault.setup(&closure.worklet);
		closure.blocker.setup();
		if(!address_space->handleFault(address, flags, &closure.fault))
			Thread::blockCurrent(&closure.blocker);

		handled = closure.fault.resolved();
	}

	if(handled)
		return;

	if(image.inKernelDomain()) {
		assert(!(*image.code() & kPfUser));
		if(handleUserAccessFault(address, *image.code() & kPfWrite, image))
			return;
	}

	frigg::infoLogger() << "thor: Unhandled page fault"
			<< " at " << (void *)address
			<< ", faulting ip: " << (void *)*image.ip() << frigg::endLog;

	if(!(*image.code() & kPfUser)) {
		auto msg = frigg::panicLogger();
		msg << "\e[31m" "thor: Page fault in kernel, at " << (void *)address
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
		msg << "\e[39m" << frigg::endLog;
	}else if(this_thread->flags & Thread::kFlagServer) {
		auto msg = frigg::infoLogger();
		msg << "\e[31m" "thor: Page fault in server, at " << (void *)address
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
		msg << "\e[39m" << frigg::endLog;
		Thread::interruptCurrent(Interrupt::kIntrPageFault, image);
	}else{
		Thread::interruptCurrent(Interrupt::kIntrPageFault, image);
	}
}

void handleOtherFault(FaultImageAccessor image, Interrupt fault) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();

	const char *name;
	switch(fault) {
	case kIntrBreakpoint: name = "breakpoint"; break;
	case kIntrGeneralFault: name = "general"; break;
	case kIntrIllegalInstruction: name = "illegal-instruction"; break;
	default:
		frigg::panicLogger() << "Unexpected fault code" << frigg::endLog;
	}

	frigg::infoLogger() << "thor: Unhandled " << name << " fault"
			<< ", faulting ip: " << (void *)*image.ip() << frigg::endLog;

	if(this_thread->flags & Thread::kFlagServer) {
		frigg::infoLogger() << "\e[31m" "thor: " << name << " fault in server.\n"
				<< "Last ip: " << (void *)*image.ip() << "\e[39m" << frigg::endLog;
		// TODO: Trigger a more-specific interrupt.
		Thread::interruptCurrent(kIntrPanic, image);
	}else{
		Thread::interruptCurrent(fault, image);
	}
}

void handleIrq(IrqImageAccessor image, int number) {
	assert(!intsAreEnabled());

	if(logEveryIrq)
		frigg::infoLogger() << "thor: IRQ slot #" << number << frigg::endLog;

	globalIrqSlots[number]->raise();
	// TODO: Can this function actually be called from non-preemptible domains?
	assert(image.inPreemptibleDomain());
	if(!noScheduleOnIrq && localScheduler()->wantSchedule()) {
		if(image.inThreadDomain()) {
			if(image.inManipulableDomain()) {
				Thread::suspendCurrent(image);
			}else{
				Thread::deferCurrent(image);
			}
		}else if(image.inFiberDomain()) {
			// TODO: For now we do not defer kernel fibers.
		}else{
			assert(image.inIdleDomain());
			runDetached([] {
				localScheduler()->reschedule();
			});
		}
	}
}

void handlePreemption(IrqImageAccessor image) {
	assert(!intsAreEnabled());

	if(logPreemptionIrq)
		frigg::infoLogger() << "thor: Preemption IRQ" << frigg::endLog;

	// TODO: Can this function actually be called from non-preemptible domains?
	assert(image.inPreemptibleDomain());
	if(localScheduler()->wantSchedule()) {
		if(image.inThreadDomain()) {
			if(image.inManipulableDomain()) {
				Thread::suspendCurrent(image);
			}else{
				Thread::deferCurrent(image);
			}
		}else if(image.inFiberDomain()) {
			// TODO: For now we do not defer kernel fibers.
		}else{
			assert(image.inIdleDomain());
			runDetached([] {
				localScheduler()->reschedule();
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
		frigg::infoLogger() << this_thread.get() << " on CPU " << getLocalApicId()
				<< " syscall #" << *image.number() << frigg::endLog;

	// Run worklets before we run the syscall.
	// This avoids useless FutexWait calls on IPC queues.
	this_thread->mainWorkQueue()->run();

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

	switch(*image.number()) {
	case kHelCallLog: {
		*image.error() = helLog((const char *)arg0, (size_t)arg1);
	} break;
	case kHelCallPanic: {
		Thread::interruptCurrent(kIntrPanic, image);
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
	case kHelCallGetCredentials: {
		*image.error() = helGetCredentials((HelHandle)arg0, (uint32_t)arg1, (char *)arg2);
	} break;
	case kHelCallCloseDescriptor: {
		*image.error() = helCloseDescriptor((HelHandle)arg0, (HelHandle)arg1);
	} break;

	case kHelCallCreateQueue: {
		HelHandle handle;
		*image.error() = helCreateQueue((HelQueue *)arg0, (uint32_t)arg1,
				(unsigned int)arg2, (size_t)arg3, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallSetupChunk: {
		*image.error() = helSetupChunk((HelHandle)arg0, (int)arg1,
				(HelChunk *)arg2, (uint32_t)arg3);
	} break;
	case kHelCallCancelAsync: {
		*image.error() = helCancelAsync((HelHandle)arg0, (uint64_t)arg1);
	} break;

	case kHelCallAllocateMemory: {
		HelHandle handle;
		*image.error() = helAllocateMemory((size_t)arg0, (uint32_t)arg1,
				(HelAllocRestrictions *)arg2, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallResizeMemory: {
		*image.error() = helResizeMemory((HelHandle)arg0, (size_t)arg1);
	} break;
	case kHelCallCreateManagedMemory: {
		HelHandle backing_handle, frontal_handle;
		*image.error() = helCreateManagedMemory((size_t)arg0, (uint32_t)arg1,
				&backing_handle, &frontal_handle);
		*image.out0() = backing_handle;
		*image.out1() = frontal_handle;
	} break;
	case kHelCallCopyOnWrite: {
		HelHandle handle;
		*image.error() = helCopyOnWrite((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallAccessPhysical: {
		HelHandle handle;
		*image.error() = helAccessPhysical((uintptr_t)arg0, (size_t)arg1, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallCreateIndirectMemory: {
		HelHandle handle;
		*image.error() = helCreateIndirectMemory((size_t)arg0, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallAlterMemoryIndirection: {
		*image.error() = helAlterMemoryIndirection((HelHandle)arg0, (size_t)arg1,
				(HelHandle)arg2, (uintptr_t)arg3, (size_t)arg4);
	} break;
	case kHelCallCreateSliceView: {
		HelHandle handle;
		*image.error() = helCreateSliceView((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2,
				(uint32_t)arg3, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallForkMemory: {
		HelHandle forkedHandle;
		*image.error() = helForkMemory((HelHandle)arg0, &forkedHandle);
		*image.out0() = forkedHandle;
	} break;
	case kHelCallCreateSpace: {
		HelHandle handle;
		*image.error() = helCreateSpace(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallMapMemory: {
		void *actual_pointer;
		*image.error() = helMapMemory((HelHandle)arg0, (HelHandle)arg1,
				(void *)arg2, (uintptr_t)arg3, (size_t)arg4, (uint32_t)arg5, &actual_pointer);
		*image.out0() = (Word)actual_pointer;
	} break;
	case kHelCallSubmitProtectMemory: {
		*image.error() = helSubmitProtectMemory((HelHandle)arg0,
				(void *)arg1, (size_t)arg2, (uint32_t)arg3, (HelHandle)arg4, (uintptr_t)arg5);
	} break;
	case kHelCallUnmapMemory: {
		*image.error() = helUnmapMemory((HelHandle)arg0, (void *)arg1, (size_t)arg2);
	} break;
	case kHelCallSubmitSynchronizeSpace: {
		*image.error() = helSubmitSynchronizeSpace((HelHandle)arg0, (void *)arg1, (size_t)arg2,
				(HelHandle)arg3, (uintptr_t)arg4);
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
	case kHelCallStoreForeign: {
		*image.error() = helStoreForeign((HelHandle)arg0, (uintptr_t)arg1,
				(size_t)arg2, (const void *)arg3);
	} break;
	case kHelCallMemoryInfo: {
		size_t size;
		*image.error() = helMemoryInfo((HelHandle)arg0, &size);
		*image.out0() = size;
	} break;
	case kHelCallSubmitManageMemory: {
		*image.error() = helSubmitManageMemory((HelHandle)arg0,
				(HelHandle)arg1, (uintptr_t)arg2);
	} break;
	case kHelCallUpdateMemory: {
		*image.error() = helUpdateMemory((HelHandle)arg0, (int)arg1,
				(uintptr_t)arg2, (size_t)arg3);
	} break;
	case kHelCallSubmitLockMemoryView: {
		*image.error() = helSubmitLockMemoryView((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2,
				(HelHandle)arg3, (uintptr_t)arg4);
	} break;
	case kHelCallLoadahead: {
		*image.error() = helLoadahead((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2);
	} break;
	case kHelCallCreateVirtualizedSpace: {
		HelHandle handle;
		*image.error() = helCreateVirtualizedSpace(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallCreateVirtualizedCpu: {
		HelHandle handle;
		*image.error() = helCreateVirtualizedCpu((HelHandle)arg0, &handle);
		*image.out0() = handle;
		break;
	}
	case kHelCallRunVirtualizedCpu: {
		*image.error() = helRunVirtualizedCpu((HelHandle)arg0, (HelVmexitReason*)arg1);
		break;
	}
	case kHelCallCreateThread: {
//		frigg::infoLogger() << "[" << this_thread->globalThreadId << "]"
//				<< " helCreateThread()"
//				<< frigg::endLog;
		HelHandle handle;
		*image.error() = helCreateThread((HelHandle)arg0, (HelHandle)arg1,
				(int)arg2, (void *)arg3, (void *)arg4, (uint32_t)arg5, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallQueryThreadStats: {
		*image.error() = helQueryThreadStats((HelHandle)arg0, (HelThreadStats *)arg1);
	} break;
	case kHelCallSetPriority: {
		*image.error() = helSetPriority((HelHandle)arg0, (int)arg1);
	} break;
	case kHelCallYield: {
		*image.error() = helYield();
	} break;
	case kHelCallSubmitObserve: {
		*image.error() = helSubmitObserve((HelHandle)arg0, (uint64_t)arg1,
				(HelHandle)arg2, (uintptr_t)arg3);
	} break;
	case kHelCallKillThread: {
		*image.error() = helKillThread((HelHandle)arg0);
	} break;
	case kHelCallInterruptThread: {
		*image.error() = helInterruptThread((HelHandle)arg0);
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
		uint64_t async_id;
		*image.error() = helSubmitAwaitClock((uint64_t)arg0,
				(HelHandle)arg1, (uintptr_t)arg2, &async_id);
		*image.out0() = async_id;
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
				(size_t)arg2, (HelHandle)arg3, (uintptr_t)arg4, (uint32_t)arg5);
	} break;
	case kHelCallShutdownLane: {
		*image.error() = helShutdownLane((HelHandle)arg0);
	} break;

	case kHelCallFutexWait: {
		*image.error() = helFutexWait((int *)arg0, (int)arg1, (int64_t)arg2);
	} break;
	case kHelCallFutexWake: {
		*image.error() = helFutexWake((int *)arg0);
	} break;

	case kHelCallCreateOneshotEvent: {
		HelHandle handle;
		*image.error() = helCreateOneshotEvent(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallCreateBitsetEvent: {
		HelHandle handle;
		*image.error() = helCreateBitsetEvent(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallRaiseEvent: {
		*image.error() = helRaiseEvent((HelHandle)arg0);
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
				(HelHandle)arg2, (uintptr_t)arg3);
	} break;
	case kHelCallAutomateIrq: {
		*image.error() = helAutomateIrq((HelHandle)arg0, (uint32_t)arg1, (HelHandle)arg2);
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

	case kHelCallBindKernlet: {
		HelHandle bound_handle;
		*image.error() = helBindKernlet((HelHandle)arg0, (const HelKernletData *)arg1,
				(size_t)arg2, &bound_handle);
		*image.out0() = bound_handle;
	} break;

	case kHelCallSetAffinity: {
		*image.error() = helSetAffinity((HelHandle)arg0, (uint8_t *)arg1, (size_t)arg2);
	} break;

	default:
		*image.error() = kHelErrIllegalSyscall;
	}

	// Run more worklets that were posted by the syscall.
	this_thread->mainWorkQueue()->run();

	Thread::raiseSignals(image);

//	frigg::infoLogger() << "exit syscall" << frigg::endLog;
}

} // namespace thor

