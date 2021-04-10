
#include <signal.h>
#include <string.h>
#include <sys/auxv.h>

#include "common.hpp"
#include "clock.hpp"
#include "exec.hpp"
#include "process.hpp"

static bool logFileAttach = false;
static bool logCleanup = false;

async::result<void> serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation);

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
	HEL_CHECK(helCreateSpace(&space));
	context->_space = helix::UniqueDescriptor(space);

	for(const auto &entry : original->_areaTree) {
		const auto &[address, area] = entry;

		helix::UniqueDescriptor copyView;
		if(area.copyOnWrite) {
			HelHandle copyHandle;
			HEL_CHECK(helForkMemory(area.copyView.getHandle(), &copyHandle));
			copyView = helix::UniqueDescriptor{copyHandle};

			void *pointer;
			HEL_CHECK(helMapMemory(copyView.getHandle(), context->_space.getHandle(),
					reinterpret_cast<void *>(address),
					0, area.areaSize, area.nativeFlags, &pointer));
		}else{
			void *pointer;
			HEL_CHECK(helMapMemory(area.fileView.getHandle(), context->_space.getHandle(),
					reinterpret_cast<void *>(address),
					area.offset, area.areaSize, area.nativeFlags, &pointer));
		}

		Area copy;
		copy.copyOnWrite = area.copyOnWrite;
		copy.areaSize = area.areaSize;
		copy.nativeFlags = area.nativeFlags;
		copy.fileView = area.fileView.dup();
		copy.copyView = std::move(copyView);
		copy.file = area.file;
		copy.offset = area.offset;
		context->_areaTree.emplace(address, std::move(copy));
	}

	return context;
}

VmContext::~VmContext() {
	if(logCleanup)
		std::cout << "\e[33mposix: VmContext is destructed\e[39m" << std::endl;
}

async::result<void *>
VmContext::mapFile(uintptr_t hint, helix::UniqueDescriptor memory,
		smarter::shared_ptr<File, FileHandle> file,
		intptr_t offset, size_t size, bool copyOnWrite, uint32_t nativeFlags) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	helix::UniqueDescriptor copyView;
	void *pointer;
	if(copyOnWrite) {
		HelHandle handle;
		HEL_CHECK(helCopyOnWrite(memory.getHandle(),
				offset, alignedSize, &handle));
		copyView = helix::UniqueDescriptor{handle};

		HEL_CHECK(helMapMemory(copyView.getHandle(), _space.getHandle(),
				reinterpret_cast<void *>(hint),
				0, alignedSize, nativeFlags, &pointer));
	}else{
		HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
				reinterpret_cast<void *>(hint),
				offset, alignedSize, nativeFlags, &pointer));
	}
	//std::cout << "posix: VM_MAP returns " << pointer
	//		<< " (size: " << (void *)size << ")" << std::endl;

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + alignedSize);
	if(succ != _areaTree.begin()) {
		auto pred = std::prev(succ);
		assert(pred->first + pred->second.areaSize <= address);
	}

	// Construct the new area.
	Area area;
	area.copyOnWrite = copyOnWrite;
	area.areaSize = alignedSize;
	area.nativeFlags = nativeFlags;
	area.fileView = std::move(memory);
	area.copyView = std::move(copyView);
	area.file = std::move(file);
	area.offset = offset;
	_areaTree.emplace(address, std::move(area));

	co_return pointer;
}

async::result<void *> VmContext::remapFile(void *oldPointer,
		size_t oldSize, size_t newSize) {
	size_t alignedOldSize = (oldSize + 0xFFF) & ~size_t(0xFFF);
	size_t alignedNewSize = (newSize + 0xFFF) & ~size_t(0xFFF);

//	std::cout << "posix: Remapping " << oldPointer << std::endl;
	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(oldPointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == alignedOldSize);

	assert(!it->second.copyOnWrite);

	auto memory = co_await it->second.file->accessMemory();

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	void *pointer;
	HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
			nullptr, it->second.offset, alignedNewSize,
			it->second.nativeFlags, &pointer));
