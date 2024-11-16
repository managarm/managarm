#include <algorithm>
#include <eir/interface.hpp>
#include <frg/string.hpp>
#include <elf.h>
#include <hel.h>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/framebuffer/fb.hpp>
#include <initgraph.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/kerncfg.hpp>
#include <thor-internal/kernlet.hpp>
#include <thor-internal/kernel-log.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/module.hpp>
#include <thor-internal/pci/pci.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/profile.hpp>
#include <thor-internal/random.hpp>
#include <thor-internal/servers.hpp>
#include <thor-internal/thread.hpp>

#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

static constexpr bool logInitialization = false;
static constexpr bool logEveryPageFault = false;
static constexpr bool logUnhandledPageFaults = false;
static constexpr bool logEveryIrq = false;
static constexpr bool logOtherFaults = false;
static constexpr bool logPreemptionIrq = false;
static constexpr bool logEverySyscall = false;

static constexpr bool noScheduleOnIrq = false;

bool debugToSerial = false;
bool debugToBochs = false;

frg::manual_box<IrqSlot> globalIrqSlots[numIrqSlots];
IrqSpinlock globalIrqSlotsLock;

MfsDirectory *mfsRoot;
frg::manual_box<frg::string<KernelAlloc>> kernelCommandLine;

void setupDebugging();

extern "C" void frg_panic(const char *cstring) {
	panicLogger() << "frg: Panic! " << cstring << frg::endlog;
}

extern "C" EirInfo *thorBootInfoPtr;

using InitializerPtr = void (*)();
extern "C" InitializerPtr __init_array_start[];
extern "C" InitializerPtr __init_array_end[];

// This function performs early initialization.
// It is called *before* running global constructors.
extern "C" void thorInitialize() {
	initializeArchitecture();

	if(thorBootInfoPtr->debugFlags & eirDebugSerial)
		debugToSerial = true;
	if(thorBootInfoPtr->debugFlags & eirDebugBochs)
		debugToBochs = true;
	setupDebugging();

	initializeBootFb(thorBootInfoPtr->frameBuffer.fbAddress, thorBootInfoPtr->frameBuffer.fbPitch,
			thorBootInfoPtr->frameBuffer.fbWidth, thorBootInfoPtr->frameBuffer.fbHeight,
			thorBootInfoPtr->frameBuffer.fbBpp, thorBootInfoPtr->frameBuffer.fbType,
			reinterpret_cast<void *>(thorBootInfoPtr->frameBuffer.fbEarlyWindow));

	infoLogger() << "Starting Thor" << frg::endlog;

	if(thorBootInfoPtr->signature == eirSignatureValue) {
		infoLogger() << "thor: Bootstrap information signature matches" << frg::endlog;
	}else{
		panicLogger() << "thor: Bootstrap information signature mismatch!" << frg::endlog;
	}

	KernelPageSpace::initialize();

	physicalAllocator.initialize();
	auto region = reinterpret_cast<EirRegion *>(thorBootInfoPtr->regionInfo);
	for(size_t i = 0; i < thorBootInfoPtr->numRegions; i++)
		physicalAllocator->bootstrapRegion(region[i].address, region[i].order,
				region[i].numRoots, reinterpret_cast<int8_t *>(region[i].buddyTree));
	infoLogger() << "thor: Number of available pages: "
			<< physicalAllocator->numFreePages() << frg::endlog;

	kernelVirtualAlloc.initialize();
	kernelHeap.initialize(*kernelVirtualAlloc);
	kernelAlloc.initialize(kernelHeap.get());

	infoLogger() << "thor: Basic memory management is ready" << frg::endlog;

	initializeAsidContext(getCpuData());
}

extern "C" void thorRunConstructors() {
	infoLogger() << "There are "
			<< (__init_array_end - __init_array_start) << " constructors" << frg::endlog;
	for(InitializerPtr *p = __init_array_start; p != __init_array_end; ++p)
			(*p)();
}

// GlobalInitEngine implementation.

static constexpr bool printDotAnnotations = false;

void GlobalInitEngine::onRealizeNode(initgraph::Node *node) {
	if(node->type() == initgraph::NodeType::stage) {
		infoLogger() << "thor: Registering stage " << node->displayName()
				<< frg::endlog;
	}else if(node->type() == initgraph::NodeType::task) {
		infoLogger() << "thor: Registering task " << node->displayName()
				<< frg::endlog;
	}

	if(printDotAnnotations) {
		if(node->type() == initgraph::NodeType::stage) {
			infoLogger() << "thor, initgraph.dot: n" << node
					<< " [label=\"" << node->displayName() << "\", shape=box];" << frg::endlog;
		}else if(node->type() == initgraph::NodeType::task) {
			infoLogger() << "thor, initgraph.dot: n" << node
					<< " [label=\"" << node->displayName() << "\"];" << frg::endlog;
		}
	}
}

