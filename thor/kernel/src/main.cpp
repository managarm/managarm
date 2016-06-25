
#include "kernel.hpp"
#include <frigg/elf.hpp>
#include <eir/interface.hpp>

using namespace thor;

// loads an elf image into the current address space
// this is called in kernel mode from the initial user thread
void enterImage(PhysicalAddr image_paddr) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<AddressSpace> space = this_thread->getAddressSpace();

	void *image_ptr = physicalToVirtual(image_paddr);
	
	// parse the ELf file format
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	assert(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	assert(ehdr->e_type == ET_EXEC);

	AddressSpace::Guard space_guard(&space->lock, frigg::dontLock);

	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);
		
		if(phdr->p_type == PT_LOAD) {
			assert(phdr->p_memsz > 0);
			
			// align virtual address and length to page size
			uintptr_t virt_address = phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			auto memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeAllocated);
			memory->resize(virt_length / kPageSize);

			PhysicalChunkAllocator::Guard physical_guard(&physicalAllocator->lock);
			for(size_t i = 0; i < memory->numPages(); i++)
				memory->setPageAt(i * kPageSize,
						physicalAllocator->allocate(physical_guard, 0x1000));
			physical_guard.unlock();
			
			uintptr_t virt_disp = phdr->p_vaddr - virt_address;
			memory->zeroPages();
			memory->copyTo(virt_disp, (void *)((uintptr_t)image_ptr + phdr->p_offset),
					phdr->p_filesz);

			VirtualAddr actual_address;
			space_guard.lock();
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				space->map(space_guard, memory, virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadWrite,
						&actual_address);
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				space->map(space_guard, memory, virt_address, 0, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadExecute,
						&actual_address);
			}else{
				frigg::panicLogger.log() << "Illegal combination of segment permissions"
						<< frigg::EndLog();
			}
			space_guard.unlock();
			thorRtInvalidateSpace();
		}else if(phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			assert(!"Unexpected PHDR");
		}
	}
	
	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stack_memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypeOnDemand);
	stack_memory->resize(stack_size / kPageSize);

	VirtualAddr stack_base;
	space_guard.lock();
	space->map(space_guard, stack_memory, 0, 0, stack_size,
			AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite, &stack_base);
	space_guard.unlock();
	thorRtInvalidateSpace();
	
	infoLogger->log() << "Entering user mode" << frigg::EndLog();
	enterUserMode((void *)(stack_base + stack_size), (void *)ehdr->e_entry);
}

extern "C" void thorMain(PhysicalAddr info_paddr) {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting Thor" << frigg::EndLog();

	initializeProcessorEarly();
	
	auto info = accessPhysical<EirInfo>(info_paddr);
	infoLogger->log() << "Bootstrap memory at "
			<< (void *)info->bootstrapPhysical
			<< ", length: " << (info->bootstrapLength / 1024) << " KiB" << frigg::EndLog();

	physicalAllocator.initialize(info->bootstrapPhysical, info->bootstrapLength);
	physicalAllocator->addChunk(info->bootstrapPhysical, info->bootstrapLength);
	physicalAllocator->bootstrap();

	PhysicalAddr pml4_ptr;
	asm volatile ( "mov %%cr3, %%rax" : "=a" (pml4_ptr) );
	kernelSpace.initialize(pml4_ptr);
	
	kernelVirtualAlloc.initialize();
	kernelAlloc.initialize(*kernelVirtualAlloc);

	for(int i = 0; i < 16; i++)
		irqRelays[i].initialize();

	activeList.initialize();
	scheduleQueue.initialize(*kernelAlloc);
	scheduleLock.initialize();

	initializeTheSystem();
	initializeThisProcessor();
	
	// create a directory and load the memory regions of all modules into it
	assert(info->numModules >= 1);
	auto modules = accessPhysicalN<EirModule>(info->moduleInfo,
			info->numModules);
	
	auto mod_directory = frigg::makeShared<RdFolder>(*kernelAlloc);
	for(size_t i = 1; i < info->numModules; i++) {
		size_t virt_length = modules[i].length + (kPageSize - (modules[i].length % kPageSize));
		assert((virt_length % kPageSize) == 0);

		// TODO: free module memory if it is not used anymore
		auto mod_memory = frigg::makeShared<Memory>(*kernelAlloc, Memory::kTypePhysical);
		mod_memory->resize(virt_length / kPageSize);
		for(size_t j = 0; j < mod_memory->numPages(); j++)
			mod_memory->setPageAt(j * kPageSize, modules[i].physicalBase + j * kPageSize);
		
		auto name_ptr = accessPhysicalN<char>(modules[i].namePtr,
				modules[i].nameLength);
		infoLogger->log() << "Module " << frigg::StringView(name_ptr, modules[i].nameLength)
				<< ", length: " << modules[i].length << frigg::EndLog();

		MemoryAccessDescriptor mod_descriptor(frigg::move(mod_memory));
		mod_directory->publish(name_ptr, modules[i].nameLength,
				AnyDescriptor(frigg::move(mod_descriptor)));
	}
	
	const char *mod_path = "initrd";
	auto root_directory = frigg::makeShared<RdFolder>(*kernelAlloc);
	root_directory->mount(mod_path, strlen(mod_path), frigg::move(mod_directory));

	// finally we lauch the user_boot program
	auto universe = frigg::makeShared<Universe>(*kernelAlloc);
	auto address_space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	address_space->setupDefaultMappings();

	auto thread = frigg::makeShared<Thread>(*kernelAlloc, frigg::move(universe),
			frigg::move(address_space), frigg::move(root_directory));
	thread->flags |= Thread::kFlagExclusive;
	
	auto group = frigg::makeShared<ThreadGroup>(*kernelAlloc);
	ThreadGroup::addThreadToGroup(frigg::move(group), thread);

	// FIXME: do not heap-allocate the state structs
	*thread->image.rdi() = modules[0].physicalBase;
	*thread->image.sp() = (uintptr_t)thread->kernelStack.base();
	*thread->image.ip() = (Word)&enterImage;
	*thread->image.kernel() = 1;
	
	KernelUnsafePtr<Thread> thread_ptr(thread);
	activeList->addBack(frigg::move(thread));
	infoLogger->log() << "Leaving Thor" << frigg::EndLog();
	enterThread(thread_ptr);
}

