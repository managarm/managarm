#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <map>
#include <memory>
#include <unordered_map>

#include <async/result.hpp>
#include <async/doorbell.hpp>
#include <boost/intrusive/list.hpp>

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
	ViewPath getWorkingDirectory();

	void changeRoot(ViewPath root);
	void changeWorkingDirectory(ViewPath root);

private:
	ViewPath _root;
	ViewPath _workDir;
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

struct UserSignal {
	int pid;
	int uid;
};

using SignalInfo = std::variant<
	UserSignal
>;

using SignalFlags = uint32_t;

inline constexpr SignalFlags signalInfo = (1 << 0);
inline constexpr SignalFlags signalOnce = (1 << 1);
inline constexpr SignalFlags signalReentrant = (1 << 2);

enum class SignalDisposition {
	none,
	ignore,
	handle
};

struct SignalHandler {
	SignalDisposition disposition;
	SignalFlags flags;
	uint64_t mask;
	uintptr_t handlerIp;
	uintptr_t restorerIp;
};

struct SignalItem {
	int signalNumber;
	SignalInfo info;
	boost::intrusive::list_member_hook<> queueHook;
};

struct SignalContext {
private:
	struct SignalSlot {
		boost::intrusive::list<
			SignalItem,
			boost::intrusive::member_hook<
				SignalItem,
				boost::intrusive::list_member_hook<>,
				&SignalItem::queueHook>
		> asyncQueue;
	};

public:
	static std::shared_ptr<SignalContext> create();
	static std::shared_ptr<SignalContext> clone(std::shared_ptr<SignalContext> original);

	SignalContext();

	void resetHandlers();

	SignalHandler changeHandler(int sn, SignalHandler handler);

	void issueSignal(int sn, SignalInfo info);

	async::result<uint64_t> pollSignal(uint64_t in_seq, uint64_t mask,
			async::cancellation_token cancellation = {});

	SignalItem *fetchSignal(uint64_t mask);

	// ------------------------------------------------------------------------
	// Signal context manipulation.
	// ------------------------------------------------------------------------
	
	void raiseContext(SignalItem *item, helix::BorrowedDescriptor thread);

	void restoreContext(helix::BorrowedDescriptor thread);

private:
	SignalHandler _handlers[64];
	SignalSlot _slots[64];
	
	async::doorbell _signalBell;
	uint64_t _currentSeq;
	uint64_t _activeSet;
};

enum class NotifyType {
	null,
	terminated
};

struct ResourceUsage {
	uint64_t userTime;
};

struct Generation {
	helix::UniqueLane posixLane;
	async::cancellation_event cancelServe;
};

struct Process : std::enable_shared_from_this<Process> {
	static std::shared_ptr<Process> findProcess(ProcessId pid);

	static async::result<std::shared_ptr<Process>> init(std::string path);

	static std::shared_ptr<Process> fork(std::shared_ptr<Process> parent);

	static async::result<void> exec(std::shared_ptr<Process> process,
			std::string path, std::vector<std::string> args, std::vector<std::string> env);

	// Called when the PID is released (by waitpid()).
	static void retire(Process *process);

public:
	Process(Process *parent);

	Process *getParent() {
		return _parent;
	}

	int pid() {
		assert(_pid); // Do not return uninitialized information.
	 	return _pid;
	}

	Generation *currentGeneration() {
		return _currentGeneration;
	}

	std::string path() {
		return _path;
	}

	helix::BorrowedDescriptor threadDescriptor() {
		return _threadDescriptor;
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
	
	void setSignalMask(uint64_t mask) {
		_signalMask = mask;
	}

	uint64_t signalMask() {
		return _signalMask;
	}

	HelHandle clientPosixLane() { return _clientPosixLane; }
	void *clientFileTable() { return _clientFileTable; }
	void *clientClkTrackerPage() { return _clientClkTrackerPage; }

	void notify();

	async::result<int> wait(int pid, bool non_blocking);

	ResourceUsage accumulatedUsage() {
		return _accumulatedUsage;
	}

private:
	Process *_parent;

	int _pid;
	Generation *_currentGeneration;
	std::string _path;
	std::shared_ptr<VmContext> _vmContext;
	std::shared_ptr<FsContext> _fsContext;
	std::shared_ptr<FileContext> _fileContext;
	std::shared_ptr<SignalContext> _signalContext;

	HelHandle _clientPosixLane;
	void *_clientFileTable;
	void *_clientClkTrackerPage;

	helix::UniqueDescriptor _threadDescriptor;

	uint64_t _signalMask;
	std::vector<std::shared_ptr<Process>> _children;

	// The following intrusive queue stores notifications for wait(). 
	NotifyType _notifyType;

	boost::intrusive::list_member_hook<> _notifyHook;

	boost::intrusive::list<
		Process,
		boost::intrusive::member_hook<
			Process,
			boost::intrusive::list_member_hook<>,
			&Process::_notifyHook
		>
	> _notifyQueue;

	async::doorbell _notifyBell;

	// Resource usage accumulated from children.
	ResourceUsage _accumulatedUsage = {};
};

std::shared_ptr<Process> findProcessWithCredentials(const char *credentials);

#endif // POSIX_SUBSYSTEM_PROCESS_HPP
