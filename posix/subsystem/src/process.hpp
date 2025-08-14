#pragma once

#include <map>
#include <memory>
#include <unordered_map>

#include <async/result.hpp>
#include <async/oneshot-event.hpp>
#include <async/recurring-event.hpp>
#include <core/cancel-events.hpp>
#include <boost/intrusive/list.hpp>
#include <protocols/posix/data.hpp>
#include <frg/expected.hpp>
#include <sys/time.h>

#include "interval-timer.hpp"
#include "vfs.hpp"
#include "procfs.hpp"

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

	~VmContext();

	helix::BorrowedDescriptor getSpace() {
		return _space;
	}

	// TODO: Pass abstract instead of hel flags to this function?
	async::result<frg::expected<Error, void *>> mapFile(uintptr_t hint, helix::UniqueDescriptor memory,
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

	std::pair<
		std::map<uintptr_t, Area>::iterator,
		std::map<uintptr_t, Area>::iterator
	> splitAreaOn_(uintptr_t addr, size_t size);

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
	mode_t getUmask();

	void changeRoot(ViewPath root);
	void changeWorkingDirectory(ViewPath root);
	mode_t setUmask(mode_t mask);

private:
	ViewPath _root;
	ViewPath _workDir;
	mode_t _umask = 0022;
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

	std::expected<int, Error> attachFile(smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false, int start_at = 0);

	std::expected<void, Error> attachFile(int fd, smarter::shared_ptr<File, FileHandle> file, bool close_on_exec = false);

	std::optional<FileDescriptor> getDescriptor(int fd);

	Error setDescriptor(int fd, bool close_on_exec);

	smarter::shared_ptr<File, FileHandle> getFile(int fd);

	Error closeFile(int fd);

	void closeOnExec();

	HelHandle clientMbusLane() {
		return _clientMbusLane;
	}

	const std::unordered_map<int, FileDescriptor> &fileTable() {
		return _fileTable;
	}

	void setFdLimit(uint64_t limit) {
		// TODO: increase the limit once we allow more than one shared fd -> HelHandle mapping page
		fdLimit_ = std::min(limit, 0x1000 / sizeof(HelHandle));
	}

private:
	helix::UniqueDescriptor _universe;

	// TODO: replace this by a tree that remembers gaps between keys.
	std::unordered_map<int, FileDescriptor> _fileTable;

	helix::UniqueDescriptor _fileTableMemory;

	HelHandle *_fileTableWindow;

	// TODO: increase the limit once we allow more than one shared fd -> HelHandle mapping page
	uint64_t fdLimit_ = 0x1000 / sizeof(HelHandle);

	HelHandle _clientMbusLane;
};

struct UserSignal {
	int pid = 0;
	int uid = 0;
};

struct TimerSignal {
	int timerId = 0;
};

struct ChildSignal {
	int code = 0;
	int pid = 0;
	int uid = 0;
	int status = 0;
	clock_t utime = 0;
	clock_t stime = 0;
};

using SignalInfo = std::variant<
	UserSignal,
	TimerSignal,
	ChildSignal
>;

using SignalFlags = uint32_t;

inline constexpr SignalFlags signalInfo = (1 << 0);
inline constexpr SignalFlags signalOnce = (1 << 1);
inline constexpr SignalFlags signalReentrant = (1 << 2);
inline constexpr SignalFlags signalOnStack = (1 << 3);
inline constexpr SignalFlags signalNoChildWait = (1 << 4);

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

using PollSignalResult = std::tuple<uint64_t, uint64_t>;
using CheckSignalResult = std::tuple<uint64_t, uint64_t>;

struct CompileSignalInfo {
	void operator() (const UserSignal &info) const;
	void operator() (const TimerSignal &info) const;
	void operator() (const ChildSignal &info) const;

	siginfo_t *si;
};

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

	CheckSignalResult checkSignal();

	async::result<SignalItem *> fetchSignal(uint64_t mask, bool nonBlock, async::cancellation_token ct = {});

	// ------------------------------------------------------------------------
	// Signal context manipulation.
	// ------------------------------------------------------------------------

	struct SignalHandling {
		bool killed = false;
		bool ignored = false;
		SignalHandler handler;
	};

	// As this function bumps the signal seq number, only call this exactly
	// once per SignalItem!
	SignalHandling determineHandling(SignalItem *item, Process *process);
	async::result<void> raiseContext(SignalItem *item, Process *process,
			SignalHandling handling);

	async::result<void> determineAndRaiseContext(SignalItem *item, Process *process,
			bool &killed);

	async::result<void> restoreContext(helix::BorrowedDescriptor thread, Process *process);

