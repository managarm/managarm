
#include "kernel.hpp"
#include "../../frigg/include/elf.hpp"
#include "../../hel/include/hel.h"
#include <eir/interface.hpp>

using namespace thor;
namespace debug = frigg::debug;
namespace traits = frigg::traits;

//FIXME: LazyInitializer<debug::VgaScreen> vgaScreen;
//LazyInitializer<debug::Terminal> vgaTerminal;

uint64_t ldBaseAddr = 0x40000000;
	
void *loadInitImage(UnsafePtr<AddressSpace, KernelAlloc> space, uintptr_t image_page) {
	char *image = (char *)physicalToVirtual(image_page);

	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image;
	ASSERT(ehdr->e_ident[0] == '\x7F'
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	ASSERT(ehdr->e_type == ET_DYN);

	for(int i = 0; i < ehdr->e_phnum; i++) {
		Elf64_Phdr *phdr = (Elf64_Phdr*)(image + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type != PT_LOAD)
			continue;

		uintptr_t bottom = phdr->p_vaddr;
		uintptr_t top = phdr->p_vaddr + phdr->p_memsz;

		if(bottom == top)
			continue;
		
		size_t page_size = 0x1000;
		uintptr_t bottom_page = bottom / page_size;
		uintptr_t top_page = top / page_size;
		uintptr_t num_pages = top_page - bottom_page;
		if(top % page_size != 0)
			num_pages++;

		auto memory = makeShared<Memory>(*kernelAlloc);
		memory->resize(num_pages * page_size);

		uint32_t map_flags = AddressSpace::kMapFixed;
		if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
			map_flags |= AddressSpace::kMapReadWrite;
		}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
			map_flags |= AddressSpace::kMapReadExecute;
		}else{
			debug::panicLogger.log() << "Illegal combination of segment permissions"
					<< debug::Finish();
		}

		VirtualAddr actual_address;
		space->map(memory, ldBaseAddr + bottom_page * page_size,
				num_pages * page_size, map_flags, &actual_address);

		for(uintptr_t page = 0; page < num_pages; page++) {
			PhysicalAddr physical = memory->getPage(page);
			for(size_t p = 0; p < page_size; p++)
				*((char *)physicalToVirtual(physical) + p) = 0;
		}

		for(size_t p = 0; p < phdr->p_filesz; p++) {
			uintptr_t page = (phdr->p_vaddr + p) / page_size - bottom_page;
			uintptr_t virt_offset = (phdr->p_vaddr + p) % page_size;
			
			PhysicalAddr physical = memory->getPage(page);
			char *ptr = (char *)physicalToVirtual(physical);
			*(ptr + virt_offset) = *(image + phdr->p_offset + p);
		}
	}
	
	return (void *)(ldBaseAddr + ehdr->e_entry);
}

extern "C" void thorMain(PhysicalAddr info_paddr) {
	//vgaScreen.initialize((char *)physicalToVirtual(0xB8000), 80, 25);
//FIXME	vgaTerminal.initialize(vgaScreen.get());
	infoLogger.initialize(infoSink);

	infoLogger->log() << "Starting Thor" << debug::Finish();

	auto info = accessPhysical<EirInfo>(info_paddr);
	infoLogger->log() << "Bootstrap memory at "
			<< (void *)info->bootstrapPhysical
			<< ", length: " << (info->bootstrapLength / 1024) << " KiB" << debug::Finish();

	physicalAllocator.initialize(info->bootstrapPhysical, info->bootstrapLength);
	physicalAllocator->addChunk(info->bootstrapPhysical, info->bootstrapLength);
	physicalAllocator->bootstrap();

	thorRtInitializeProcessor();
	
	PhysicalAddr pml4_ptr;
	asm volatile ( "mov %%cr3, %%rax" : "=a" (pml4_ptr) );
	kernelSpace.initialize(pml4_ptr);
	
	kernelVirtualAlloc.initialize();
	kernelAlloc.initialize(*kernelVirtualAlloc);
	
	kernelStackBase = kernelAlloc->allocate(kernelStackLength);

	irqRelays.initialize();
	thorRtSetupIrqs();
	
	// create a directory and load the memory regions of all modules into it
	ASSERT(info->numModules >= 2);
	auto modules = accessPhysicalN<EirModule>(info->moduleInfo,
			info->numModules);
	
	auto folder = makeShared<RdFolder>(*kernelAlloc);
	for(size_t i = 0; i < info->numModules; i++) {
		auto mod_memory = makeShared<Memory>(*kernelAlloc);
		for(size_t offset = 0; offset < modules[i].length; offset += 0x1000)
			mod_memory->addPage(modules[i].physicalBase + offset);
		
		auto name_ptr = accessPhysicalN<char>(modules[i].namePtr,
				modules[i].nameLength);

		MemoryAccessDescriptor mod_descriptor(traits::move(mod_memory));
		folder->publish(name_ptr, modules[i].nameLength,
				AnyDescriptor(traits::move(mod_descriptor)));
	}
	
	// create a user space thread from the init image
	PageSpace user_space = kernelSpace->clone();
	user_space.switchTo();

	auto universe = makeShared<Universe>(*kernelAlloc);
	auto address_space = makeShared<AddressSpace>(*kernelAlloc, user_space);

	auto entry = (void (*)(uintptr_t))loadInitImage(
			address_space, modules[0].physicalBase);
	thorRtInvalidateSpace();
	
	// allocate and memory memory for the user stack
	size_t stack_size = 0x200000;
	auto stack_memory = makeShared<Memory>(*kernelAlloc);
	stack_memory->resize(stack_size);
	
	VirtualAddr stack_base;
	address_space->map(stack_memory, 0, stack_size, AddressSpace::kMapReadWrite
			| AddressSpace::kMapPreferTop, &stack_base);
	
	auto program_memory = makeShared<Memory>(*kernelAlloc);
	for(size_t offset = 0; offset < modules[1].length; offset += 0x1000)
		program_memory->addPage(modules[1].physicalBase + offset);
	
	auto program_descriptor = MemoryAccessDescriptor(traits::move(program_memory));
	Handle program_handle = universe->attachDescriptor(traits::move(program_descriptor));

	auto thread = makeShared<Thread>(*kernelAlloc);
	thread->setup(entry, program_handle, (void *)(stack_base + stack_size));
	thread->setUniverse(traits::move(universe));
	thread->setAddressSpace(traits::move(address_space));
	thread->setDirectory(traits::move(folder));

	currentThread.initialize(SharedPtr<Thread, KernelAlloc>());

	scheduleQueue.initialize();
	scheduleQueue->addBack(traits::move(thread));
	schedule();
}

