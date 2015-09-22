
#include "kernel.hpp"
#include <frigg/elf.hpp>
#include "../../hel/include/hel.h"
#include <eir/interface.hpp>

using namespace thor;
namespace debug = frigg::debug;
namespace traits = frigg::traits;
namespace memory = frigg::memory;
namespace util = frigg::util;

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
			
			auto memory = frigg::makeShared<Memory>(*kernelAlloc);
			memory->resize(virt_length);

			VirtualAddr actual_address;
			space_guard.lock();
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				space->map(space_guard, memory, virt_address, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadWrite,
						&actual_address);
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				space->map(space_guard, memory, virt_address, virt_length,
						AddressSpace::kMapFixed | AddressSpace::kMapReadExecute,
						&actual_address);
			}else{
				debug::panicLogger.log() << "Illegal combination of segment permissions"
						<< debug::Finish();
			}
			space_guard.unlock();
			thorRtInvalidateSpace();
			
			uintptr_t virt_disp = phdr->p_vaddr - virt_address;
			memset((void *)virt_address, 0, virt_length);
			memcpy((void *)(virt_address + virt_disp),
					(void *)((uintptr_t)image_ptr + phdr->p_offset),
					phdr->p_filesz);
		}else if(phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			assert(!"Unexpected PHDR");
		}
	}
	
	// allocate and map memory for the user mode stack
	size_t stack_size = 0x200000;
	auto stack_memory = frigg::makeShared<Memory>(*kernelAlloc);
	stack_memory->resize(stack_size);

	VirtualAddr stack_base;
	space_guard.lock();
	space->map(space_guard, stack_memory, 0, stack_size,
			AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite,
			&stack_base);
	space_guard.unlock();
	thorRtInvalidateSpace();
	
	infoLogger->log() << "Entering user mode" << debug::Finish();
	enterUserMode((void *)(stack_base + stack_size), (void *)ehdr->e_entry);
}

extern "C" void thorMain(PhysicalAddr info_paddr) {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting Thor" << debug::Finish();

	initializeProcessorEarly();
	
	auto info = accessPhysical<EirInfo>(info_paddr);
	infoLogger->log() << "Bootstrap memory at "
			<< (void *)info->bootstrapPhysical
			<< ", length: " << (info->bootstrapLength / 1024) << " KiB" << debug::Finish();

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
		auto mod_memory = frigg::makeShared<Memory>(*kernelAlloc);
		for(size_t offset = 0; offset < modules[i].length; offset += 0x1000)
			mod_memory->addPage(modules[i].physicalBase + offset);
		
		auto name_ptr = accessPhysicalN<char>(modules[i].namePtr,
				modules[i].nameLength);
		infoLogger->log() << "Module " << util::StringView(name_ptr, modules[i].nameLength)
				<< ", length: " << modules[i].length << debug::Finish();

		MemoryAccessDescriptor mod_descriptor(traits::move(mod_memory));
		mod_directory->publish(name_ptr, modules[i].nameLength,
				AnyDescriptor(traits::move(mod_descriptor)));
	}
	
	const char *mod_path = "initrd";
	auto root_directory = frigg::makeShared<RdFolder>(*kernelAlloc);
	root_directory->mount(mod_path, strlen(mod_path), traits::move(mod_directory));

	// finally we lauch the user_boot program
	auto universe = frigg::makeShared<Universe>(*kernelAlloc);
	auto address_space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	address_space->setupDefaultMappings();

	auto thread = frigg::makeShared<Thread>(*kernelAlloc, traits::move(universe),
			traits::move(address_space), traits::move(root_directory), true);
	
	uintptr_t stack_ptr = (uintptr_t)thread->accessSaveState().syscallStack
			+ ThorRtThreadState::kSyscallStackSize;
	auto base_state = thread->accessSaveState().accessGeneralBaseState();
	base_state->rdi = modules[0].physicalBase;
	base_state->rsp = stack_ptr;
	base_state->rip = (Word)&enterImage;
	base_state->kernel = 1;
	
	KernelUnsafePtr<Thread> thread_ptr(thread);
	activeList->addBack(traits::move(thread));
	infoLogger->log() << "Leaving Thor" << debug::Finish();
	enterThread(thread_ptr);
}

extern "C" void handleDivideByZeroFault() {
	debug::panicLogger.log() << "Divide by zero" << debug::Finish();
}

extern "C" void handleDebugFault() {
	infoLogger->log() << "Debug fault" << debug::Finish();
}

extern "C" void handleOpcodeFault() {
	debug::panicLogger.log() << "Invalid opcode" << debug::Finish();
}

extern "C" void handleDoubleFault() {
	debug::panicLogger.log() << "Double fault" << debug::Finish();
}

extern "C" void handleProtectionFault(Word error) {
//	auto base_state = getCurrentThread()->accessSaveState().accessGeneralBaseState();
	debug::panicLogger.log() << "General protection fault\n"
//			<< "    Faulting IP: " << (void *)base_state->rip << "\n"
			<< "    Faulting segment: " << (void *)error << debug::Finish();
}