private:
	SignalHandler _handlers[64];
	SignalSlot _slots[64];

	async::recurring_event _signalBell;
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

using WaitFlags = uint32_t;
inline constexpr WaitFlags waitNonBlocking = 1;
inline constexpr WaitFlags waitLeaveZombie = 2;
inline constexpr WaitFlags waitExited = 4;

struct ResourceUsage {
	uint64_t userTime;
};

// This struct is mainly needed to coordinate the destruction of kernel threads
// and request handelers during exec(). The exec() and terminate() implementations
// use it to wait until all running request handlers are finished.
struct Generation {
	~Generation();

	bool inTermination = false;
	async::cancellation_event cancelServe;
	async::oneshot_event signalsDone;
	async::oneshot_event requestsDone;
};

// --------------------------------------------------------------------------------------
// The 'Process' class.
// --------------------------------------------------------------------------------------

// This struct owns the PID.
// It remains alive until the PID can be recycled.
struct PidHull : std::enable_shared_from_this<PidHull> {
	PidHull(pid_t pid);

	PidHull(const PidHull &) = delete;

	~PidHull();

	PidHull &operator= (const PidHull &) = delete;

	pid_t getPid() {
		return pid_;
	}

	void initializeProcess(Process *process);
	void initializeProcessGroup(ProcessGroup *group);
	void initializeTerminalSession(TerminalSession *session);

