#pragma once

#include <map>
#include <memory>
#include <unordered_map>

#include <async/result.hpp>
#include <async/doorbell.hpp>
#include <boost/intrusive/list.hpp>

#include "vfs.hpp"

struct Generation;
struct Process;
struct ProcessGroup;
struct TerminalSession;
struct ControllingTerminalState;

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
	async::result<void *> mapFile(uintptr_t hint, helix::UniqueDescriptor memory,
			smarter::shared_ptr<File, FileHandle> file,
			intptr_t offset, size_t size, bool copyOnWrite, uint32_t nativeFlags);

	async::result<void *> remapFile(void *old_pointer, size_t old_size, size_t new_size);

	async::result<void> protectFile(void *pointer, size_t size, uint32_t protectionFlags);

	void unmapFile(void *pointer, size_t size);

private:
	struct Area {
		bool copyOnWrite;
		size_t areaSize;
		uint32_t nativeFlags;
		helix::UniqueDescriptor fileView;
		helix::UniqueDescriptor copyView;
		smarter::shared_ptr<File, FileHandle> file;
		intptr_t offset;
	};

	helix::UniqueDescriptor _space;

	std::map<uintptr_t, Area> _areaTree;

public:
	struct AreaAccessor {
		AreaAccessor(std::map<uintptr_t, Area>::iterator iter)
		:_it{iter} {}

		uintptr_t baseAddress() {
			return _it->first;
		}

		size_t size() {
			return _it->second.areaSize;
		}

		bool isPrivate() {
			return _it->second.copyOnWrite;
		}

		bool isReadable() {
			return _it->second.nativeFlags & kHelMapProtRead;
		}

		bool isWritable() {
			return _it->second.nativeFlags & kHelMapProtWrite;
		}

		bool isExecutable() {
			return _it->second.nativeFlags & kHelMapProtExecute;
		}

		smarter::borrowed_ptr<File, FileHandle> backingFile() {
			return _it->second.file;
		}

		intptr_t backingFileOffset() {
			return _it->second.offset;
		}

	private:
		std::map<uintptr_t, Area>::iterator _it;
	};

	struct AreaIterator {
		AreaIterator(std::map<uintptr_t, Area>::iterator iter)
		:_it{iter} {}

		AreaIterator &operator++() {
			_it++;
			return *this;
		}

		AreaAccessor operator*() {
			return AreaAccessor{_it};
		}

		bool operator!=(const AreaIterator &other) {
			return _it != other._it;
		}
	private:
		std::map<uintptr_t, Area>::iterator _it;
	};

	AreaIterator begin() {
		return AreaIterator{_areaTree.begin()};
	}

	AreaIterator end() {
		return AreaIterator{_areaTree.end()};
	}
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

	~FileContext();

	helix::BorrowedDescriptor getUniverse() {
		return _universe;
	}

	helix::BorrowedDescriptor fileTableMemory() {
		return _fileTableMemory;
	}

	int attachFile(smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false);

	void attachFile(int fd, smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false);

	std::optional<FileDescriptor> getDescriptor(int fd);

	Error setDescriptor(int fd, bool close_on_exec);

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

using PollSignalResult = std::tuple<uint64_t, uint64_t, uint64_t>;

struct SignalContext {
private:
	struct SignalSlot {
		uint64_t raiseSeq = 0;

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

	SignalHandler getHandler(int sn);
	SignalHandler changeHandler(int sn, SignalHandler handler);

	void issueSignal(int sn, SignalInfo info);

	async::result<PollSignalResult> pollSignal(uint64_t in_seq, uint64_t mask,
			async::cancellation_token cancellation = {});

	PollSignalResult checkSignal(uint64_t mask);

	SignalItem *fetchSignal(uint64_t mask);

	// ------------------------------------------------------------------------
	// Signal context manipulation.
	// ------------------------------------------------------------------------

	void raiseContext(SignalItem *item, Process *process, Generation *generation);

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

struct TerminationByExit {
	int code;
};

struct TerminationBySignal {
	int signo;
};

using TerminationState = std::variant<
	std::monostate,
	TerminationByExit,
	TerminationBySignal
>;

struct ResourceUsage {
	uint64_t userTime;
};

// Represents exactly the state of a process that is changed by execve().
// In particular, stores the kernel thread.
struct Generation {
	~Generation();

	helix::UniqueLane posixLane;
	helix::UniqueDescriptor threadDescriptor;
	async::cancellation_event cancelServe;
};

struct ThreadPage {
	int globalSignalFlag;
};

// --------------------------------------------------------------------------------------
// The 'Process' class.
// --------------------------------------------------------------------------------------

struct Process : std::enable_shared_from_this<Process> {
	friend struct ProcessGroup;
	friend struct TerminalSession;
	friend struct ControllingTerminalState;

	static std::shared_ptr<Process> findProcess(ProcessId pid);

	static async::result<std::shared_ptr<Process>> init(std::string path);

	static std::shared_ptr<Process> fork(std::shared_ptr<Process> parent);
	static std::shared_ptr<Process> clone(std::shared_ptr<Process> parent, void *ip, void *sp);

	static async::result<Error> exec(std::shared_ptr<Process> process,
			std::string path, std::vector<std::string> args, std::vector<std::string> env);

	// Called when the PID is released (by waitpid()).
	static void retire(Process *process);

public:
	Process(Process *parent);

	~Process();

	Process *getParent() {
		return _parent;
	}

	int pid() {
		assert(_pid); // Do not return uninitialized information.
	 	return _pid;
	}