extern "C" void thorKernelPageFault(uintptr_t address,
		uintptr_t fault_ip, Word error) {
	assert((error & 4) == 0);
	assert((error & 8) == 0);
	auto msg = debug::panicLogger.log();
	msg << "Kernel page fault"
			<< " at " << (void *)address
			<< ", faulting ip: " << (void *)fault_ip << "\n";
	msg << "Errors: ";
	if((error & 1) == 0) {
		msg << " (Page not present)";
	}else{
		msg << " (Access violation)";
	}
	if((error & 2) != 0) {
		msg << " (Write)";
	}else if((error & 16) != 0) {
		msg << " (Instruction fetch)";
	}else{
		msg << " (Read)";
	}
	msg << debug::Finish();
}

extern "C" void handlePageFault(Word error, uintptr_t fault_ip) {
	uintptr_t address;
	asm volatile ( "mov %%cr2, %0" : "=r" (address) );

	assert((error & 8) == 0);
	auto msg = debug::panicLogger.log();
	msg << "Page fault"
			<< " at " << (void *)address
			<< ", faulting ip: " << (void *)fault_ip << "\n";
	msg << "Errors:";
	if((error & 4) != 0) {
		msg << " (User)";
	}else{
		msg << " (Supervisor)";
	}
	if((error & 1) == 0) {
		msg << " (Page not present)";
	}else{
		msg << " (Access violation)";
	}
	if((error & 2) != 0) {
		msg << " (Write)";
	}else if((error & 16) != 0) {
		msg << " (Instruction fetch)";
	}else{
		msg << " (Read)";
	}
	msg << debug::Finish();
}

extern "C" void thorIrq(int irq) {
	assert(!intsAreEnabled());

	infoLogger->log() << "IRQ #" << irq << debug::Finish();
	
	if(irq == 2)
		timerInterrupt();
	acknowledgeIrq(irq);
	
	IrqRelay::Guard irq_guard(&irqRelays[irq]->lock);
	irqRelays[irq]->fire(irq_guard);
	irq_guard.unlock();
}

extern "C" void thorImplementNoThreadIrqs() {
	assert(!"Implement no-thread IRQ stubs");
}

