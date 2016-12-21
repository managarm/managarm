
#include <string.h>

#include "common.hpp"
#include "process.hpp"

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
// Process.
// ----------------------------------------------------------------------------

std::shared_ptr<Process> Process::createInit() {
	auto process = std::make_shared<Process>();
	process->_vmContext = VmContext::create();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	process->_universe = helix::UniqueDescriptor(universe);

	HelHandle file_table_memory;
	void *file_table_window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &file_table_memory));
	HEL_CHECK(helMapMemory(file_table_memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite, &file_table_window));
	HEL_CHECK(helMapMemory(file_table_memory, process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&process->_clientFileTable));
	HEL_CHECK(helCloseDescriptor(file_table_memory));
	process->_fileTableWindow = reinterpret_cast<HelHandle *>(file_table_window);

	return process;
}

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> original) {
	auto process = std::make_shared<Process>();
	process->_vmContext = VmContext::clone(original->_vmContext);

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	process->_universe = helix::UniqueDescriptor(universe);

	HelHandle file_table_memory;
	void *file_table_window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &file_table_memory));
	HEL_CHECK(helMapMemory(file_table_memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite, &file_table_window));
	HEL_CHECK(helMapMemory(file_table_memory, process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&process->_clientFileTable));
	HEL_CHECK(helCloseDescriptor(file_table_memory));
	process->_fileTableWindow = reinterpret_cast<HelHandle *>(file_table_window);

	for(auto entry : original->_fileTable) {
		auto lane = getPassthroughLane(entry.second);

		HelHandle handle;
		HEL_CHECK(helTransferDescriptor(lane.getHandle(), universe, &handle));
		process->_fileTableWindow[entry.first] = handle;
	}

	return process;
}

int Process::attachFile(std::shared_ptr<File> file) {	
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

void Process::attachFile(int fd, std::shared_ptr<File> file) {	
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

std::shared_ptr<File> Process::getFile(int fd) {
	return _fileTable.at(fd);
}

void Process::closeFile(int fd) {
	auto it = _fileTable.find(fd);
	if(it != _fileTable.end())
		_fileTable.erase(it);
}

