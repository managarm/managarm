
#include <string.h>
#include <sys/auxv.h>

#include <cofiber.hpp>
#include "common.hpp"
#include "exec.hpp"
#include "process.hpp"

static bool logFileAttach = false;

cofiber::no_future serve(std::shared_ptr<Process> self, helix::UniqueDescriptor p);

// ----------------------------------------------------------------------------
// VmContext.
// ----------------------------------------------------------------------------

std::shared_ptr<VmContext> VmContext::create() {
	auto context = std::make_shared<VmContext>();

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);
	
	return context;
}

std::shared_ptr<VmContext> VmContext::clone(std::shared_ptr<VmContext> original) {
	auto context = std::make_shared<VmContext>();

	HelHandle space;
	HEL_CHECK(helForkSpace(original->_space.getHandle(), &space));
	context->_space = helix::UniqueDescriptor(space);
	context->_areaTree = original->_areaTree; // Copy construction is sufficient here.

	return context;
}

COFIBER_ROUTINE(async::result<void *>, VmContext::mapFile(std::shared_ptr<File> file,
		intptr_t offset, size_t size, uint32_t native_flags), ([=] {
	auto memory = COFIBER_AWAIT file->accessMemory(offset);

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	size_t aligned_size = (size + 0xFFF) & ~size_t(0xFFF);
	void *pointer;
	HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
			nullptr, 0 /*offset*/, aligned_size, native_flags, &pointer));
//	std::cout << "posix: VM_MAP returns " << pointer << std::endl;

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + aligned_size);
	if(succ != _areaTree.begin()) {
		auto pred = std::prev(succ);
		assert(pred->first + pred->second.areaSize <= address);
	}

	// Construct the new area.
	Area area;
	area.areaSize = aligned_size;
	area.nativeFlags = native_flags;
	area.file = std::move(file);
	area.offset = offset;
	_areaTree.insert({address, std::move(area)});

	COFIBER_RETURN(pointer);
}))

COFIBER_ROUTINE(async::result<void *>, VmContext::remapFile(void *old_pointer,
		size_t old_size, size_t new_size), ([=] {
//	std::cout << "posix: Remapping " << old_pointer << std::endl;
	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(old_pointer));
	assert(it != _areaTree.end());
	size_t aligned_old_size = (old_size + 0xFFF) & ~size_t(0xFFF);
	assert(it->second.areaSize == aligned_old_size);
	
	auto memory = COFIBER_AWAIT it->second.file->accessMemory(it->second.offset);

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	size_t aligned_new_size = (new_size + 0xFFF) & ~size_t(0xFFF);
	void *pointer;
	HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
			nullptr, 0 /*offset*/, aligned_new_size, it->second.nativeFlags, &pointer));
//	std::cout << "posix: VM_REMAP returns " << pointer << std::endl;

	// TODO: Unmap the old area.
	std::cout << "\e[35mposix: remapFile does not correctly unmap areas\e[39m" << std::endl;
	//HEL_CHECK(helUnmapMemory(_space.getHandle(), old_pointer, aligned_old_size));
	
	// Construct the new area from the old one.
	Area area;
	area.areaSize = aligned_new_size;
	area.nativeFlags = it->second.nativeFlags;
	area.file = std::move(it->second.file);
	area.offset = it->second.offset;
	_areaTree.erase(it);

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + aligned_new_size);
	if(succ != _areaTree.begin()) {
		auto pred = std::prev(succ);
		assert(pred->first + pred->second.areaSize <= address);
	}

	_areaTree.insert({address, std::move(area)});

	COFIBER_RETURN(pointer);
}))

// ----------------------------------------------------------------------------
// FsContext.
// ----------------------------------------------------------------------------

std::shared_ptr<FsContext> FsContext::create() {
	auto context = std::make_shared<FsContext>();

	context->_root = rootPath();

	return context;
}

std::shared_ptr<FsContext> FsContext::clone(std::shared_ptr<FsContext> original) {
	auto context = std::make_shared<FsContext>();

	context->_root = original->_root;

	return context;
}

ViewPath FsContext::getRoot() {
	return _root;
}

void FsContext::changeRoot(ViewPath root) {
	_root = std::move(root);
}

// ----------------------------------------------------------------------------
// FileContext.
// ----------------------------------------------------------------------------

std::shared_ptr<FileContext> FileContext::create() {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->_fileTableWindow = reinterpret_cast<HelHandle *>(window);

	unsigned long mbus_upstream;
	if(peekauxval(AT_MBUS_SERVER, &mbus_upstream))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	HEL_CHECK(helTransferDescriptor(mbus_upstream,
			context->_universe.getHandle(), &context->_clientMbusLane));

	return context;
}

