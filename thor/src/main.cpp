
#include "../../frigg/include/types.hpp"
#include "util/general.hpp"
#include "runtime.hpp"
#include "debug.hpp"
#include "../../frigg/include/elf.hpp"
#include "util/vector.hpp"
#include "util/smart-ptr.hpp"
#include "memory/physical-alloc.hpp"
#include "memory/paging.hpp"
#include "memory/kernel-alloc.hpp"
#include "core.hpp"
#include "schedule.hpp"
#include "../../hel/include/hel.h"

using namespace thor;

LazyInitializer<debug::VgaScreen> vgaScreen;
LazyInitializer<debug::Terminal> vgaTerminal;

LazyInitializer<memory::PhysicalChunkAllocator> physicalAllocator;

void *loadInitImage(UnsafePtr<AddressSpace, KernelAlloc> space, uintptr_t image_page) {
	char *image = (char *)memory::physicalToVirtual(image_page);

	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image;
	ASSERT(ehdr->e_ident[0] == '\x7F'
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	ASSERT(ehdr->e_type == ET_EXEC);
	
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

		Mapping *mapping = space->allocateAt(bottom, page_size * num_pages);

		auto memory = makeShared<Memory>(*kernelAlloc);
		memory->resize(num_pages * page_size);

		for(uintptr_t page = 0; page < num_pages; page++) {
			PhysicalAddr physical = memory->getPage(page);
			for(int p = 0; p < page_size; p++)
				*((char *)memory::physicalToVirtual(physical) + p) = 0;
		}

		for(size_t p = 0; p < phdr->p_filesz; p++) {
			uintptr_t page = (phdr->p_vaddr + p) / page_size - bottom_page;
			uintptr_t virt_offset = (phdr->p_vaddr + p) % page_size;
			
			PhysicalAddr physical = memory->getPage(page);
			char *ptr = (char *)memory::physicalToVirtual(physical);
			*(ptr + virt_offset) = *(image + phdr->p_offset + p);
		}

		for(uintptr_t page = 0; page < num_pages; page++) {
			PhysicalAddr physical = memory->getPage(page);
			
			space->mapSingle4k((void *)((bottom_page + page) * page_size),
					physical);
		}

		mapping->type = Mapping::kTypeMemory;
		mapping->memoryRegion = util::move(memory);
	}
	
	return (void *)ehdr->e_entry;
}

extern "C" void thorMain(uint64_t init_image) {
	vgaScreen.initialize((char *)memory::physicalToVirtual(0xB8000), 80, 25);
	
	vgaTerminal.initialize(vgaScreen.get());
	debug::infoSink = vgaTerminal.get();
	debug::infoLogger.initialize(vgaTerminal.get());
	debug::panicLogger.initialize(vgaTerminal.get());

	debug::infoLogger->log() << "Starting Thor" << debug::Finish();

	physicalAllocator.initialize(0x5000000, 0x2000000);
	physicalAllocator->addChunk(0x5000000, 0x2000000);
	physicalAllocator->bootstrap();
	memory::tableAllocator = physicalAllocator.get();

	thorRtInitializeProcessor();
	
	memory::kernelSpace.initialize(0x4001000);
	kernelAlloc.initialize();
	
	irqRelays.initialize();
	thorRtSetupIrqs();

	memory::PageSpace user_space = memory::kernelSpace->clone();
	user_space.switchTo();

	auto universe = makeShared<Universe>(*kernelAlloc);
	auto address_space = makeShared<AddressSpace>(*kernelAlloc, user_space);
	
	auto entry = (void (*)(uintptr_t))loadInitImage(
			address_space, init_image);
	thorRtInvalidateSpace();
	
	// allocate and memory memory for the user stack
	size_t stack_size = 0x200000;
	auto stack_memory = makeShared<Memory>(*kernelAlloc);
	stack_memory->resize(stack_size);

	Mapping *stack_mapping = address_space->allocate(stack_size);
	for(size_t i = 0; i < stack_size / 0x1000; i++)
		address_space->mapSingle4k((void *)(stack_mapping->baseAddress
				+ i * 0x1000), stack_memory->getPage(i));

	auto thread = makeShared<Thread>(*kernelAlloc);
	thread->setup(entry, 0, (void *)(stack_mapping->baseAddress + stack_size));
	thread->setUniverse(util::move(universe));
	thread->setAddressSpace(util::move(address_space));
	
	currentThread.initialize(SharedPtr<Thread, KernelAlloc>());
	scheduleQueue.initialize();

	scheduleQueue->addBack(util::move(thread));
	schedule();
}

extern "C" void thorDoubleFault() {
	debug::panicLogger->log() << "Double fault" << debug::Finish();
}

extern "C" void thorKernelPageFault(uintptr_t address,
		uintptr_t fault_ip, Word error) {
	debug::panicLogger->log() << "Kernel page fault"
			<< " at " << (void *)address
			<< ", faulting ip: " << (void *)fault_ip
			<< debug::Finish();
}


extern "C" void thorUserPageFault(uintptr_t address, Word error) {
	debug::panicLogger->log() << "User page fault"
			<< " at " << (void *)address
			<< ", faulting ip: " << (void *)thorRtUserContext->rip
			<< debug::Finish();
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
			HelError error = helLog((const char *)arg0, (size_t)arg1);
			
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
		case kHelCallMapMemory: {
			void *actual_pointer;
			HelError error = helMapMemory((HelHandle)arg0,
					(void *)arg1, (size_t)arg2, &actual_pointer);

			thorRtReturnSyscall2((Word)error, (Word)actual_pointer);
		}

		case kHelCallCreateThread: {
			HelHandle handle;
			HelError error = helCreateThread((void (*) (uintptr_t))arg0,
					(uintptr_t)arg1, (void *)arg2, &handle);

			thorRtReturnSyscall2((Word)error, (Word)handle);
		}
		case kHelCallExitThisThread: {
			HelError error = helExitThisThread();
			
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