void GlobalInitEngine::onRealizeEdge(initgraph::Edge *edge) {
	if(printDotAnnotations)
		infoLogger() << "thor, initgraph.dot: n" << edge->source()
				<< " -> n" << edge->target() << ";" << frg::endlog;
}

void GlobalInitEngine::preActivate(initgraph::Node *node) {
	if(node->type() == initgraph::NodeType::task)
		infoLogger() << "thor: Running task " << node->displayName()
				<< frg::endlog;
}

void GlobalInitEngine::postActivate(initgraph::Node *node) {
	if(node->type() == initgraph::NodeType::stage)
		infoLogger() << "thor: Reached stage " << node->displayName()
				<< frg::endlog;
}

void GlobalInitEngine::reportUnreached(initgraph::Node *node) {
	if(node->type() == initgraph::NodeType::stage)
		infoLogger() << "thor: Initialization stage "
				<< node->displayName() << " could not be reached" << frg::endlog;
}

void GlobalInitEngine::onUnreached() {
	panicLogger() << "thor: There are initialization nodes"
			" that could not be reached (circular dependencies?)" << frg::endlog;
}

constinit GlobalInitEngine globalInitEngine;

initgraph::Stage *getTaskingAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "tasking-available"};
	return &s;
}

// Since we boot on a fiber, fibers must be available before we enter tasking mode.
initgraph::Edge fibersTaskingEdge{
	getFibersAvailableStage(),
	getTaskingAvailableStage()
};

extern "C" void thorMain() {
	kernelCommandLine.initialize(*kernelAlloc,
			reinterpret_cast<const char *>(thorBootInfoPtr->commandLine));

	for(int i = 0; i < numIrqSlots; i++)
		globalIrqSlots[i].initialize();

	// Run the initgraph tasks that we need for tasking.
	globalInitEngine.run(getTaskingAvailableStage());

	initializeRandom();

	if(logInitialization)
		infoLogger() << "thor: Bootstrap processor initialized successfully."
				<< frg::endlog;

	// This has to be done after the scheduler is available.
	if(thorBootInfoPtr->debugFlags & eirDebugKernelProfile)
		wantKernelProfile = true;
	initializeProfile();

	KernelFiber::run([=] () mutable {
		// Complete the system initialization.
		initializeMbusStream();

		// Run all other initgraph tasks.
		globalInitEngine.run();

		transitionBootFb();

		pci::runAllBridges();
		pci::runAllDevices();

		// Parse the initrd image.
		auto modules = reinterpret_cast<EirModule *>(thorBootInfoPtr->moduleInfo);

		mfsRoot = frg::construct<MfsDirectory>(*kernelAlloc);
		{
			assert(modules[0].physicalBase % kPageSize == 0);
			auto base = static_cast<const char *>(KernelVirtualMemory::global().allocate(
					modules[0].length));
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
						panicLogger() << "Unexpected character 0x" << frg::hex_fmt(*c)
								<< " in CPIO header" << frg::endlog;
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
					infoLogger() << "thor: initrd directory " << path << frg::endlog;

					auto name = frg::string<KernelAlloc>{*kernelAlloc,
							path.sub_string(it - path.data(), end - it)};
					dir->link(frg::string<KernelAlloc>{*kernelAlloc, std::move(name)},
							frg::construct<MfsDirectory>(*kernelAlloc));
				}else{
					assert((mode & type_mask) == regular_type);
	//				if(logInitialization)
						debugLogger() << "thor: initrd file " << path << frg::endlog;

					auto memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc,
							(file_size + (kPageSize - 1)) & ~size_t{kPageSize - 1});
					memory->selfPtr = memory;
					auto copyOutcome = KernelFiber::asyncBlockCurrent(memory->copyTo(0,
							data, file_size,
							thisFiber()->associatedWorkQueue()->take()));
					assert(copyOutcome);

					auto name = frg::string<KernelAlloc>{*kernelAlloc,
							path.sub_string(it - path.data(), end - it)};
					dir->link(std::move(name), frg::construct<MfsRegular>(*kernelAlloc,
							std::move(memory), file_size));
				}

				p = data + ((file_size + 3) & ~uint32_t{3});
			}
		}

		if(logInitialization)
			infoLogger() << "thor: Modules are set up successfully."
					<< frg::endlog;

		// Launch initial user space programs.
		initializeKerncfg();
		initializeSvrctl();
		infoLogger() << "thor: Launching user space." << frg::endlog;
		KernelFiber::asyncBlockCurrent(runMbus());
		initializeKernletCtl();
		KernelFiber::asyncBlockCurrent(runServer("sbin/kernletcc"));
		KernelFiber::asyncBlockCurrent(runServer("sbin/clocktracker"));
		KernelFiber::asyncBlockCurrent(runServer("sbin/posix-subsystem"));
		KernelFiber::asyncBlockCurrent(runServer("sbin/virtio-console"));
	});

	Scheduler::resume(getCpuData()->wqFiber);

	infoLogger() << "thor: Entering initilization fiber." << frg::endlog;
	localScheduler()->update();
	localScheduler()->forceReschedule();
	localScheduler()->commitReschedule();
}