//	std::cout << "posix: VM_REMAP returns " << pointer << std::endl;

	// Unmap the old area.
	HEL_CHECK(helUnmapMemory(_space.getHandle(), oldPointer, alignedOldSize));

	// Construct the new area from the old one.
	Area area;
	area.copyOnWrite = it->second.copyOnWrite;
	area.areaSize = alignedNewSize;
	area.nativeFlags = it->second.nativeFlags;
	area.fileView = std::move(it->second.fileView);
	area.copyView = std::move(it->second.copyView);
	area.file = std::move(it->second.file);
	area.offset = it->second.offset;
	_areaTree.erase(it);

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + alignedNewSize);
	if(succ != _areaTree.begin()) {
		auto pred = std::prev(succ);
		assert(pred->first + pred->second.areaSize <= address);
	}

	_areaTree.insert({address, std::move(area)});

	co_return pointer;
}

async::result<void> VmContext::protectFile(void *pointer, size_t size, uint32_t protectionFlags) {
	size_t alignedSize = (size + 0xFFF) & ~size_t(0xFFF);

	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(pointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == alignedSize);

	helix::ProtectMemory protect;
	auto &&submit = helix::submitProtectMemory(_space, &protect,
			pointer, size, protectionFlags, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(protect.error());
	it->second.nativeFlags &= ~(kHelMapProtRead | kHelMapProtWrite | kHelMapProtExecute);
	it->second.nativeFlags |= protectionFlags;

	co_return;
}

void VmContext::unmapFile(void *pointer, size_t size) {
	size_t aligned_size = (size + 0xFFF) & ~size_t(0xFFF);

	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(pointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == aligned_size);

	HEL_CHECK(helUnmapMemory(_space.getHandle(), pointer, aligned_size));

	// Update our idea of the process' VM space.
	_areaTree.erase(it);
}

// ----------------------------------------------------------------------------
// FsContext.
// ----------------------------------------------------------------------------

std::shared_ptr<FsContext> FsContext::create() {
	auto context = std::make_shared<FsContext>();

	context->_root = rootPath();
	context->_workDir = rootPath();

	return context;
}

std::shared_ptr<FsContext> FsContext::clone(std::shared_ptr<FsContext> original) {
	auto context = std::make_shared<FsContext>();

	context->_root = original->_root;
	context->_workDir = original->_workDir;

	return context;
}

ViewPath FsContext::getRoot() {
	return _root;
}

ViewPath FsContext::getWorkingDirectory() {
	return _workDir;
}

void FsContext::changeRoot(ViewPath root) {
	_root = std::move(root);
}

void FsContext::changeWorkingDirectory(ViewPath workdir) {
	_workDir = std::move(workdir);
}

// ----------------------------------------------------------------------------
// FileContext.
// ----------------------------------------------------------------------------

std::shared_ptr<FileContext> FileContext::create() {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->_fileTableWindow = reinterpret_cast<HelHandle *>(window);

	unsigned long mbus_upstream;
	if(peekauxval(AT_MBUS_SERVER, &mbus_upstream))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	HEL_CHECK(helTransferDescriptor(mbus_upstream,
			context->_universe.getHandle(), &context->_clientMbusLane));

	return context;
}

std::shared_ptr<FileContext> FileContext::clone(std::shared_ptr<FileContext> original) {
	auto context = std::make_shared<FileContext>();

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));
	context->_universe = helix::UniqueDescriptor(universe);

	HelHandle memory;
	void *window;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr,
			0, 0x1000, kHelMapProtRead | kHelMapProtWrite, &window));
	context->_fileTableMemory = helix::UniqueDescriptor(memory);
	context->_fileTableWindow = reinterpret_cast<HelHandle *>(window);

	for(auto entry : original->_fileTable) {
		//std::cout << "Clone FD " << entry.first << std::endl;
		context->attachFile(entry.first, entry.second.file, entry.second.closeOnExec);
	}

	unsigned long mbus_upstream;
	if(peekauxval(AT_MBUS_SERVER, &mbus_upstream))
		throw std::runtime_error("No AT_MBUS_SERVER specified");
	HEL_CHECK(helTransferDescriptor(mbus_upstream,
			context->_universe.getHandle(), &context->_clientMbusLane));

	return context;
}

FileContext::~FileContext() {
	if(logCleanup)
		std::cout << "\e[33mposix: FileContext is destructed\e[39m" << std::endl;
}

int FileContext::attachFile(smarter::shared_ptr<File, FileHandle> file,
		bool close_on_exec) {
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(file->getPassthroughLane().getHandle(),
			_universe.getHandle(), &handle));

	for(int fd = 0; ; fd++) {
		if(_fileTable.find(fd) != _fileTable.end())
			continue;

		if(logFileAttach)
			std::cout << "posix: Attaching FD " << fd << std::endl;

		_fileTable.insert({fd, {std::move(file), close_on_exec}});
		_fileTableWindow[fd] = handle;
		return fd;
	}
}