extern "C" void thorSyscall(Word index, Word arg0, Word arg1,
		Word arg2, Word arg3, Word arg4, Word arg5,
		Word arg6, Word arg7, Word arg8) {
//	infoLogger->log() << "syscall #" << index << debug::Finish();

	switch(index) {
		case kHelCallLog: {
			HelError error = helLog((const char *)arg0, (size_t)arg1);
			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallPanic: {
			infoLogger->log() << "User space panic:" << debug::Finish();
			helLog((const char *)arg0, (size_t)arg1);
			
			while(true) { }
		}

		case kHelCallDescriptorInfo: {
			HelError error = helDescriptorInfo((HelHandle)arg0, (HelDescriptorInfo *)arg1);
			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallCloseDescriptor: {
			HelError error = helCloseDescriptor((HelHandle)arg0);
			thorRtReturnSyscall1((Word)error);
		}

		case kHelCallAllocateMemory: {
			HelHandle handle;
			HelError error = helAllocateMemory((size_t)arg0, &handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallAccessPhysical: {
			HelHandle handle;
			HelError error = helAccessPhysical((uintptr_t)arg0, (size_t)arg1, &handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallCreateSpace: {
			HelHandle handle;
			HelError error = helCreateSpace(&handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallForkSpace: {
			HelHandle forked;
			HelError error = helForkSpace((HelHandle)arg0, &forked);
			thorRtReturnSyscall2((Word)error, (Word)forked);
		}
		case kHelCallMapMemory: {
			void *actual_pointer;
			HelError error = helMapMemory((HelHandle)arg0, (HelHandle)arg1,
					(void *)arg2, (size_t)arg3, (uint32_t)arg4, &actual_pointer);
			thorRtReturnSyscall2((Word)error, (Word)actual_pointer);
		}
		case kHelCallMemoryInfo: {
			size_t size;
			HelError error = helMemoryInfo((HelHandle)arg0, &size);
			thorRtReturnSyscall2((Word)error, (Word)size);
		}

		case kHelCallCreateThread: {
			HelHandle handle;
			HelError error = helCreateThread((HelHandle)arg0,
					(HelHandle)arg1, (HelThreadState *)arg2, &handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallExitThisThread: {
			HelError error = helExitThisThread();
			thorRtReturnSyscall1((Word)error);
		}

		case kHelCallCreateEventHub: {
//			infoLogger->log() << "helCreateEventHub" << frigg::debug::Finish();
			HelHandle handle;
			HelError error = helCreateEventHub(&handle);

//			infoLogger->log() << "    -> " << handle << frigg::debug::Finish();
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallWaitForEvents: {
//			infoLogger->log() << "helWaitForEvents(" << (HelHandle)arg0
//					<< ", " << (void *)arg1 << ", " << (HelNanotime)arg2
//					<< ", " << (HelNanotime)arg3 << ")" << frigg::debug::Finish();

			size_t num_items;
			HelError error = helWaitForEvents((HelHandle)arg0,
					(HelEvent *)arg1, (size_t)arg2, (HelNanotime)arg3,
					&num_items);
			thorRtReturnSyscall2((Word)error, (Word)num_items);
		}

		case kHelCallCreateBiDirectionPipe: {
			HelHandle first;
			HelHandle second;
			HelError error = helCreateBiDirectionPipe(&first, &second);
			thorRtReturnSyscall3((Word)error, (Word)first, (Word)second);
		}
		case kHelCallSendString: {
			HelError error = helSendString((HelHandle)arg0,
					(const uint8_t *)arg1, (size_t)arg2,
					(int64_t)arg3, (int64_t)arg4);
			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallSendDescriptor: {
			HelError error = helSendDescriptor((HelHandle)arg0, (HelHandle)arg1,
					(int64_t)arg2, (int64_t)arg3);
			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallSubmitRecvDescriptor: {
			int64_t async_id;
			HelError error = helSubmitRecvDescriptor((HelHandle)arg0, (HelHandle)arg1,
					(int64_t)arg2, (int64_t)arg3,
					(uintptr_t)arg4, (uintptr_t)arg5, &async_id);
			thorRtReturnSyscall2((Word)error, (Word)async_id);
		}
		case kHelCallSubmitRecvString: {
			int64_t async_id;
			HelError error = helSubmitRecvString((HelHandle)arg0,
					(HelHandle)arg1, (uint8_t *)arg2, (size_t)arg3,
					(int64_t)arg4, (int64_t)arg5,
					(uintptr_t)arg6, (uintptr_t)arg7, &async_id);
			thorRtReturnSyscall2((Word)error, (Word)async_id);
		}
		
		case kHelCallCreateServer: {
			HelHandle server_handle;
			HelHandle client_handle;
			HelError error = helCreateServer(&server_handle, &client_handle);
			thorRtReturnSyscall3((Word)error, (Word)server_handle, (Word)client_handle);
		}
		case kHelCallSubmitAccept: {
			int64_t async_id;
			HelError error = helSubmitAccept((HelHandle)arg0, (HelHandle)arg1,
					(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
			thorRtReturnSyscall2((Word)error, (Word)async_id);
		}
		case kHelCallSubmitConnect: {
			int64_t async_id;
			HelError error = helSubmitConnect((HelHandle)arg0, (HelHandle)arg1,
					(uintptr_t)arg2, (uintptr_t)arg3, &async_id);
			thorRtReturnSyscall2((Word)error, (Word)async_id);
		}

		case kHelCallCreateRd: {
			HelHandle handle;
			HelError error = helCreateRd(&handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallRdMount: {
			HelError error = helRdMount((HelHandle)arg0,
					(const char *)arg1, (size_t)arg2, (HelHandle)arg3);
			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallRdPublish: {
			HelError error = helRdPublish((HelHandle)arg0,
					(const char *)arg1, (size_t)arg2, (HelHandle)arg3);
			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallRdOpen: {
			HelHandle handle;
			HelError error = helRdOpen((const char *)arg0,
					(size_t)arg1, &handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}

		case kHelCallAccessIrq: {
			HelHandle handle;
			HelError error = helAccessIrq((int)arg0, &handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallSubmitWaitForIrq: {
			int64_t async_id;
			HelError error = helSubmitWaitForIrq((HelHandle)arg0,
					(HelHandle)arg1, (uintptr_t)arg2, (uintptr_t)arg3, &async_id);
			thorRtReturnSyscall2((Word)error, (Word)async_id);
		}

		case kHelCallAccessIo: {
			HelHandle handle;
			HelError error = helAccessIo((uintptr_t *)arg0, (size_t)arg1, &handle);
			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallEnableIo: {
			HelError error = helEnableIo((HelHandle)arg0);
			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallEnableFullIo: {
			HelError error = helEnableFullIo();
			thorRtReturnSyscall1((Word)error);
		}
		
		case kHelCallControlKernel: {
			int subsystem = (int)arg0;
			int interface = (int)arg1;
			const void *user_input = (const void *)arg2;
			void *user_output = (void *)arg3;

			if(subsystem == kThorSubArch) {
				controlArch(interface, user_input, user_output);
				thorRtReturnSyscall1((Word)kHelErrNone);
			}else if(subsystem == kThorSubDebug) {
				assert(!"Illegal debug interface");
			}else{
				assert(!"Illegal subsystem");
			}
		}
		default:
			thorRtReturnSyscall1(kHelErrIllegalSyscall);
	}

	assert(!"No return at end of thorSyscall()");
}