std::shared_ptr<FileContext> FileContext::clone(std::shared_ptr<FileContext> original) {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->_fileTableWindow = reinterpret_cast<HelHandle *>(window);

	for(auto entry : original->_fileTable) {
		//std::cout << "Clone FD " << entry.first << std::endl;
		context->attachFile(entry.first, entry.second.file, entry.second.closeOnExec);
	}

	unsigned long mbus_upstream;
	if(peekauxval(AT_MBUS_SERVER, &mbus_upstream))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	HEL_CHECK(helTransferDescriptor(mbus_upstream,
			context->_universe.getHandle(), &context->_clientMbusLane));

	return context;
}

int FileContext::attachFile(std::shared_ptr<File> file, bool close_on_exec) {	
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(file->getPassthroughLane().getHandle(),
			_universe.getHandle(), &handle));

	for(int fd = 0; ; fd++) {
		if(_fileTable.find(fd) != _fileTable.end())
			continue;

		if(logFileAttach)
			std::cout << "posix: Attaching FD " << fd << std::endl;

		_fileTable.insert({fd, {std::move(file), close_on_exec}});
		_fileTableWindow[fd] = handle;
		return fd;
	}
}

void FileContext::attachFile(int fd, std::shared_ptr<File> file, bool close_on_exec) {	
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(file->getPassthroughLane().getHandle(),
			_universe.getHandle(), &handle));
	
	if(logFileAttach)
		std::cout << "posix: Attaching fixed FD " << fd << std::endl;

	auto it = _fileTable.find(fd);
	if(it != _fileTable.end()) {
		it->second = {std::move(file), close_on_exec};
	}else{
		_fileTable.insert({fd, {std::move(file), close_on_exec}});
	}
	_fileTableWindow[fd] = handle;
}

FileDescriptor FileContext::getDescriptor(int fd) {
	auto file = _fileTable.find(fd);
	assert(file != _fileTable.end());
	return file->second;
}

std::shared_ptr<File> FileContext::getFile(int fd) {
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return std::shared_ptr<File>{};
	return file->second.file;
}

void FileContext::closeFile(int fd) {
	if(logFileAttach)
		std::cout << "posix: Closing FD " << fd << std::endl;
	auto it = _fileTable.find(fd);
	assert(it != _fileTable.end());

	_fileTableWindow[fd] = 0;
	_fileTable.erase(it);
}

void FileContext::closeOnExec() {
	auto it = _fileTable.begin();
	while(it != _fileTable.end()) {
		if(it->second.closeOnExec) {
			_fileTableWindow[it->first] = 0;
			it = _fileTable.erase(it);
		}else{
			it++;
		}
	}
}

// ----------------------------------------------------------------------------
// Process.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(async::result<std::shared_ptr<Process>>, Process::init(std::string path),
		([=] {
	auto process = std::make_shared<Process>();
	process->_vmContext = VmContext::create();
	process->_fsContext = FsContext::create();
	process->_fileContext = FileContext::create();

	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientFileTable));

	// TODO: Do not pass an empty argument vector?
	auto thread = COFIBER_AWAIT execute(process->_fsContext->getRoot(), path,
			std::vector<std::string>{}, std::vector<std::string>{},
			process->_vmContext,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	serve(process, std::move(thread));

	COFIBER_RETURN(process);
}))

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> original) {
	auto process = std::make_shared<Process>();
	process->_vmContext = VmContext::clone(original->_vmContext);
	process->_fsContext = FsContext::clone(original->_fsContext);
	process->_fileContext = FileContext::clone(original->_fileContext);

	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientFileTable));

	return process;
}

COFIBER_ROUTINE(async::result<void>, Process::exec(std::shared_ptr<Process> process,
		std::string path, std::vector<std::string> args, std::vector<std::string> env), ([=] {
	auto exec_vm_context = VmContext::create();

	void *exec_client_table;
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&exec_client_table));
	
	// TODO: We should only do this if the execute succeeds.
	process->_fileContext->closeOnExec();

	// Perform the exec() in a new VM context so that we
	// can catch errors before trashing the calling process.
	auto thread = COFIBER_AWAIT execute(process->_fsContext->getRoot(),
			path, std::move(args), std::move(env), exec_vm_context,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	serve(process, std::move(thread));

	// "Commit" the exec() operation.
	process->_vmContext = std::move(exec_vm_context);
	process->_clientFileTable = exec_client_table;

	// TODO: execute() should return a stopped thread that we can start here.

	COFIBER_RETURN();
}))