void handlePageFault(FaultImageAccessor image, uintptr_t address, Word errorCode) {
	smarter::borrowed_ptr<Thread> this_thread = getCurrentThread();
	auto address_space = this_thread->getAddressSpace();

	assert(!(errorCode & kPfBadTable));

	auto logFault = [&] {
		auto msg = infoLogger();
		msg << "thor: Page fault at " << (void *)address
				<< ", faulting ip: " << (void *)*image.ip() << "\n";
		msg << "Errors:";
		if(errorCode & kPfUser) {
			msg << " (User)";
		}else{
			msg << " (Supervisor)";
		}
		if(errorCode & kPfAccess) {
			msg << " (Access violation)";
		}else{
			msg << " (Page not present)";
		}
		if(errorCode & kPfWrite) {
			msg << " (Write)";
		}else if(errorCode & kPfInstruction) {
			msg << " (Instruction fetch)";
		}else{
			msg << " (Read)";
		}
		msg << frg::endlog;
	};

	if(logEveryPageFault)
		logFault();

	// Panic on SMAP violations.
	if(image.inKernelDomain()) {
		assert(!(errorCode & kPfUser));

		if(!image.allowUserPages()) {
			if(!logEveryPageFault)
				logFault();
			panicLogger() << "thor: SMAP fault." << frg::endlog;
		}
	}else{
		assert(errorCode & kPfUser);
	}

	// Try to handle the page fault.
	uint32_t flags = 0;
	if(errorCode & kPfWrite)
		flags |= AddressSpace::kFaultWrite;
	if(errorCode & kPfInstruction)
		flags |= AddressSpace::kFaultExecute;

	auto wq = this_thread->pagingWorkQueue();
	if(Thread::asyncBlockCurrent(
			address_space->handleFault(address, flags, wq->take()), wq))
		return;

	// If we get here, the page fault could not be handled.

	if(logUnhandledPageFaults) {
		infoLogger() << "thor: Unhandled page fault"
				<< " at " << (void *)address
				<< ", faulting ip: " << (void *)*image.ip() << frg::endlog;
		logFault();
	}

	// Let the UAR error out if it is active.
	// Otherwise, panic on page faults in the kernel.
	if(image.inKernelDomain()) {
		if(handleUserAccessFault(address, errorCode & kPfWrite, image))
			return;

		if(!logEveryPageFault)
			logFault();
		panicLogger() << "thor: Page fault in kernel, at " << (void *)address
				<< ", faulting ip: " << (void *)*image.ip() << frg::endlog;
	}

	// Otherwise, interrupt the current thread.
	if(this_thread->flags & Thread::kFlagServer) {
		if(!logEveryPageFault)
			logFault();
		urgentLogger() << "thor: Page fault in server, at " << (void *)address
				<< ", faulting ip: " << (void *)*image.ip() << frg::endlog;
	}
	Thread::interruptCurrent(Interrupt::kIntrPageFault, image);
}

void handleOtherFault(FaultImageAccessor image, Interrupt fault) {
	smarter::borrowed_ptr<Thread> this_thread = getCurrentThread();

	const char *name;
	switch(fault) {
	case kIntrDivByZero: name = "div-by-zero"; break;
	case kIntrBreakpoint: name = "breakpoint"; break;
	case kIntrGeneralFault: name = "general"; break;
	case kIntrIllegalInstruction: name = "illegal-instruction"; break;
	default:
		panicLogger() << "Unexpected fault code" << frg::endlog;
	}

	if(logOtherFaults)
		infoLogger() << "thor: Unhandled " << name << " fault"
				<< ", faulting ip: " << (void *)*image.ip() << frg::endlog;

	if(this_thread->flags & Thread::kFlagServer) {
		urgentLogger() << "thor: " << name << " fault in server.\n"
				<< "Last ip: " << (void *)*image.ip() << frg::endlog;
		// TODO: Trigger a more-specific interrupt.
		Thread::interruptCurrent(kIntrPanic, image);
	}else{
		Thread::interruptCurrent(fault, image);
	}
}