	std::shared_ptr<Process> getProcess();
	std::shared_ptr<ProcessGroup> getProcessGroup();
	std::shared_ptr<TerminalSession> getTerminalSession();

private:
	pid_t pid_;
	std::weak_ptr<Process> process_;
	std::weak_ptr<ProcessGroup> processGroup_;
	std::weak_ptr<TerminalSession> terminalSession_;
};

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
	Process(std::shared_ptr<PidHull> hull, Process *parent);

	~Process();

	Process *getParent() {
		return _parent;
	}

	PidHull *getHull() {
		return _hull.get();
	}

	int pid() {
		assert(_hull);
		return _hull->getPid();
	}

	int tid() {
		assert(_hull);
		return _hull->getPid();
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

	bool didExecute() {
		return _didExecute;
	}

	std::string path() {
		return _path;
	}

	std::string name() {
		return _name;
	}

	void setName(std::string name) {
		_name = name;
	}

	helix::BorrowedLane posixLane() {
		return _posixLane;
	}

	helix::BorrowedDescriptor threadDescriptor() {
		return _threadDescriptor;
	}

	// As the contexts associated with a process can change (e.g. when unshare() is implemented),
	// those functions return refcounted pointers.
	std::shared_ptr<VmContext> vmContext() { return _vmContext; }
	std::shared_ptr<FsContext> fsContext() { return _fsContext; }
	std::shared_ptr<FileContext> fileContext() { return _fileContext; }
	std::shared_ptr<ProcessGroup> pgPointer() { return _pgPointer; }
	SignalContext *signalContext() { return _signalContext.get(); }

	void setSignalMask(uint64_t mask) {
		_signalMask = mask;
	}

	uint64_t signalMask() {
		return _signalMask;
	}

	HelHandle clientPosixLane() { return _clientPosixLane; }
	posix::ThreadPage *clientThreadPage() { return _clientThreadPage; }
	void *clientFileTable() { return _clientFileTable; }
	void *clientClkTrackerPage() { return _clientClkTrackerPage; }
	void *clientAuxBegin() { return _clientAuxBegin; }
	void *clientAuxEnd() { return _clientAuxEnd; }

	posix::ThreadPage *accessThreadPage() {
		return reinterpret_cast<posix::ThreadPage *>(_threadPageMapping.get());
	}

	async::result<void> cancelEvent();

	// Like checkOrRequestSignalRaise() but only check if raising is possible.
	bool checkSignalRaise();

	// Check if signals can currently be raised (via the thread page).
	// If not, request the thread to raise its signals.
	// Preconditon: the thread has to be stopped!
	bool checkOrRequestSignalRaise();

	async::result<void> terminate(TerminationState state);

	struct WaitResult {
		int pid = 0;
		int uid = 0;
		TerminationState state = std::monostate{};
		ResourceUsage stats = {};
	};

	async::result<frg::expected<Error, WaitResult>>
	wait(int pid, WaitFlags flags, async::cancellation_token ct);

	bool hasChild(int pid);

	ResourceUsage accumulatedUsage() {
		return _childrenUsage;
	}

	bool isOnAltStack(uint64_t sp) {
		return sp >= _altStackSp && sp <= (_altStackSp + _altStackSize);
	}

	uint64_t altStackSp() {
		return _altStackSp;
	}

	size_t altStackSize() {
		return _altStackSize;
	}

	void setAltStackSp(uint64_t ptr, size_t size) {
		_altStackSp = ptr;
		_altStackSize = size;
	}

	bool isAltStackEnabled() {
		return _altStackEnabled;
	}

	void setAltStackEnabled(bool en) {
		_altStackEnabled = en;
	}

	uint64_t enteredSignalSeq() {
		return _enteredSignalSeq;
	}

	void enterSignal() {
		_enteredSignalSeq++;
	}

	void setParentDeathSignal(std::optional<int> sig) {
		parentDeathSignal_ = sig;
	}

	void setDumpable(bool dumpable) {
		dumpable_ = dumpable;
	}

	bool getDumpable() {
		return dumpable_;
	}

	NotifyType notifyType() const {
		return notifyType_;
	}

	async::result<bool> awaitNotifyTypeChange(async::cancellation_token token = {});

	async::result<void> coredump(TerminationState state);

	CancelEventRegistry &cancelEventRegistry() {
		return cancelEventRegistry_;
	}

	helix_ng::CredentialsView credentials() const {
		return {credentials_};
	}

	struct IntervalTimer : posix::IntervalTimer {
		IntervalTimer(std::weak_ptr<Process> process, uint64_t initial, uint64_t interval)
			: posix::IntervalTimer(initial, interval), process_{process} {}

		void raise(bool success) override {
			if(!success)
				return;

			auto proc_lock = process_.lock();
			if(proc_lock)
				proc_lock->signalContext()->issueSignal(SIGALRM, {});
		}

		void expired() override {
		}

	private:
		std::weak_ptr<Process> process_;
	};

	struct PosixTimer;

	struct PosixTimerContext {
		clockid_t clockid;
		std::shared_ptr<PosixTimer> timer = {};
		std::optional<int> tid = std::nullopt;
		int signo;
	};

	struct PosixTimer : posix::IntervalTimer {
		PosixTimer(std::weak_ptr<Process> proc, std::optional<int> tid,
			int signo, int timerId, uint64_t initial, uint64_t interval)
			: posix::IntervalTimer(initial, interval), process_{proc},
			tid{tid}, signo{signo}, timerId{timerId} {}

		void raise(bool success) override {
			if(!success)
				return;

			if(tid) {
				auto proc = process_.lock();
				if(!proc) {
					cancel();
					expired();
					return;
				}
				proc->signalContext()->issueSignal(signo, TimerSignal{static_cast<int>(timerId)});
			}
		}

		void expired() override {
			isExpired = true;
		}

	private:
		std::weak_ptr<Process> process_;
		std::optional<int> tid = std::nullopt;
		int signo;
		int timerId;
		bool isExpired = false;
	};

	std::shared_ptr<IntervalTimer> realTimer;
	std::unordered_map<int, std::shared_ptr<PosixTimerContext>> timers;
	id_allocator<int> timerIdAllocator{};

private:
	Process *_parent;

	std::shared_ptr<PidHull> _hull;
	int _uid;
	int _euid;
	int _gid;
	int _egid;
	bool _didExecute;
	std::string _path;
	std::string _name;
	helix::UniqueLane _posixLane;
	helix::UniqueDescriptor _threadDescriptor;
	std::shared_ptr<Generation> _currentGeneration;
	std::shared_ptr<VmContext> _vmContext;
	std::shared_ptr<FsContext> _fsContext;
	std::shared_ptr<FileContext> _fileContext;
	std::shared_ptr<SignalContext> _signalContext;
	std::shared_ptr<procfs::Link> _procfs_dir;

	std::shared_ptr<ProcessGroup> _pgPointer;
	boost::intrusive::list_member_hook<> _pgHook;

	helix::UniqueDescriptor _threadPageMemory;
	helix::Mapping _threadPageMapping;

	HelHandle _clientPosixLane;
	posix::ThreadPage *_clientThreadPage;
	void *_clientFileTable;
	void *_clientClkTrackerPage;
	// Pointers to the aux vector in the client.
	void *_clientAuxBegin = nullptr;
	void *_clientAuxEnd = nullptr;

	uint64_t _signalMask;
	std::vector<std::shared_ptr<Process>> _children;

	// The following intrusive queue stores notifications for wait().
	NotifyType notifyType_;
	async::recurring_event notifyTypeChange_;
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

	async::recurring_event _notifyBell;

	// Resource usage accumulated from previous generations.
	ResourceUsage _generationUsage = {};
	// Resource usage accumulated from children.
	ResourceUsage _childrenUsage = {};

	bool _altStackEnabled = false;
	uint64_t _altStackSp = 0;
	size_t _altStackSize = 0;

	// Used for tracking signals that happened between sigprocmask and
	// a call that resumes on a signal.
	uint64_t _enteredSignalSeq = 0;

	std::optional<int> parentDeathSignal_ = std::nullopt;
	// equivalent to PR_[SG]ET_DUMPABLE
	bool dumpable_ = true;

	CancelEventRegistry cancelEventRegistry_;
	std::array<char, 16> credentials_{};
};

