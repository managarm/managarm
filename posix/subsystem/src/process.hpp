
#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <map>
#include <memory>
#include <unordered_map>

#include <async/result.hpp>

#include "vfs.hpp"

typedef int ProcessId;

// TODO: This struct should store the process' VMAs once we implement them.
struct VmContext {
	static std::shared_ptr<VmContext> create();
	static std::shared_ptr<VmContext> clone(std::shared_ptr<VmContext> original);

	helix::BorrowedDescriptor getSpace() {
		return _space;
	}

	// TODO: Pass abstract instead of hel flags to this function?
	async::result<void *> mapFile(std::shared_ptr<File> file, intptr_t offset, size_t size,
			uint32_t native_flags);

	async::result<void *> remapFile(void *old_pointer, size_t old_size, size_t new_size);

private:
	struct Area {
		size_t areaSize;
		uint32_t nativeFlags;
		std::shared_ptr<File> file;
		intptr_t offset;
	};

	helix::UniqueDescriptor _space;

	std::map<uintptr_t, Area> _areaTree;
};

struct FsContext {
	static std::shared_ptr<FsContext> create();
	static std::shared_ptr<FsContext> clone(std::shared_ptr<FsContext> original);

	ViewPath getRoot();

	void changeRoot(ViewPath root);

private:
	ViewPath _root;
};

struct FileDescriptor {
	std::shared_ptr<File> file;
	bool closeOnExec;
};

struct FileContext {
public:
	static std::shared_ptr<FileContext> create();
	static std::shared_ptr<FileContext> clone(std::shared_ptr<FileContext> original);

	helix::BorrowedDescriptor getUniverse() {
		return _universe;
	}
	
	helix::BorrowedDescriptor fileTableMemory() {
		return _fileTableMemory;
	}
	
	int attachFile(std::shared_ptr<File> file, bool close_on_exec = false);

	void attachFile(int fd, std::shared_ptr<File> file, bool close_on_exec = false);

	FileDescriptor getDescriptor(int fd);

	std::shared_ptr<File> getFile(int fd);

	void closeFile(int fd);

	void closeOnExec();

	HelHandle clientMbusLane() {
		return _clientMbusLane;
	}

private:
	helix::UniqueDescriptor _universe;

	// TODO: replace this by a tree that remembers gaps between keys.
	std::unordered_map<int, FileDescriptor> _fileTable;

	helix::UniqueDescriptor _fileTableMemory;

	HelHandle *_fileTableWindow;

	HelHandle _clientMbusLane;
};

struct Process {
	static async::result<std::shared_ptr<Process>> init(std::string path);

	static std::shared_ptr<Process> fork(std::shared_ptr<Process> parent);

	static async::result<void> exec(std::shared_ptr<Process> process,
			std::string path, std::vector<std::string> args, std::vector<std::string> env);

	std::shared_ptr<VmContext> vmContext() {
		return _vmContext;
	}
	
	std::shared_ptr<FsContext> fsContext() {
		return _fsContext;
	}
	
	std::shared_ptr<FileContext> fileContext() {
		return _fileContext;
	}

	void *clientFileTable() {
		return _clientFileTable;
	}

private:
	std::shared_ptr<VmContext> _vmContext;
	std::shared_ptr<FsContext> _fsContext;
	std::shared_ptr<FileContext> _fileContext;

	void *_clientFileTable;
};

#endif // POSIX_SUBSYSTEM_PROCESS_HPP