void FileContext::attachFile(int fd, smarter::shared_ptr<File, FileHandle> file,
		bool close_on_exec) {
	HelHandle handle;
	HEL_CHECK(helTransferDescriptor(file->getPassthroughLane().getHandle(),
			_universe.getHandle(), &handle));

	if(logFileAttach)
		std::cout << "posix: Attaching fixed FD " << fd << std::endl;

	auto it = _fileTable.find(fd);
	if(it != _fileTable.end()) {
		it->second = {std::move(file), close_on_exec};
	}else{
		_fileTable.insert({fd, {std::move(file), close_on_exec}});
	}
	_fileTableWindow[fd] = handle;
}

std::optional<FileDescriptor> FileContext::getDescriptor(int fd) {
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return std::nullopt;
	return file->second;
}

Error FileContext::setDescriptor(int fd, bool close_on_exec) {
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		return Error::noSuchFile;
	}
	it->second.closeOnExec = close_on_exec;
	return Error::success;
}

smarter::shared_ptr<File, FileHandle> FileContext::getFile(int fd) {
	auto file = _fileTable.find(fd);
	if(file == _fileTable.end())
		return smarter::shared_ptr<File, FileHandle>{};
	return file->second.file;
}

void FileContext::closeFile(int fd) {
	if(logFileAttach)
		std::cout << "posix: Closing FD " << fd << std::endl;
	auto it = _fileTable.find(fd);
	if(it == _fileTable.end()) {
		std::cout << "\e[31m" "posix: Trying to close non-existant FD "
				<< fd << "\e[39m" << std::endl;
		return;
	}

	HEL_CHECK(helCloseDescriptor(_universe.getHandle(), _fileTableWindow[fd]));

	_fileTableWindow[fd] = 0;
	_fileTable.erase(it);
}

void FileContext::closeOnExec() {
	auto it = _fileTable.begin();
	while(it != _fileTable.end()) {
		if(it->second.closeOnExec) {
			HEL_CHECK(helCloseDescriptor(_universe.getHandle(), _fileTableWindow[it->first]));

			_fileTableWindow[it->first] = 0;
			it = _fileTable.erase(it);
		}else{
			it++;
		}
	}
}

// ----------------------------------------------------------------------------
// SignalContext.
// ----------------------------------------------------------------------------

namespace {

struct CompileSignalInfo {
	void operator() (const UserSignal &info) const {
		//si->si_code = SI_USER;
		si->si_pid = info.pid;
		si->si_uid = info.uid;
	}

	siginfo_t *si;
};

} // anonymous namespace

SignalContext::SignalContext()
: _currentSeq{1}, _activeSet{0} { }

std::shared_ptr<SignalContext> SignalContext::create() {
	auto context = std::make_shared<SignalContext>();

	// All signals use their default disposition.
	for(int sn = 1; sn <= 64; sn++)
		context->_handlers[sn - 1].disposition = SignalDisposition::none;

	return context;
}

std::shared_ptr<SignalContext> SignalContext::clone(std::shared_ptr<SignalContext> original) {
	auto context = std::make_shared<SignalContext>();

	// Copy the current signal handler table.
	for(int sn = 1; sn <= 64; sn++)
		context->_handlers[sn - 1] = original->_handlers[sn];

	return context;
}

void SignalContext::resetHandlers() {
	for(int sn = 1; sn <= 64; sn++)
		if(_handlers[sn - 1].disposition == SignalDisposition::handle)
			_handlers[sn - 1].disposition = SignalDisposition::none;
}

SignalHandler SignalContext::getHandler(int sn) {
	return _handlers[sn - 1];
}

SignalHandler SignalContext::changeHandler(int sn, SignalHandler handler) {
	assert(sn - 1 < 64);
	return std::exchange(_handlers[sn - 1], handler);
}

void SignalContext::issueSignal(int sn, SignalInfo info) {
	assert(sn - 1 < 64);
	auto item = new SignalItem;
	item->signalNumber = sn;
	item->info = info;

	_slots[sn - 1].raiseSeq = ++_currentSeq;
	_slots[sn - 1].asyncQueue.push_back(*item);
	_activeSet |= (UINT64_C(1) << (sn - 1));
	_signalBell.ring();
}