extern "C" void handleDivideByZeroFault(FaultImagePtr image) {
	frigg::panicLogger.log() << "Divide by zero" << frigg::EndLog();
}

extern "C" void handleDebugFault(FaultImagePtr image) {
	infoLogger->log() << "Debug fault at "
			<< (void *)*image.ip() << frigg::EndLog();
}

extern "C" void handleOpcodeFault(FaultImagePtr image) {
	frigg::panicLogger.log() << "Invalid opcode" << frigg::EndLog();
}

extern "C" void handleNoFpuFault(FaultImagePtr image) {
	frigg::panicLogger.log() << "FPU invoked at "
			<< (void *)*image.ip() << frigg::EndLog();
}

extern "C" void handleDoubleFault(FaultImagePtr image) {
	frigg::panicLogger.log() << "Double fault at "
			<< (void *)*image.ip() << frigg::EndLog();
}

extern "C" void handleProtectionFault(FaultImagePtr image) {
	frigg::panicLogger.log() << "General protection fault\n"
			<< "    Faulting IP: " << (void *)*image.ip() << "\n"
			<< "    Faulting segment: " << (void *)*image.code() << frigg::EndLog();
}

extern "C" void handlePageFault(FaultImagePtr image, Word error) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
	KernelUnsafePtr<AddressSpace> address_space = this_thread->getAddressSpace();

	uintptr_t address;
	asm volatile ( "mov %%cr2, %0" : "=r" (address) );

	const Word kPfAccess = 1;
	const Word kPfWrite = 2;
	const Word kPfUser = 4;
	const Word kPfBadTable = 8;
	const Word kPfInstruction = 16;
	assert(!(*image.code() & kPfBadTable));

	uint32_t flags = 0;
	if(*image.code() & kPfWrite)
		flags |= AddressSpace::kFaultWrite;

	AddressSpace::Guard space_guard(&address_space->lock);
	bool handled = address_space->handleFault(space_guard, address, flags);
	space_guard.unlock();
	
	if(!handled) {
		auto msg = frigg::panicLogger.log();
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
		msg << frigg::EndLog();
	}
}

extern "C" void handleIrq(IrqImagePtr image, int irq) {
	assert(!intsAreEnabled());

	infoLogger->log() << "IRQ #" << irq << frigg::EndLog();
	
	if(irq == 2)
		timerInterrupt();
	
	IrqRelay::Guard irq_guard(&irqRelays[irq]->lock);
	irqRelays[irq]->fire(irq_guard);
	irq_guard.unlock();
}

extern "C" void thorImplementNoThreadIrqs() {
	assert(!"Implement no-thread IRQ stubs");
}

