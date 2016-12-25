
#include <string.h>
#include <sys/auxv.h>

#include "common.hpp"
#include "exec.hpp"
#include "process.hpp"

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

	return context;
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
			0, 0x1000, kHelMapReadWrite, &window));
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
			0, 0x1000, kHelMapReadWrite, &window));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->_fileTableWindow = reinterpret_cast<HelHandle *>(window);

	for(auto entry : original->_fileTable) {
		//std::cout << "Clone FD " << entry.first << std::endl;
		context->attachFile(entry.first, entry.second);
	}

	unsigned long mbus_upstream;
	if(peekauxval(AT_MBUS_SERVER, &mbus_upstream))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	HEL_CHECK(helTransferDescriptor(mbus_upstream,
			context->_universe.getHandle(), &context->_clientMbusLane));

	return context;
}

int FileContext::attachFile(std::shared_ptr<File> file) {	
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(getPassthroughLane(file).getHandle(),
			_universe.getHandle(), &handle));

	for(int fd = 0; ; fd++) {
		if(_fileTable.find(fd) != _fileTable.end())
			continue;
		_fileTable.insert({ fd, std::move(file) });
		_fileTableWindow[fd] = handle;
		return fd;
	}
}

void FileContext::attachFile(int fd, std::shared_ptr<File> file) {	
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(getPassthroughLane(file).getHandle(),
			_universe.getHandle(), &handle));

	auto it = _fileTable.find(fd);
	if(it != _fileTable.end()) {
		it->second = std::move(file);
	}else{
		_fileTable.insert({ fd, std::move(file) });
	}
	_fileTableWindow[fd] = handle;
}

std::shared_ptr<File> FileContext::getFile(int fd) {
	return _fileTable.at(fd);
}

void FileContext::closeFile(int fd) {
	//std::cout << "Close " << fd << std::endl;
	auto it = _fileTable.find(fd);
	if(it != _fileTable.end())
		_fileTable.erase(it);
}

// ----------------------------------------------------------------------------
// Process.
// ----------------------------------------------------------------------------

COFIBER_ROUTINE(cofiber::future<std::shared_ptr<Process>>, Process::init(std::string path),
		([=] {
	auto process = std::make_shared<Process>();
	process->_vmContext = VmContext::create();
	process->_fileContext = FileContext::create();

	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&process->_clientFileTable));

	auto thread = COFIBER_AWAIT execute(path, process->_vmContext,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	serve(process, std::move(thread));

	COFIBER_RETURN(process);
}))

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> original) {
	auto process = std::make_shared<Process>();
	process->_vmContext = VmContext::clone(original->_vmContext);
	process->_fileContext = FileContext::clone(original->_fileContext);

	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&process->_clientFileTable));

	return process;
}

COFIBER_ROUTINE(cofiber::future<void>, Process::exec(std::shared_ptr<Process> process,
		std::string path), ([=] {
	auto exec_vm_context = VmContext::create();

	// Perform the exec() in a new VM context so that we
	// can catch errors before trashing the calling process.
	auto thread = COFIBER_AWAIT execute(path, exec_vm_context,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	serve(process, std::move(thread));

	// "Commit" the exec() operation.
	process->_vmContext = std::move(exec_vm_context);
	
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&process->_clientFileTable));

	COFIBER_RETURN();
}))