async::result<PollSignalResult>
SignalContext::pollSignal(uint64_t in_seq, uint64_t mask,
		async::cancellation_token cancellation) {
	assert(in_seq <= _currentSeq);

	while(in_seq == _currentSeq && !cancellation.is_cancellation_requested()) {
		auto future = _signalBell.async_wait();
		async::result_reference<void> ref = future;
		async::cancellation_callback cancel_callback{cancellation, [&] {
			_signalBell.cancel_async_wait(ref);
		}};
		co_await std::move(future);
	}

	// Wait until one of the requested signals becomes active.
	while(!(_activeSet & mask) && !cancellation.is_cancellation_requested()) {
		auto future = _signalBell.async_wait();
		async::result_reference<void> ref = future;
		async::cancellation_callback cancel_callback{cancellation, [&] {
			_signalBell.cancel_async_wait(ref);
		}};
		co_await std::move(future);
	}

	uint64_t edges = 0;
	for(int sn = 1; sn <= 64; sn++)
		if(_slots[sn - 1].raiseSeq > in_seq)
			edges |= UINT64_C(1) << (sn - 1);

	co_return PollSignalResult{_currentSeq, edges};
}

CheckSignalResult SignalContext::checkSignal() {
	return CheckSignalResult(_currentSeq, _activeSet);
}

async::result<SignalItem *> SignalContext::fetchSignal(uint64_t mask, bool nonBlock) {
	int sn;
	while(true) {
		for(sn = 1; sn <= 64; sn++) {
			if(!(mask & (UINT64_C(1) << (sn - 1))))
				continue;
			if(!_slots[sn - 1].asyncQueue.empty())
				break;
		}
		if(sn - 1 != 64)
			break;
		if(nonBlock)
			co_return nullptr;
		co_await _signalBell.async_wait();
	}

	assert(!_slots[sn - 1].asyncQueue.empty());
	auto item = &_slots[sn - 1].asyncQueue.front();
	_slots[sn - 1].asyncQueue.pop_front();
	if(_slots[sn - 1].asyncQueue.empty())
		_activeSet &= ~(UINT64_C(1) << (sn - 1));

	co_return item;
}

// We follow a similar model as Linux. The linux layout is a follows:
// struct rt_sigframe. Placed at the top of the stack.
//     struct ucontext. Part of struct rt_sigframe.
//         struct sigcontext. Part of struct ucontext.
//             Actually stores the registers.
//             Stores a pointer to the FPU state.
//     siginfo_t. Part of struct rt_sigframe.
// FPU state is store at a higher (undefined) position on the stack.

// This is our signal frame, similar to Linux' struct rt_sigframe.
struct SignalFrame {
	uint64_t returnAddress; // Address for 'ret' instruction.
	uintptr_t gprs[kHelNumGprs];
	uintptr_t pcrs[2];
	siginfo_t info;
};

async::result<void> SignalContext::raiseContext(SignalItem *item, Process *process,
		bool &killed) {
	auto thread = process->threadDescriptor();

	SignalHandler handler = _handlers[item->signalNumber - 1];

	// Implement SA_RESETHAND by resetting the signal disposition to default.
	if(handler.flags & signalOnce)
		_handlers[item->signalNumber].disposition = SignalDisposition::none;

	if(handler.disposition == SignalDisposition::none) {
		if(item->signalNumber == SIGCHLD) { // TODO: Handle default actions generically.
			// Ignore the signal.
			killed = false;
			co_return;
		}else{
			std::cout << "posix: Thread killed as the result of signal "
							<< item->signalNumber << std::endl;
			killed = true;
			co_await process->terminate(TerminationBySignal{item->signalNumber});
			co_return;
		}
	} else if(handler.disposition == SignalDisposition::ignore) {
		// Ignore the signal.
		killed = false;
		co_return;
	}

	assert(handler.disposition == SignalDisposition::handle);
	killed = false;

	SignalFrame sf;
	memset(&sf, 0, sizeof(SignalFrame));
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &sf.gprs));
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &sf.pcrs));

	sf.returnAddress = handler.restorerIp;

	// Once compile siginfo_t if that is neccessary (matches Linux behavior).
	if(handler.flags & signalInfo) {
		sf.info.si_signo = item->signalNumber;
		std::visit(CompileSignalInfo{&sf.info}, item->info);
	}

	// Setup the stack frame.
	uintptr_t nsp = sf.pcrs[kHelRegSp] - 128;

	auto alignFrame = [&] (size_t size) -> uintptr_t {
		nsp = ((nsp - size) & ~uintptr_t(15)) - 8;
		return nsp;
	};

	// Store the current register stack on the stack.
	assert(alignof(SignalFrame) == 8);
	auto frame = alignFrame(sizeof(SignalFrame));
	auto storeFrame = co_await helix_ng::writeMemory(thread, frame,
			sizeof(SignalFrame), &sf);
	HEL_CHECK(storeFrame.error());

	std::cout << "posix: Saving pre-signal stack to " << (void *)frame << std::endl;
	std::cout << "posix: Calling signal handler at " << (void *)handler.handlerIp << std::endl;

	// Setup the new register image and resume.
	// TODO: Linux sets rdx to the ucontext.