void handleIrq(IrqImageAccessor image, int number) {
	assert(!intsAreEnabled());
	auto cpuData = getCpuData();

	if(logEveryIrq)
		infoLogger() << "thor: IRQ slot #" << number << frg::endlog;

	globalIrqSlots[number]->raise();

	// Inject IRQ timing entropy into the PRNG accumulator.
	// Since we track the sequence number per CPU, we also include the CPU number.
	uint64_t tsc = getRawTimestampCounter();
	uint8_t entropy[6];
	entropy[0] = number;
	entropy[1] = cpuData->cpuIndex;
	entropy[2] = tsc & 0xFF; // Assumption: only the low 32 bits contain entropy.
	entropy[3] = (tsc >> 8) & 0xFF;
	entropy[4] = (tsc >> 16) & 0xFF;
	entropy[5] = (tsc >> 24) & 0xFF;
	injectEntropy(entropySrcIrqs, cpuData->irqEntropySeq++, entropy, 6);

	assert(image.inPreemptibleDomain());
	if(!noScheduleOnIrq)
		localScheduler()->currentRunnable()->handlePreemption(image);
}

void handlePreemption(IrqImageAccessor image) {
	assert(!intsAreEnabled());

	if(logPreemptionIrq)
		infoLogger() << "thor: Preemption IRQ" << frg::endlog;

	assert(image.inPreemptibleDomain());
	localScheduler()->currentRunnable()->handlePreemption(image);
}

void handleSyscall(SyscallImageAccessor image) {
	smarter::borrowed_ptr<Thread> this_thread = getCurrentThread();
	auto cpuData = getCpuData();
	if(logEverySyscall && *image.number() != kHelCallLog)
		infoLogger() << this_thread.get() << " on CPU " << cpuData->cpuIndex
				<< " syscall #" << *image.number() << frg::endlog;

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
		*image.error() = helLog((HelLogSeverity)arg0, (const char *)arg1, (size_t)arg2);
	} break;
	case kHelCallPanic: {
		Thread::interruptCurrent(kIntrPanic, image);
	} break;

	case kHelCallNop: {
		*image.error() = helNop();
	} break;
	case kHelCallSubmitAsyncNop: {
		*image.error() = helSubmitAsyncNop((HelHandle)arg0, (uintptr_t)arg1);
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
		*image.error() = helCreateQueue((HelQueueParameters *)arg0, &handle);
		*image.out0() = handle;
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
	case kHelCallSubmitReadMemory: {
		*image.error() = helSubmitReadMemory((HelHandle)arg0, (uintptr_t)arg1,
				(size_t)arg2, (void *)arg3,
				(HelHandle)arg4, (uintptr_t)arg5);
	} break;
	case kHelCallSubmitWriteMemory: {
		*image.error() = helSubmitWriteMemory((HelHandle)arg0, (uintptr_t)arg1,
				(size_t)arg2, (const void *)arg3,
				(HelHandle)arg4, (uintptr_t)arg5);
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
	case kHelCallGetRandomBytes: {
		size_t actualSize;
		*image.error() = helGetRandomBytes((void *)arg0, (size_t)arg1, &actualSize);
		*image.out0() = actualSize;
	} break;
	case kHelCallCreateThread: {
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
	case kHelCallReadFsBase: {
		void *pointer;
		*image.error() = helReadFsBase(&pointer);
		*image.out0() = (Word)pointer;
	} break;
	case kHelCallWriteGsBase: {
		*image.error() = helWriteGsBase((void *)arg0);
	} break;
	case kHelCallReadGsBase: {
		void *pointer;
		*image.error() = helReadGsBase(&pointer);
		*image.out0() = (Word)pointer;
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
		*image.error() = helCreateStream(&lane1, &lane2, (uint32_t) arg0);
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

	case kHelCallGetAffinity: {
		*image.error() = helGetAffinity((HelHandle)arg0, (uint8_t *)arg1, (size_t)arg2, (size_t*)arg3);
	} break;
	case kHelCallSetAffinity: {
		*image.error() = helSetAffinity((HelHandle)arg0, (uint8_t *)arg1, (size_t)arg2);
	} break;
	case kHelCallGetCurrentCpu: {
		int cpu;
		*image.error() = helGetCurrentCpu(&cpu);
		*image.out0() = (Word)cpu;
	} break;

	case kHelCallQueryRegisterInfo: {
		*image.error() = helQueryRegisterInfo((int)arg0, (HelRegisterInfo *)arg1);
	} break;

	case kHelCallCreateToken: {
		HelHandle handle;
		*image.error() = helCreateToken(&handle);
		*image.out0() = handle;
	} break;

	default:
		*image.error() = kHelErrIllegalSyscall;
	}

	// Run more worklets that were posted by the syscall.
	this_thread->mainWorkQueue()->run();

	Thread::raiseSignals(image);

//	infoLogger() << "exit syscall" << frg::endlog;
}

} // namespace thor