std::shared_ptr<Process> findProcessWithCredentials(helix_ng::CredentialsView);

// --------------------------------------------------------------------------------------
// Process groups and sessions.
// --------------------------------------------------------------------------------------

struct ProcessGroup : std::enable_shared_from_this<ProcessGroup> {
	friend struct TerminalSession;
	friend struct ControllingTerminalState;

	static std::shared_ptr<ProcessGroup> findProcessGroup(ProcessId pgid);

	ProcessGroup(std::shared_ptr<PidHull> hull);

	~ProcessGroup();

	void reassociateProcess(Process *process);

	void dropProcess(Process *process);

	void issueSignalToGroup(int sn, SignalInfo info);

	PidHull *getHull() {
		return hull_.get();
	}

	TerminalSession *getSession() { return sessionPointer_.get(); }

private:
	std::shared_ptr<PidHull> hull_;

	boost::intrusive::list<
		Process,
		boost::intrusive::member_hook<
			Process,
			boost::intrusive::list_member_hook<>,
			&Process::_pgHook
		>
	> members_;

	std::shared_ptr<TerminalSession> sessionPointer_;
	boost::intrusive::list_member_hook<> sessionHook_;
};

struct TerminalSession : std::enable_shared_from_this<TerminalSession> {
	friend struct ControllingTerminalState;

	TerminalSession(std::shared_ptr<PidHull> hull);

	~TerminalSession();

	pid_t getSessionId();

	static std::shared_ptr<TerminalSession> initializeNewSession(Process *sessionLeader);

	std::shared_ptr<ProcessGroup> spawnProcessGroup(Process *groupLeader);

	std::shared_ptr<ProcessGroup> getProcessGroupById(pid_t id);

	ProcessGroup *getForegroundGroup() { return foregroundGroup_; }

	ControllingTerminalState *getControllingTerminal() { return ctsPointer_; }

	void dropGroup(ProcessGroup *group);

	Error setForegroundGroup(ProcessGroup *group);

private:
	std::shared_ptr<PidHull> hull_;

	boost::intrusive::list<
		ProcessGroup,
		boost::intrusive::member_hook<
			ProcessGroup,
			boost::intrusive::list_member_hook<>,
			&ProcessGroup::sessionHook_
		>
	> groups_;

	ProcessGroup *foregroundGroup_ = nullptr;

	ControllingTerminalState *ctsPointer_ = nullptr;
};

struct ControllingTerminalState {
	Error assignSessionOf(Process *process);

	void dropSession(TerminalSession *session);

	void issueSignalToForegroundGroup(int sn, SignalInfo info);

	TerminalSession *getSession() { return associatedSession_; }

private:
	TerminalSession *associatedSession_ = nullptr;
};