#if defined(__x86_64__)
	sf.gprs[kHelRegRdi] = item->signalNumber;
	sf.gprs[kHelRegRsi] = frame + offsetof(SignalFrame, info);
	sf.gprs[kHelRegRax] = 0; // Number of variable arguments.
#elif defined(__aarch64__)
	sf.gprs[kHelRegX0] = item->signalNumber;
	sf.gprs[kHelRegX1] = frame + offsetof(SignalFrame, info);
#endif

	sf.pcrs[kHelRegIp] = handler.handlerIp;
	sf.pcrs[kHelRegSp] = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &sf.gprs));
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsProgram, &sf.pcrs));

	delete item;
}

async::result<void> SignalContext::restoreContext(helix::BorrowedDescriptor thread) {
	uintptr_t pcrs[2];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
	auto frame = pcrs[kHelRegSp] - 8;

	std::cout << "posix: Restoring post-signal stack from " << (void *)frame << std::endl;

	SignalFrame sf;
	auto loadFrame = co_await helix_ng::readMemory(thread, frame,
			sizeof(SignalFrame), &sf);
	HEL_CHECK(loadFrame.error());

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &sf.gprs));
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsProgram, &sf.pcrs));
}

// ----------------------------------------------------------------------------
// Generation.
// ----------------------------------------------------------------------------

Generation::~Generation() {
	if(logCleanup)
		std::cout << "\e[33mposix: Generation is destructed\e[39m" << std::endl;
}

// ----------------------------------------------------------------------------
// Process.
// ----------------------------------------------------------------------------

// PID 1 is reserved for the init process, therefore we start at 2.
ProcessId nextPid = 2;
std::map<ProcessId, PidHull *> globalPidMap;

PidHull::PidHull(pid_t pid)
: pid_{pid} {
	auto [it, success] = globalPidMap.insert({pid_, this});
	assert(success);
	(void)it;
}

PidHull::~PidHull() {
	auto it = globalPidMap.find(pid_);
	assert(it != globalPidMap.end());
	globalPidMap.erase(it);
}

void PidHull::initializeProcess(Process *process) {
	process_ = process->weak_from_this();
}

void PidHull::initializeTerminalSession(TerminalSession *session) {
	// TODO: verify that no terminal session is associated with this PidHull.
	terminalSession_ = session->weak_from_this();
}

void PidHull::initializeProcessGroup(ProcessGroup *group) {
	// TODO: verify that no process group is associated with this PidHull.
	processGroup_ = group->weak_from_this();
}

std::shared_ptr<Process> PidHull::getProcess() {
	return process_.lock();
}

std::shared_ptr<ProcessGroup> PidHull::getProcessGroup() {
	return processGroup_.lock();
}

std::shared_ptr<TerminalSession> PidHull::getTerminalSession() {
	return terminalSession_.lock();
}

std::shared_ptr<Process> Process::findProcess(ProcessId pid) {
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->getProcess();
}

Process::Process(std::shared_ptr<PidHull> hull, Process *parent)
: _parent{parent}, _hull{std::move(hull)},
		_clientPosixLane{kHelNullHandle}, _clientFileTable{nullptr},
		_notifyType{NotifyType::null} { }

Process::~Process() {
	std::cout << "\e[33mposix: Process is destructed\e[39m" << std::endl;
	_pgPointer->dropProcess(this);
}

bool Process::checkSignalRaise() {
	auto p = reinterpret_cast<unsigned int *>(accessThreadPage());
	unsigned int gsf = __atomic_load_n(p, __ATOMIC_RELAXED);
	if(!gsf)
		return true;
	return false;
}

