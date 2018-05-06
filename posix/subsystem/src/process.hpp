#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <map>
#include <memory>
#include <unordered_map>

#include <async/result.hpp>

#include "vfs.hpp"

typedef int ProcessId;

// TODO: This struct should store the process' VMAs once we implement them.
// TODO: We need a clarification here: Does mmap() keep file descriptions open (e.g. for flock())?
struct VmContext {
	static std::shared_ptr<VmContext> create();
	static std::shared_ptr<VmContext> clone(std::shared_ptr<VmContext> original);

	helix::BorrowedDescriptor getSpace() {
		return _space;
	}

	// TODO: Pass abstract instead of hel flags to this function?
	async::result<void *> mapFile(smarter::shared_ptr<File, FileHandle> file,
			intptr_t offset, size_t size, uint32_t native_flags);

	async::result<void *> remapFile(void *old_pointer, size_t old_size, size_t new_size);

	void unmapFile(void *pointer, size_t size);

private:
	struct Area {
		size_t areaSize;
		uint32_t nativeFlags;
		smarter::shared_ptr<File, FileHandle> file;
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
	smarter::shared_ptr<File, FileHandle> file;
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
	
	int attachFile(smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false);

	void attachFile(int fd, smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false);

	FileDescriptor getDescriptor(int fd);

	smarter::shared_ptr<File, FileHandle> getFile(int fd);

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

struct SignalContext {
private:
	struct SignalSlot {
		uintptr_t handler;
		uintptr_t restorer;
	};

public:
	static std::shared_ptr<SignalContext> create();
	static std::shared_ptr<SignalContext> clone(std::shared_ptr<SignalContext> original);

	void setSignalHandler(int number, uintptr_t handler, uintptr_t restorer);
	
	void restoreSignal(helix::BorrowedDescriptor thread);
	
	void raiseSynchronousSignal(int number, helix::BorrowedDescriptor thread);

private:
	SignalSlot _slots[64];
};

struct Process {
	static async::result<std::shared_ptr<Process>> init(std::string path);

	static std::shared_ptr<Process> fork(std::shared_ptr<Process> parent);

	static async::result<void> exec(std::shared_ptr<Process> process,
			std::string path, std::vector<std::string> args, std::vector<std::string> env);

public:
	Process();

	int pid() {
		assert(_pid); // Do not return uninitialized information.
	 	return _pid;
	}

	std::string path() {
		return _path;
	}

	// TODO: The following three function do not need to return shared_ptrs.
	std::shared_ptr<VmContext> vmContext() {
		return _vmContext;
	}
	
	std::shared_ptr<FsContext> fsContext() {
		return _fsContext;
	}
	
	std::shared_ptr<FileContext> fileContext() {
		return _fileContext;
	}

	SignalContext *signalContext() {
		return _signalContext.get();
	}
	
	void *clientClkTrackerPage() {
		return _clientClkTrackerPage;
	}

	void *clientFileTable() {
		return _clientFileTable;
	}

private:
	int _pid;
	std::string _path;
	std::shared_ptr<VmContext> _vmContext;
	std::shared_ptr<FsContext> _fsContext;
	std::shared_ptr<FileContext> _fileContext;
	std::shared_ptr<SignalContext> _signalContext;

	void *_clientClkTrackerPage;
	void *_clientFileTable;
};

std::shared_ptr<Process> findProcessWithCredentials(const char *credentials);

#endif // POSIX_SUBSYSTEM_PROCESS_HPP
