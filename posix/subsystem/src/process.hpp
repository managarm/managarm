
#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <memory>
#include <unordered_map>

#include "vfs.hpp"

typedef int ProcessId;

struct SharedProcess {
	// TODO: fix this by replacing SharedProcess with a vtable.
	friend SharedProcess fork(SharedProcess parent);

	static SharedProcess createInit() {
		auto data = std::make_shared<Data>();

		HelHandle universe;
		HEL_CHECK(helCreateUniverse(&universe));
		data->universe = helix::UniqueDescriptor(universe);

		HelHandle space;
		HEL_CHECK(helCreateSpace(&space));
		data->space = helix::UniqueDescriptor(space);

		HelHandle file_table_memory;
		void *file_table_window;
		HEL_CHECK(helAllocateMemory(0x1000, 0, &file_table_memory));
		HEL_CHECK(helMapMemory(file_table_memory, kHelNullHandle, nullptr,
				0, 0x1000, kHelMapReadWrite, &file_table_window));
		HEL_CHECK(helMapMemory(file_table_memory, data->space.getHandle(), nullptr,
				0, 0x1000, kHelMapReadOnly | kHelMapDropAtFork,
				&data->clientFileTable));
		HEL_CHECK(helCloseDescriptor(file_table_memory));
		data->fileTableWindow = reinterpret_cast<HelHandle *>(file_table_window);

		return SharedProcess(std::move(data));
	}

	int attachFile(std::shared_ptr<File> file) const {
		for(int fd = 0; ; fd++) {
			if(_data->fileTable.find(fd) != _data->fileTable.end())
				continue;
			_data->fileTable.insert({ fd, std::move(file) });
			return fd;
		}
	}
	
	void attachFile(int fd, std::shared_ptr<File> file) const {
		auto it = _data->fileTable.find(fd);
		if(it != _data->fileTable.end()) {
			it->second = std::move(file);
		}else{
			_data->fileTable.insert({ fd, std::move(file) });
		}
	}

	std::shared_ptr<File> getFile(int fd) const {
		return _data->fileTable.at(fd);
	}

	void closeFile(int fd) const {
		auto it = _data->fileTable.find(fd);
		if(it != _data->fileTable.end())
			_data->fileTable.erase(it);
	}

	helix::BorrowedDescriptor getUniverse() const {
		return _data->universe;
	}

	helix::BorrowedDescriptor getVmSpace() const {
		return _data->space;
	}

	void *clientFileTable() const {
		return _data->clientFileTable;
	}

private:
	struct Data {
		helix::UniqueDescriptor universe;
		helix::UniqueDescriptor space;

		// TODO: replace this by a tree that remembers gaps between keys.
		std::unordered_map<int, std::shared_ptr<File>> fileTable;

		void *clientFileTable;

		HelHandle *fileTableWindow;
	};

	explicit SharedProcess(std::shared_ptr<Data> data)
	: _data(std::move(data)) { }

	std::shared_ptr<Data> _data;
};

SharedProcess fork(SharedProcess parent);

#endif // POSIX_SUBSYSTEM_PROCESS_HPP