bool Process::checkOrRequestSignalRaise() {
	auto p = reinterpret_cast<unsigned int *>(accessThreadPage());
	unsigned int gsf = __atomic_load_n(p, __ATOMIC_RELAXED);
	if(!gsf)
		return true;
	if(gsf == 1) {
		__atomic_store_n(p, 2, __ATOMIC_RELAXED);
	}else if(gsf != 2) {
		std::cout << "\e[33m" "posix: Ignoring unexpected value "
				<< gsf << " of global signal flag" "\e[39m" << std::endl;
	}
	return false;
}

async::result<std::shared_ptr<Process>> Process::init(std::string path) {
	auto hull = std::make_shared<PidHull>(1);
	auto process = std::make_shared<Process>(std::move(hull), nullptr);
	process->_path = path;
	process->_vmContext = VmContext::create();
	process->_fsContext = FsContext::create();
	process->_fileContext = FileContext::create();
	process->_signalContext = SignalContext::create();

	TerminalSession::initializeNewSession(process.get());

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};

	// The initial signal mask allows all signals.
	process->_signalMask = 0;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(client_lane.getHandle(),
			process->_fileContext->getUniverse().getHandle(), &process->_clientPosixLane));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientThreadPage));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientClkTrackerPage));

	process->_uid = 0;
	process->_euid = 0;
	process->_gid = 0;
	process->_egid = 0;
	process->_hull->initializeProcess(process.get());

	// TODO: Do not pass an empty argument vector?
	auto threadResult = co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::vector<std::string>{}, std::vector<std::string>{},
			process->_vmContext,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	if(!threadResult)
		throw std::logic_error("Could not execute() init process");

	process->_threadDescriptor = std::move(threadResult.value());
	process->_posixLane = std::move(server_lane);
	process->_didExecute = true;

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	helResume(process->_threadDescriptor.getHandle());
	async::detach(serve(process, std::move(generation)));

	co_return process;
}

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> original) {
	auto hull = std::make_shared<PidHull>(nextPid++);
	auto process = std::make_shared<Process>(std::move(hull), original.get());
	process->_path = original->path();
	process->_vmContext = VmContext::clone(original->_vmContext);
	process->_fsContext = FsContext::clone(original->_fsContext);
	process->_fileContext = FileContext::clone(original->_fileContext);
	process->_signalContext = SignalContext::clone(original->_signalContext);

	original->_pgPointer->reassociateProcess(process.get());

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};

	// Signal masks are copied on fork().
	process->_signalMask = original->_signalMask;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(client_lane.getHandle(),
			process->_fileContext->getUniverse().getHandle(), &process->_clientPosixLane));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientThreadPage));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&process->_clientClkTrackerPage));

	process->_uid = original->_uid;
	process->_euid = original->_euid;
	process->_gid = original->_gid;
	process->_egid = original->_egid;
	original->_children.push_back(process);
	process->_hull->initializeProcess(process.get());
	process->_didExecute = false;

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
			0, 0, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};
	process->_posixLane = std::move(server_lane);

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	async::detach(serve(process, std::move(generation)));

	return process;
}

std::shared_ptr<Process> Process::clone(std::shared_ptr<Process> original, void *ip, void *sp) {
	auto hull = std::make_shared<PidHull>(nextPid++);
	auto process = std::make_shared<Process>(std::move(hull), original.get());
	process->_path = original->path();
	process->_vmContext = original->_vmContext;
	process->_fsContext = original->_fsContext;
	process->_fileContext = original->_fileContext;
	process->_signalContext = original->_signalContext;

	// TODO: ProcessGroups should probably store ThreadGroups and not processes.
	original->_pgPointer->reassociateProcess(process.get());

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &thread_memory));
	process->_threadPageMemory = helix::UniqueDescriptor{thread_memory};
	process->_threadPageMapping = helix::Mapping{process->_threadPageMemory, 0, 0x1000};

	// Signal masks are copied on clone().
	process->_signalMask = original->_signalMask;

	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(client_lane.getHandle(),
			process->_fileContext->getUniverse().getHandle(), &process->_clientPosixLane));
	client_lane.release();

	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&process->_clientThreadPage));

	process->_clientFileTable = original->_clientFileTable;
	process->_clientClkTrackerPage = original->_clientClkTrackerPage;

	process->_uid = original->_uid;
	process->_euid = original->_euid;
	process->_gid = original->_gid;
	process->_egid = original->_egid;
	original->_children.push_back(process);
	process->_hull->initializeProcess(process.get());
	process->_didExecute = false;

	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
			ip, sp, kHelThreadStopped, &new_thread));
	process->_threadDescriptor = helix::UniqueDescriptor{new_thread};
	process->_posixLane = std::move(server_lane);

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	async::detach(serve(process, std::move(generation)));

	return process;
}