extern "C" void handleSyscall(SyscallImagePtr image) {
	KernelUnsafePtr<Thread> this_thread = getCurrentThread();
//	if(index != kHelCallLog)
//		infoLogger->log() << "syscall #" << index << frigg::EndLog();

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
		infoLogger->log() << "User space panic:" << frigg::EndLog();
		helLog((const char *)arg0, (size_t)arg1);
		
		while(true) { }
	} break;

	case kHelCallDescriptorInfo: {
		*image.error() = helDescriptorInfo((HelHandle)arg0, (HelDescriptorInfo *)arg1);
	} break;
	case kHelCallCloseDescriptor: {
//		infoLogger->log() << "helCloseDescriptor(" << (HelHandle)arg0 << ")" << frigg::EndLog();
		*image.error() = helCloseDescriptor((HelHandle)arg0);
	} break;

	case kHelCallAllocateMemory: {
		HelHandle handle;
		*image.error() = helAllocateMemory((size_t)arg0, (uint32_t)arg1, &handle);
		*image.out0() = handle;
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
	case kHelCallMemoryInfo: {
		size_t size;
		*image.error() = helMemoryInfo((HelHandle)arg0, &size);
		*image.out0() = size;
	} break;
	case kHelCallSubmitProcessLoad: {
		int64_t async_id;
		*image.error() = helSubmitProcessLoad((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallCompleteLoad: {
		*image.error() = helCompleteLoad((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2);
	} break;
	case kHelCallSubmitLockMemory: {
		int64_t async_id;
		*image.error() = helSubmitLockMemory((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (size_t)arg3,
				(uintptr_t)arg4, (uintptr_t)arg5, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallLoadahead: {
		*image.error() = helLoadahead((HelHandle)arg0, (uintptr_t)arg1, (size_t)arg2);
	} break;

	case kHelCallCreateThread: {
//		infoLogger->log() << "[" << this_thread->globalThreadId << "]"
//				<< " helCreateThread()"
//				<< frigg::EndLog();
		HelHandle handle;
		*image.error() = helCreateThread((HelHandle)arg0,
				(HelHandle)arg1, (int)arg2, (void *)arg3, (void *)arg4, (uint32_t)arg5, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallYield: {
		*image.error() = helYield();
	} break;
	case kHelCallSubmitJoin: {
		int64_t async_id;
		*image.error() = helSubmitJoin((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallExitThisThread: {
		*image.error() = helExitThisThread();
	} break;
	case kHelCallWriteFsBase: {
		*image.error() = helWriteFsBase((void *)arg0);
	} break;
	case kHelCallGetClock: {
		uint64_t counter;
		*image.error() = helGetClock(&counter);
		*image.out0() = counter;
	} break;

	case kHelCallCreateSignal: {
		HelHandle handle;
		*image.error() = helCreateSignal((void *)arg0, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallRaiseSignal: {
		*image.error() = helRaiseSignal((HelHandle)arg0);
	} break;
	case kHelCallReturnFromSignal: {
		*image.error() = helReturnFromSignal();
	} break;

	case kHelCallCreateEventHub: {
//			infoLogger->log() << "helCreateEventHub" << frigg::EndLog();
		HelHandle handle;
		*image.error() = helCreateEventHub(&handle);
//			infoLogger->log() << "    -> " << handle << frigg::EndLog();
		*image.out0() = handle;
	} break;
	case kHelCallWaitForEvents: {
//			infoLogger->log() << "helWaitForEvents(" << (HelHandle)arg0
//					<< ", " << (void *)arg1 << ", " << (HelNanotime)arg2
//					<< ", " << (HelNanotime)arg3 << ")" << frigg::EndLog();

		size_t num_items;
		*image.error() = helWaitForEvents((HelHandle)arg0,
				(HelEvent *)arg1, (size_t)arg2, (HelNanotime)arg3,
				&num_items);
		*image.out0() = num_items;
	} break;
	
	case kHelCallCreateRing: {
		HelHandle handle;
		*image.error() = helCreateRing((HelHandle)arg0, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallSubmitRing: {
		int64_t async_id;
		*image.error() = helSubmitRing((HelHandle)arg0, (HelHandle)arg1,
				(HelRingBuffer *)arg2, (size_t)arg3,
				(uintptr_t)arg4, (uintptr_t)arg5, &async_id);
		*image.out0() = async_id;
	} break;

	case kHelCallCreateFullPipe: {
		HelHandle first;
		HelHandle second;
		*image.error() = helCreateFullPipe(&first, &second);
		*image.out0() = first;
		*image.out1() = second;
	} break;
	case kHelCallSubmitSendString: {
		int64_t async_id;
		*image.error() = helSubmitSendString((HelHandle)arg0,
				(HelHandle)arg1, (const void *)arg2, (size_t)arg3,
				(int64_t)arg4, (int64_t)arg5,
				(uintptr_t)arg6, (uintptr_t)arg7, (uint32_t)arg8, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitSendDescriptor: {
		int64_t async_id;
		*image.error() = helSubmitSendDescriptor((HelHandle)arg0,
				(HelHandle)arg1, (HelHandle)arg2,
				(int64_t)arg3, (int64_t)arg4,
				(uintptr_t)arg5, (uintptr_t)arg6, (uint32_t)arg7, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitRecvDescriptor: {
		int64_t async_id;
		*image.error() = helSubmitRecvDescriptor((HelHandle)arg0, (HelHandle)arg1,
				(int64_t)arg2, (int64_t)arg3,
				(uintptr_t)arg4, (uintptr_t)arg5, (uint32_t)arg6, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitRecvString: {
		int64_t async_id;
		*image.error() = helSubmitRecvString((HelHandle)arg0,
				(HelHandle)arg1, (void *)arg2, (size_t)arg3,
				(int64_t)arg4, (int64_t)arg5,
				(uintptr_t)arg6, (uintptr_t)arg7, (uint32_t)arg8, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitRecvStringToRing: {
		int64_t async_id;
		*image.error() = helSubmitRecvStringToRing((HelHandle)arg0,
				(HelHandle)arg1, (HelHandle)arg2,
				(int64_t)arg3, (int64_t)arg4,
				(uintptr_t)arg5, (uintptr_t)arg6, (uint32_t)arg7, &async_id);
		*image.out0() = async_id;
	} break;
	
	case kHelCallCreateServer: {
		HelHandle server_handle;
		HelHandle client_handle;
		*image.error() = helCreateServer(&server_handle, &client_handle);
		*image.out0() = server_handle;
		*image.out1() = client_handle;
	} break;
	case kHelCallSubmitAccept: {
		int64_t async_id;
		*image.error() = helSubmitAccept((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubmitConnect: {
		int64_t async_id;
		*image.error() = helSubmitConnect((HelHandle)arg0, (HelHandle)arg1,
				(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;

	case kHelCallCreateRd: {
		HelHandle handle;
		*image.error() = helCreateRd(&handle);
		*image.out0() = handle;
	} break;
	case kHelCallRdMount: {
		*image.error() = helRdMount((HelHandle)arg0,
				(const char *)arg1, (size_t)arg2, (HelHandle)arg3);
	} break;
	case kHelCallRdPublish: {
		*image.error() = helRdPublish((HelHandle)arg0,
				(const char *)arg1, (size_t)arg2, (HelHandle)arg3);
	} break;
	case kHelCallRdOpen: {
		HelHandle handle;
		*image.error() = helRdOpen((const char *)arg0,
				(size_t)arg1, &handle);
		*image.out0() = handle;
	} break;

	case kHelCallAccessIrq: {
		HelHandle handle;
		*image.error() = helAccessIrq((int)arg0, &handle);
		*image.out0() = handle;
	} break;
	case kHelCallSetupIrq: {
		*image.error() = helSetupIrq((HelHandle)arg0, (uint32_t)arg1);
	} break;
	case kHelCallAcknowledgeIrq: {
		*image.error() = helAcknowledgeIrq((HelHandle)arg0);
	} break;
	case kHelCallSubmitWaitForIrq: {
		int64_t async_id;
		*image.error() = helSubmitWaitForIrq((HelHandle)arg0,
				(HelHandle)arg1, (uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
	} break;
	case kHelCallSubscribeIrq: {
		int64_t async_id;
		*image.error() = helSubscribeIrq((HelHandle)arg0,
				(HelHandle)arg1, (uintptr_t)arg2, (uintptr_t)arg3, &async_id);
		*image.out0() = async_id;
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
	
	case kHelCallControlKernel: {
		int subsystem = (int)arg0;
		int interface = (int)arg1;
		const void *user_input = (const void *)arg2;
		void *user_output = (void *)arg3;

		if(subsystem == kThorSubArch) {
			controlArch(interface, user_input, user_output);
			*image.error() = kHelErrNone;
		}else if(subsystem == kThorSubDebug) {
			if(interface == kThorIfDebugMemory) {
				infoLogger->log() << "Memory info:\n"
						<< "    Physical pages: Used: " << physicalAllocator->numUsedPages()
						<< ", free: " << physicalAllocator->numFreePages() << "\n"
						<< "    kernelAlloc: Used " << kernelAlloc->numUsedPages()
						<< frigg::EndLog();
				*image.error() = kHelErrNone;
			}else{
				assert(!"Illegal debug interface");
			}
		}else{
			assert(!"Illegal subsystem");
		}
	} break;
	default:
		*image.error() = kHelErrIllegalSyscall;
	}
	
	this_thread->issueSignalAfterSyscall();
}

