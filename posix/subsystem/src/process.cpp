
#include <string.h>

#include "common.hpp"
#include "process.hpp"

std::shared_ptr<Process> Process::createInit() {
	auto data = std::make_shared<Process>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	data->_universe = helix::UniqueDescriptor(universe);

	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));
	data->_space = helix::UniqueDescriptor(space);

	HelHandle file_table_memory;
	void *file_table_window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &file_table_memory));
	HEL_CHECK(helMapMemory(file_table_memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite, &file_table_window));
	HEL_CHECK(helMapMemory(file_table_memory, data->_space.getHandle(), nullptr,
			0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&data->_clientFileTable));
	HEL_CHECK(helCloseDescriptor(file_table_memory));
	data->_fileTableWindow = reinterpret_cast<HelHandle *>(file_table_window);

	return data;
}

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> parent) {
	auto data = std::make_shared<Process>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	data->_universe = helix::UniqueDescriptor(universe);

	HelHandle space;
	HEL_CHECK(helForkSpace(parent->_space.getHandle(), &space));
	data->_space = helix::UniqueDescriptor(space);

	HelHandle file_table_memory;
	void *file_table_window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &file_table_memory));
	HEL_CHECK(helMapMemory(file_table_memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite, &file_table_window));
	HEL_CHECK(helMapMemory(file_table_memory, data->_space.getHandle(), nullptr,
			0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
			&data->_clientFileTable));
	HEL_CHECK(helCloseDescriptor(file_table_memory));
	data->_fileTableWindow = reinterpret_cast<HelHandle *>(file_table_window);

	for(auto entry : parent->_fileTable) {
		auto lane = getPassthroughLane(entry.second);

		HelHandle handle;
		HEL_CHECK(helTransferDescriptor(lane.getHandle(), universe, &handle));
		data->_fileTableWindow[entry.first] = handle;
	}

	return data;
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

