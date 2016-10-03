
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <iostream>

#include <cofiber.hpp>
#include <cofiber/stash.hpp>
#include <cofiber/future.hpp>
#include <frigg/elf.hpp>

#include "common.hpp"
#include "exec.hpp"
#include <fs.pb.h>

constexpr size_t kPageSize = 0x1000;

COFIBER_ROUTINE(cofiber::no_future, fsOpen(std::string path,
		cofiber::stash<helix::UniquePipe> &promise),
		([path, &promise] {
	using M = helix::AwaitMechanism;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::OPEN);
	req.set_path(path);

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(helix::Dispatcher::global(), fsPipe, serialized.data(),
			serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(helix::Dispatcher::global(), fsPipe, buffer, 128,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	
	helix::RecvDescriptor<M> recv_file(helix::Dispatcher::global(), fsPipe,
			0, 1, kHelResponse);
	COFIBER_AWAIT recv_file.future();
	HEL_CHECK(recv_file.error());
	promise.set_value(helix::UniquePipe(recv_file.descriptor()));
}))

COFIBER_ROUTINE(cofiber::future<void>, fsSeek(helix::BorrowedPipe file,
		uintptr_t offset), ([=] {
	using M = helix::AwaitMechanism;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::SEEK_ABS);
	req.set_rel_offset(offset);

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(helix::Dispatcher::global(), file, serialized.data(),
			serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(helix::Dispatcher::global(), file, buffer, 128,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	COFIBER_RETURN();
}))

COFIBER_ROUTINE(cofiber::future<void>, fsRead(helix::BorrowedPipe file,
		void *data, size_t length), ([=] {
	using M = helix::AwaitMechanism;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::READ);
	req.set_size(length);

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(helix::Dispatcher::global(), file, serialized.data(),
			serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(helix::Dispatcher::global(), file, buffer, 128,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	helix::RecvString<M> recv_data(helix::Dispatcher::global(), file, data, length,
			0, 1, kHelResponse);
	COFIBER_AWAIT recv_data.future();
	HEL_CHECK(recv_data.error());
	assert(recv_data.actualLength() == length);

	COFIBER_RETURN();
}))

COFIBER_ROUTINE(cofiber::no_future, fsMap(helix::BorrowedPipe file,
			cofiber::stash<helix::UniqueDescriptor> &promise),
		([file, &promise] {
	using M = helix::AwaitMechanism;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::MMAP);

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(helix::Dispatcher::global(), file, serialized.data(),
			serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(helix::Dispatcher::global(), file, buffer, 128,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);
	
	helix::RecvDescriptor<M> recv_memory(helix::Dispatcher::global(), file,
			0, 1, kHelResponse);
	COFIBER_AWAIT recv_memory.future();
	HEL_CHECK(recv_memory.error());
	promise.set_value(recv_memory.descriptor());
}))

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
};

COFIBER_ROUTINE(cofiber::no_future, load(helix::BorrowedDescriptor space,
		std::string path, uintptr_t base, cofiber::stash<ImageInfo> &promise),
		([space, path, base, &promise] {
	assert(base % kPageSize == 0);
	ImageInfo info;

	// get a handle to the file's memory.
	cofiber::stash<helix::UniquePipe> file;
	fsOpen(path, file);
	COFIBER_AWAIT file;
	
	cofiber::stash<helix::UniqueDescriptor> mapping;
	fsMap(*file, mapping);
	COFIBER_AWAIT mapping;

	// read the elf file header and verify the signature.
	Elf64_Ehdr ehdr;
	COFIBER_AWAIT fsRead(*file, &ehdr, sizeof(Elf64_Ehdr));

	assert(ehdr.e_ident[0] == 0x7F
			&& ehdr.e_ident[1] == 'E'
			&& ehdr.e_ident[2] == 'L'
			&& ehdr.e_ident[3] == 'F');
	assert(ehdr.e_type == ET_EXEC || ehdr.e_type == ET_DYN);

	info.entryIp = (char *)base + ehdr.e_entry;
	info.phdrEntrySize = ehdr.e_phentsize;
	info.phdrCount = ehdr.e_phnum;

	// read the elf program headers and load them into the address space.
	auto phdr_buffer = (char *)malloc(ehdr.e_phnum * ehdr.e_phentsize);
	COFIBER_AWAIT fsSeek(*file, ehdr.e_phoff);
	COFIBER_AWAIT fsRead(*file, phdr_buffer, ehdr.e_phnum * size_t(ehdr.e_phentsize));

	for(int i = 0; i < ehdr.e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)(phdr_buffer + i * ehdr.e_phentsize);
		
		if(phdr->p_type == PT_LOAD) {
			assert(phdr->p_memsz > 0);
			
			size_t misalign = phdr->p_vaddr % kPageSize;
			uintptr_t map_address = base + phdr->p_vaddr - misalign;
			size_t map_length = phdr->p_memsz + misalign;
			if((map_length % kPageSize) != 0)
				map_length += kPageSize - (map_length % kPageSize);
			
			// check if we can share the segment.
			if(!(phdr->p_flags & PF_W)) {
				assert(misalign == 0);
				assert(phdr->p_offset % kPageSize == 0);
			
				// map the segment with correct permissions into the process.
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
					HEL_CHECK(helLoadahead(mapping->getHandle(), phdr->p_offset, map_length));

					void *map_pointer;
					HEL_CHECK(helMapMemory(mapping->getHandle(), space.getHandle(),
							(void *)map_address, phdr->p_offset, map_length,
							kHelMapReadExecute | kHelMapShareAtFork, &map_pointer));
				}else{
					throw std::runtime_error("Illegal combination of segment permissions");
				}
			}else{
				// map the segment with write permission into this address space.
				HelHandle segment_memory;
				HEL_CHECK(helAllocateMemory(map_length, 0, &segment_memory));

				void *window;
				HEL_CHECK(helMapMemory(segment_memory, kHelNullHandle, nullptr,
						0, map_length, kHelMapReadWrite, &window));
			
				// map the segment with correct permissions into the process.
				if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
					void *map_pointer;
					HEL_CHECK(helMapMemory(segment_memory, space.getHandle(), (void *)map_address,
							0, map_length, kHelMapReadWrite | kHelMapCopyOnWriteAtFork, &map_pointer));
				}else{
					throw std::runtime_error("Illegal combination of segment permissions");
				}
				HEL_CHECK(helCloseDescriptor(segment_memory));

				// read the segment contents from the file.
				memset(window, 0, map_length);
				COFIBER_AWAIT fsSeek(*file, phdr->p_offset);
				COFIBER_AWAIT fsRead(*file, (char *)window + misalign, phdr->p_filesz);
				HEL_CHECK(helUnmapMemory(kHelNullHandle, window, map_length));
			}
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_DYNAMIC || phdr->p_type == PT_INTERP
				|| phdr->p_type == PT_TLS
				|| phdr->p_type == PT_GNU_EH_FRAME || phdr->p_type == PT_GNU_STACK) {
			// ignore this PHDR here.
		}else{
			throw std::runtime_error("Unexpected PHDR type");
		}
	}
	
	promise.set_value(info);
}))

