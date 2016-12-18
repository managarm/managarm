
#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <memory>
#include <unordered_map>

#include "vfs.hpp"

typedef int ProcessId;

struct Process {
	static std::shared_ptr<Process> createInit();

	static std::shared_ptr<Process> fork(std::shared_ptr<Process> parent);

	helix::BorrowedDescriptor getUniverse() {
		return _universe;
	}

	helix::BorrowedDescriptor getVmSpace() {
		return _space;
	}

	void *clientFileTable() {
		return _clientFileTable;
	}
	
	int attachFile(std::shared_ptr<File> file);

	void attachFile(int fd, std::shared_ptr<File> file);

	std::shared_ptr<File> getFile(int fd);

	void closeFile(int fd);

private:
	helix::UniqueDescriptor _universe;
	helix::UniqueDescriptor _space;

	// TODO: replace this by a tree that remembers gaps between keys.
	std::unordered_map<int, std::shared_ptr<File>> _fileTable;

	void *_clientFileTable;

	HelHandle *_fileTableWindow;
};

#endif // POSIX_SUBSYSTEM_PROCESS_HPP