async::result<Error> Process::exec(std::shared_ptr<Process> process,
		std::string path, std::vector<std::string> args, std::vector<std::string> env) {
	auto exec_vm_context = VmContext::create();

	// Perform the exec() in a new VM context so that we
	// can catch errors before trashing the calling process.
	auto threadResult = co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::move(args), std::move(env), exec_vm_context,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	if(!threadResult) {
		switch(threadResult.error()) {
		case Error::noSuchFile:
		case Error::badExecutable:
			co_return threadResult.error();
		default:
			throw std::logic_error("Unexpected error from execute()");
		}
	}

	// Allocate resources.
	HelHandle exec_posix_lane;
	auto [server_lane, client_lane] = helix::createStream();
	HEL_CHECK(helTransferDescriptor(client_lane.getHandle(),
			process->_fileContext->getUniverse().getHandle(), &exec_posix_lane));
	client_lane.release();

	void *exec_thread_page;
	void *exec_clk_tracker_page;
	void *exec_client_table;
	HEL_CHECK(helMapMemory(process->_threadPageMemory.getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			&exec_thread_page));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&exec_clk_tracker_page));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead,
			&exec_client_table));

	// Kill the old thread.
	// After this is done, we cannot roll back the exec() operation.
	HEL_CHECK(helKillThread(process->_threadDescriptor.getHandle()));
	auto previousGeneration = process->_currentGeneration;
	previousGeneration->inTermination = true;
	previousGeneration->cancelServe.cancel();
	co_await previousGeneration->signalsDone.wait();
	co_await previousGeneration->requestsDone.wait();

	// Perform pre-exec() work.
	// From here on, we can now release resources of the old process image.
	process->_fileContext->closeOnExec();

	// "Commit" the exec() operation.
	process->_path = std::move(path);
	process->_posixLane = std::move(server_lane);
	process->_threadDescriptor = std::move(threadResult.value());
	process->_vmContext = std::move(exec_vm_context);
	process->_signalContext->resetHandlers();
	process->_clientThreadPage = exec_thread_page;
	process->_clientPosixLane = exec_posix_lane;
	process->_clientFileTable = exec_client_table;
	process->_clientClkTrackerPage = exec_clk_tracker_page;
	process->_didExecute = true;

	auto generation = std::make_shared<Generation>();
	process->_currentGeneration = generation;
	helResume(process->_threadDescriptor.getHandle());
	async::detach(serve(process, std::move(generation)));

	co_return Error::success;
}

void Process::retire(Process *process) {
	assert(process->_parent);
	process->_parent->_childrenUsage.userTime += process->_generationUsage.userTime;
}

async::result<void> Process::terminate(TerminationState state) {
	auto parent = getParent();
	assert(parent);

	// Kill the current thread and accumulate stats.
	HEL_CHECK(helKillThread(_threadDescriptor.getHandle()));
	_currentGeneration->inTermination = true;
	_currentGeneration->cancelServe.cancel();
	co_await _currentGeneration->signalsDone.wait();
	co_await _currentGeneration->requestsDone.wait();

	// TODO: Also do this before switching to a new Generation in execve().
	// TODO: Do the accumulation + _currentGeneration reset after the thread has really terminated?
	HelThreadStats stats;
	HEL_CHECK(helQueryThreadStats(_threadDescriptor.getHandle(), &stats));
	_generationUsage.userTime += stats.userTime;

	_posixLane = {};
	_threadDescriptor = {};
	_vmContext = nullptr;
	_fsContext = nullptr;
	_fileContext = nullptr;
	//_signalContext = nullptr; // TODO: Migrate the notifications to PID 1.
	_currentGeneration = nullptr;

	// Notify the parent of our status change.
	assert(_notifyType == NotifyType::null);
	_notifyType = NotifyType::terminated;
	_state = std::move(state);
	parent->_notifyQueue.push_back(*this);
	parent->_notifyBell.ring();

	// Send SIGCHLD to the parent.
	UserSignal info;
	info.pid = pid();
	parent->signalContext()->issueSignal(SIGCHLD, info);
}