	Error setUid(int uid) {
		if(uid < 0) {
			return Error::illegalArguments;
		}
		if(_uid == 0 || _euid == 0) {
			_uid = uid;
			_euid = uid;
			return Error::success;
		} else if(uid == _uid) {
			_uid = uid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int uid() {
		return _uid;
	}

	Error setEuid(int euid) {
		if(euid < 0) {
			return Error::illegalArguments;
		}
		if(_uid == 0 || _euid == 0 || euid == _uid) {
			_euid = euid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int euid() {
		return _euid;
	}

	Error setGid(int gid) {
		if(gid < 0) {
			return Error::illegalArguments;
		}
		if(_gid == 0 || _egid == 0) {
			_gid = gid;
			_egid = gid;
			return Error::success;
		} else if(gid == _gid) {
			_egid = gid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int gid() {
		return _gid;
	}

	Error setEgid(int egid) {
		if(egid < 0) {
			return Error::illegalArguments;
		}
		if(_gid == 0 || _egid == 0 || _gid == egid || _egid == egid) {
			_egid = egid;
			return Error::success;
		}
		return Error::accessDenied;
	}

	int egid() {
		return _egid;
	}

	std::shared_ptr<Generation> currentGeneration() {
		return _currentGeneration;
	}

	std::string path() {
		return _path;
	}

	// As the contexts associated with a process can change (e.g. when unshare() is implemented),
	// those functions return refcounted pointers.
	std::shared_ptr<VmContext> vmContext() { return _vmContext; }
	std::shared_ptr<FsContext> fsContext() { return _fsContext; }
	std::shared_ptr<FileContext> fileContext() { return _fileContext; }
	SignalContext *signalContext() { return _signalContext.get(); }

	void setSignalMask(uint64_t mask) {
		_signalMask = mask;
	}

	uint64_t signalMask() {
		return _signalMask;
	}

	HelHandle clientPosixLane() { return _clientPosixLane; }
	void *clientThreadPage() { return _clientThreadPage; }
	void *clientFileTable() { return _clientFileTable; }
	void *clientClkTrackerPage() { return _clientClkTrackerPage; }

	ThreadPage *accessThreadPage() {
		return reinterpret_cast<ThreadPage *>(_threadPageMapping.get());
	}

	// Like checkOrRequestSignalRaise() but only check if raising is possible.
	bool checkSignalRaise();

	// Check if signals can currently be raised (via the thread page).
	// If not, request the thread to raise its signals.
	// Preconditon: the thread has to be stopped!
	bool checkOrRequestSignalRaise();

	void terminate(TerminationState state);

	async::result<int> wait(int pid, bool nonBlocking, TerminationState *state);

	ResourceUsage accumulatedUsage() {
		return _childrenUsage;
	}

private:
	Process *_parent;

	int _pid;
	int _uid;
	int _euid;
	int _gid;
	int _egid;
	std::shared_ptr<Generation> _currentGeneration;
	std::string _path;
	std::shared_ptr<VmContext> _vmContext;
	std::shared_ptr<FsContext> _fsContext;
	std::shared_ptr<FileContext> _fileContext;
	std::shared_ptr<SignalContext> _signalContext;

	std::shared_ptr<ProcessGroup> _pgPointer;
	boost::intrusive::list_member_hook<> _pgHook;

	helix::UniqueDescriptor _threadPageMemory;
	helix::Mapping _threadPageMapping;

	HelHandle _clientPosixLane;
	void *_clientThreadPage;
	void *_clientFileTable;
	void *_clientClkTrackerPage;

	uint64_t _signalMask;
	std::vector<std::shared_ptr<Process>> _children;

	// The following intrusive queue stores notifications for wait(). 
	NotifyType _notifyType;
	TerminationState _state;

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

	// Resource usage accumulated from previous generations.
	ResourceUsage _generationUsage = {};
	// Resource usage accumulated from children.
	ResourceUsage _childrenUsage = {};
};

std::shared_ptr<Process> findProcessWithCredentials(const char *credentials);

// --------------------------------------------------------------------------------------
// Process groups and sessions.
// --------------------------------------------------------------------------------------

struct ProcessGroup : std::enable_shared_from_this<ProcessGroup> {
	friend struct TerminalSession;
	friend struct ControllingTerminalState;

	~ProcessGroup();

	void reassociateProcess(Process *process);

	void dropProcess(Process *process);

private:
	boost::intrusive::list<
		Process,
		boost::intrusive::member_hook<
			Process,
			boost::intrusive::list_member_hook<>,
			&Process::_pgHook
		>
	> members_;

	Process *leaderProcess_ = nullptr;

	std::shared_ptr<TerminalSession> sessionPointer_;
	boost::intrusive::list_member_hook<> sessionHook_;
};

struct TerminalSession : std::enable_shared_from_this<TerminalSession> {
	friend struct ControllingTerminalState;

	~TerminalSession();

	static std::shared_ptr<TerminalSession> initializeNewSession(Process *sessionLeader);

	std::shared_ptr<ProcessGroup> spawnProcessGroup(Process *groupLeader);

	void dropGroup(ProcessGroup *group);

private:
	boost::intrusive::list<
		ProcessGroup,
		boost::intrusive::member_hook<
			ProcessGroup,
			boost::intrusive::list_member_hook<>,
			&ProcessGroup::sessionHook_
		>
	> groups_;

	ProcessGroup *leaderGroup_ = nullptr;

	ProcessGroup *foregroundGroup_ = nullptr;

	ControllingTerminalState *ctsPointer_ = nullptr;
};

struct ControllingTerminalState {
	Error assignSessionOf(Process *process);

	void dropSession(TerminalSession *session);

private:
	TerminalSession *associatedSession_ = nullptr;
};