template<typename T, size_t N>
void *copyArrayToStack(void *window, size_t &d, const T (&value)[N]) {
	assert(d >= alignof(T) + sizeof(T));
	d -= d % alignof(T);
	d -= sizeof(value) * N;
	void *ptr = (char *)window + d;
	memcpy(ptr, &value, sizeof(value) * N);
	return ptr;
}

// FIXME: remove this helper function
COFIBER_ROUTINE(cofiber::no_future, _execute(std::string path), ([=] {
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));

	cofiber::stash<ImageInfo> exec_info;
	load(helix::BorrowedDescriptor(space), path, 0, exec_info);
	COFIBER_AWAIT exec_info;
	
	cofiber::stash<ImageInfo> interp_info;
	load(helix::BorrowedDescriptor(space), "ld-init.so", 0x40000000, interp_info);
	COFIBER_AWAIT interp_info;
	
	constexpr size_t stack_size = 0x10000;
	
	// allocate memory for the stack and map it into the remote space.
	HelHandle stack_memory;
	HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));

	void *stack_base;
	HEL_CHECK(helMapMemory(stack_memory, space, nullptr,
			0, stack_size, kHelMapReadWrite | kHelMapCopyOnWriteAtFork, &stack_base));
	
	// map the stack into this process and set it up.
	void *window;
	HEL_CHECK(helMapMemory(stack_memory, kHelNullHandle, nullptr,
			0, stack_size, kHelMapReadWrite, &window));
	HEL_CHECK(helCloseDescriptor(stack_memory));

	// the offset at which the stack image starts.
	size_t d = stack_size;

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	
	HelHandle remote_fs;
	HEL_CHECK(helTransferDescriptor(fsPipe.getHandle(), universe, &remote_fs));

	copyArrayToStack(window, d, (uintptr_t[]){
		AT_ENTRY,
		uintptr_t(exec_info->entryIp),
		AT_PHDR,
		uintptr_t(exec_info->phdrPtr),
		AT_PHENT,
		exec_info->phdrEntrySize,
		AT_PHNUM,
		exec_info->phdrCount,
		AT_FS_SERVER,
		uintptr_t(remote_fs),
		AT_NULL,
		0
	});

	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, stack_size));

	// finally create a new thread to run the executable
	//FIXME helx::Directory directory = Process::runServer(process);

	HelHandle thread;
	HEL_CHECK(helCreateThread(universe, space, kHelAbiSystemV,
			(void *)interp_info->entryIp, (char *)stack_base + d, 0, &thread));

/*	auto action = frigg::await<void(HelError)>([=] (auto callback) {
		int64_t async_id;
		HEL_CHECK(helSubmitObserve(thread, eventHub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				&async_id));
	})
	+ frigg::lift([=](HelError error) {
		frigg::infoLogger() << "Observe triggered" << frigg::endLog;
		HEL_CHECK(helResume(thread));
	});

	frigg::run(frigg::move(action), allocator.get());*/
}))

void execute(std::shared_ptr<Process> process, std::string path) {
	_execute(path);	
}