async::result<int> Process::wait(int pid, bool nonBlocking, TerminationState *state) {
	assert(pid == -1 || pid > 0);

	int result = 0;
	TerminationState resultState;
	while(true) {
		for(auto it = _notifyQueue.begin(); it != _notifyQueue.end(); ++it) {
			if(pid > 0 && pid != it->pid())
				continue;
			_notifyQueue.erase(it);
			result = it->pid();
			resultState = it->_state;
			Process::retire(&(*it));
			break;
		}

		if(result > 0 || nonBlocking) {
			*state = resultState;
			co_return result;
		}
		co_await _notifyBell.async_wait();
	}
}

// --------------------------------------------------------------------------------------
// Process groups and sessions.
// --------------------------------------------------------------------------------------

ProcessGroup::ProcessGroup(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

ProcessGroup::~ProcessGroup() {
	sessionPointer_->dropGroup(this);
}

void ProcessGroup::reassociateProcess(Process *process) {
	if(process->_pgPointer) {
		auto oldGroup = process->_pgPointer.get();
		oldGroup->members_.erase(oldGroup->members_.iterator_to(*process));
	}
	process->_pgPointer = shared_from_this();
	members_.push_back(*process);
}

void ProcessGroup::dropProcess(Process *process) {
	assert(process->_pgPointer.get() == this);
	members_.erase(members_.iterator_to(*process));
	// Note: this assignment can destruct 'this'.
	process->_pgPointer = nullptr;
}

void ProcessGroup::issueSignalToGroup(int sn, SignalInfo info) {
	for(auto &processRef : members_)
		processRef.signalContext()->issueSignal(sn, info);
}

TerminalSession::TerminalSession(std::shared_ptr<PidHull> hull)
: hull_{std::move(hull)} { }

TerminalSession::~TerminalSession() {
	if(ctsPointer_)
		ctsPointer_->dropSession(this);
}

pid_t TerminalSession::getSessionId() {
	return hull_->getPid();
}

std::shared_ptr<TerminalSession> TerminalSession::initializeNewSession(Process *sessionLeader) {
	auto session = std::make_shared<TerminalSession>(sessionLeader->getHull()->shared_from_this());
	auto group = session->spawnProcessGroup(sessionLeader);
	session->foregroundGroup_ = group.get();
	session->hull_->initializeTerminalSession(session.get());
	return session;
}

std::shared_ptr<ProcessGroup> TerminalSession::spawnProcessGroup(Process *groupLeader) {
	auto group = std::make_shared<ProcessGroup>(groupLeader->getHull()->shared_from_this());
	group->reassociateProcess(groupLeader);
	group->sessionPointer_ = shared_from_this();
	groups_.push_back(*group);
	group->hull_->initializeProcessGroup(group.get());
	return group;
}

std::shared_ptr<ProcessGroup> TerminalSession::getProcessGroupById(pid_t id) {
	for(auto &i : groups_) {
		if(i.getHull()->getPid() == id)
			return i.getHull()->getProcessGroup()->shared_from_this();
	}
	return nullptr;
}

void TerminalSession::dropGroup(ProcessGroup *group) {
	assert(group->sessionPointer_.get() == this);
	if(foregroundGroup_ == group)
		foregroundGroup_ = nullptr;
	groups_.erase(groups_.iterator_to(*group));
	// Note: this assignment can destruct 'this'.
	group->sessionPointer_ = nullptr;
}

Error TerminalSession::setForegroundGroup(ProcessGroup *group) {
	assert(group);
	if(group->sessionPointer_.get() != this)
		return Error::insufficientPermissions;
	foregroundGroup_ = group;
	return Error::success;
}

Error ControllingTerminalState::assignSessionOf(Process *process) {
	auto group = process->_pgPointer.get();
	auto session = group->sessionPointer_.get();
	if(process->getHull() != session->hull_.get())
		return Error::illegalArguments; // Process is not a session leader.
	if(associatedSession_)
		return Error::insufficientPermissions;
	if(session->ctsPointer_)
		return Error::insufficientPermissions;
	associatedSession_ = session;
	session->ctsPointer_ = this;
	return Error::success;
}

void ControllingTerminalState::dropSession(TerminalSession *session) {
	assert(associatedSession_ == session);
	associatedSession_ = nullptr;
	session->ctsPointer_ = nullptr;
}

void ControllingTerminalState::issueSignalToForegroundGroup(int sn, SignalInfo info) {
	if(!associatedSession_)
		return;
	if(!associatedSession_->foregroundGroup_)
		return;
	associatedSession_->foregroundGroup_->issueSignalToGroup(sn, info);
}
