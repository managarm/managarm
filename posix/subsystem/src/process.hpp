
#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <memory>
#include <unordered_map>

#include "vfs.hpp"

typedef int ProcessId;

// TODO: This struct should store the process' VMAs once we implement them.
struct VmContext {
	static std::shared_ptr<VmContext> create();
	static std::shared_ptr<VmContext> clone(std::shared_ptr<VmContext> original);

	helix::BorrowedDescriptor getSpace() {
		return _space;
	}

private:
	helix::UniqueDescriptor _space;
};

struct Process {
	static std::shared_ptr<Process> createInit();

	static std::shared_ptr<Process> fork(std::shared_ptr<Process> parent);

	helix::BorrowedDescriptor getUniverse() {
		return _universe;
	}

	std::shared_ptr<VmContext> vmContext() {
		return _vmContext;
	}

	int attachFile(std::shared_ptr<File> file);

	void attachFile(int fd, std::shared_ptr<File> file);

	std::shared_ptr<File> getFile(int fd);

	void closeFile(int fd);

	void *clientFileTable() {
		return _clientFileTable;
	}

private:
	std::shared_ptr<VmContext> _vmContext;

	helix::UniqueDescriptor _universe;

	// TODO: replace this by a tree that remembers gaps between keys.
	std::unordered_map<int, std::shared_ptr<File>> _fileTable;

	void *_clientFileTable;

	HelHandle *_fileTableWindow;
};

#endif // POSIX_SUBSYSTEM_PROCESS_HPP