extern "C" void thorDivideByZeroError() {
	debug::panicLogger.log() << "Divide by zero" << debug::Finish();
}

extern "C" void thorInvalidOpcode() {
	uintptr_t fault_ip = thorRtUserContext->rip;
	debug::panicLogger.log() << "Invalid opcode"
			<< ", faulting ip: " << (void *)fault_ip
			<< debug::Finish();
}

extern "C" void thorDoubleFault() {
	debug::panicLogger.log() << "Double fault" << debug::Finish();
}

extern "C" void thorGeneralProtectionFault() {
	debug::panicLogger.log() << "General protection fault" << debug::Finish();
}

extern "C" void thorKernelPageFault(uintptr_t address,
		uintptr_t fault_ip, Word error) {
	ASSERT((error & 4) == 0);
	ASSERT((error & 8) == 0);
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

extern "C" void thorUserPageFault(uintptr_t address, Word error) {
	auto stack_ptr = (uint64_t *)thorRtUserContext->rsp;
	auto trace = infoLogger->log() << "Stack trace:\n";
	for(int i = 0; i < 5; i++)
		trace << "    -" << (i * 8) << "(%rsp) " << (void *)stack_ptr[-i] << "\n";
	trace << debug::Finish();

	ASSERT((error & 4) != 0);
	ASSERT((error & 8) == 0);
	auto msg = debug::panicLogger.log();
	msg << "User page fault"
			<< " at " << (void *)address
			<< ", faulting ip: " << (void *)thorRtUserContext->rip << "\n";
	msg << "Errors:";
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
	thorRtAcknowledgeIrq(irq);

	(*irqRelays)[irq].fire();

	if(irq == 0) {
		schedule();
	}else{
		thorRtFullReturn();
	}
	
	ASSERT(!"No return at end of thorIrq()");
}

extern "C" void thorSyscall(Word index, Word arg0, Word arg1,
		Word arg2, Word arg3, Word arg4, Word arg5,
		Word arg6, Word arg7, Word arg8) {
	switch(index) {
		case kHelCallLog: {
			HelError error = helLog((const char *)arg0, (size_t)arg1);

			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallPanic: {
			helLog((const char *)arg0, (size_t)arg1);
			
			while(true) { }
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
		case kHelCallCreateSpace: {
			HelHandle handle;
			HelError error = helCreateSpace(&handle);
			
			thorRtReturnSyscall2((Word)error, (Word)handle);
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
			HelError error = helCreateThread((void (*) (uintptr_t))arg0,
					(uintptr_t)arg1, (void *)arg2, &handle);

			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallExitThisThread: {
			helExitThisThread();
			
			schedule();
		}

		case kHelCallCreateEventHub: {
			HelHandle handle;
			HelError error = helCreateEventHub(&handle);

			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallWaitForEvents: {
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
		case kHelCallSubmitRecvString: {
			HelError error = helSubmitRecvString((HelHandle)arg0,
					(HelHandle)arg1, (uint8_t *)arg2, (size_t)arg3,
					(int64_t)arg4, (int64_t)arg5,
					(int64_t)arg6, (uintptr_t)arg7, (uintptr_t)arg8);

			thorRtReturnSyscall1((Word)error);
		}
		
		case kHelCallCreateServer: {
			HelHandle server_handle;
			HelHandle client_handle;
			HelError error = helCreateServer(&server_handle, &client_handle);
			
			thorRtReturnSyscall3((Word)error, (Word)server_handle, (Word)client_handle);
		}
		case kHelCallSubmitAccept: {
			HelError error = helSubmitAccept((HelHandle)arg0, (HelHandle)arg1,
					(int64_t)arg2, (uintptr_t)arg3, (uintptr_t)arg4);

			thorRtReturnSyscall1((Word)error);
		}
		case kHelCallSubmitConnect: {
			HelError error = helSubmitConnect((HelHandle)arg0, (HelHandle)arg1,
					(int64_t)arg2, (uintptr_t)arg3, (uintptr_t)arg4);

			thorRtReturnSyscall1((Word)error);
		}

		case kHelCallCreateRd: {
			HelHandle handle;
			HelError error = helCreateRd(&handle);

			thorRtReturnSyscall2((Word)error, (Word)handle);
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
			HelError error = helSubmitWaitForIrq((HelHandle)arg0,
					(HelHandle)arg1, (int64_t)arg2,
					(uintptr_t)arg3, (uintptr_t)arg4);

			thorRtReturnSyscall1((Word)error);
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
		default:
			ASSERT(!"Illegal syscall");
	}

	ASSERT(!"No return at end of thorSyscall()");
}

