
#include <signal.h>
#include <string.h>
#include <sys/auxv.h>

#include "common.hpp"
#include "clock.hpp"
#include "exec.hpp"
#include "process.hpp"

static bool logFileAttach = false;
static bool logCleanup = false;

void serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation);

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
	HEL_CHECK(helForkSpace(original->_space.getHandle(), &space));
	context->_space = helix::UniqueDescriptor(space);

	for(const auto &entry : context->_areaTree) {
		const auto &[address, area] = entry;
		Area copy;
		copy.areaSize = area.areaSize;
		copy.nativeFlags = area.nativeFlags;
		copy.memory = area.memory.dup();
		copy.file = area.file;
		copy.offset = area.offset;
		context->_areaTree.emplace(address, std::move(copy));
	}

	return context;
}

async::result<void *>
VmContext::mapFile(helix::UniqueDescriptor memory,
		smarter::shared_ptr<File, FileHandle> file,
		intptr_t offset, size_t size, uint32_t native_flags) {
	size_t aligned_size = (size + 0xFFF) & ~size_t(0xFFF);

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	void *pointer;
	HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
			nullptr, 0 /*offset*/, aligned_size, native_flags, &pointer));
//	std::cout << "posix: VM_MAP returns " << pointer << std::endl;

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + aligned_size);
	if(succ != _areaTree.begin()) {
		auto pred = std::prev(succ);
		assert(pred->first + pred->second.areaSize <= address);
	}

	// Construct the new area.
	Area area;
	area.areaSize = aligned_size;
	area.nativeFlags = native_flags;
	area.memory = std::move(memory);
	area.file = std::move(file);
	area.offset = offset;
	_areaTree.emplace(address, std::move(area));

	co_return pointer;
}

async::result<void *> VmContext::remapFile(void *old_pointer,
		size_t old_size, size_t new_size) {
	size_t aligned_old_size = (old_size + 0xFFF) & ~size_t(0xFFF);
	size_t aligned_new_size = (new_size + 0xFFF) & ~size_t(0xFFF);

//	std::cout << "posix: Remapping " << old_pointer << std::endl;
	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(old_pointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == aligned_old_size);

	auto memory = co_await it->second.file->accessMemory(it->second.offset);

	// Perform the actual mapping.
	// POSIX specifies that non-page-size mappings are rounded up and filled with zeros.
	void *pointer;
	HEL_CHECK(helMapMemory(memory.getHandle(), _space.getHandle(),
			nullptr, 0 /*offset*/, aligned_new_size, it->second.nativeFlags, &pointer));
//	std::cout << "posix: VM_REMAP returns " << pointer << std::endl;

	// Unmap the old area.
	HEL_CHECK(helUnmapMemory(_space.getHandle(), old_pointer, aligned_old_size));

	// Construct the new area from the old one.
	Area area;
	area.areaSize = aligned_new_size;
	area.nativeFlags = it->second.nativeFlags;
	area.file = std::move(it->second.file);
	area.offset = it->second.offset;
	_areaTree.erase(it);

	// Perform some sanity checking.
	auto address = reinterpret_cast<uintptr_t>(pointer);
	auto succ = _areaTree.lower_bound(address + aligned_new_size);
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
	HEL_CHECK(helAllocateMemory(0x1000, 0, &memory));
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
	HEL_CHECK(helAllocateMemory(0x1000, 0, &memory));
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

	_fileTableWindow[fd] = 0;
	_fileTable.erase(it);
}

void FileContext::closeOnExec() {
	auto it = _fileTable.begin();
	while(it != _fileTable.end()) {
		if(it->second.closeOnExec) {
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
	for(int sn = 0; sn < 64; sn++)
		context->_handlers[sn].disposition = SignalDisposition::none;

	return context;
}

std::shared_ptr<SignalContext> SignalContext::clone(std::shared_ptr<SignalContext> original) {
	auto context = std::make_shared<SignalContext>();

	// Copy the current signal handler table.
	for(int sn = 0; sn < 64; sn++)
		context->_handlers[sn] = original->_handlers[sn];

	return context;
}

void SignalContext::resetHandlers() {
	for(int sn = 0; sn < 64; sn++)
		if(_handlers[sn].disposition == SignalDisposition::handle)
			_handlers[sn].disposition = SignalDisposition::none;
}

SignalHandler SignalContext::getHandler(int sn) {
	return _handlers[sn];
}

SignalHandler SignalContext::changeHandler(int sn, SignalHandler handler) {
	assert(sn < 64);
	return std::exchange(_handlers[sn], handler);
}

void SignalContext::issueSignal(int sn, SignalInfo info) {
	assert(sn < 64);
	auto item = new SignalItem;
	item->signalNumber = sn;
	item->info = info;

	_slots[sn].raiseSeq = ++_currentSeq;
	_slots[sn].asyncQueue.push_back(*item);
	_activeSet |= (UINT64_C(1) << sn);
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
	for(int sn = 0; sn < 64; sn++)
		if(_slots[sn].raiseSeq > in_seq)
			edges |= UINT64_C(1) << sn;

	co_return PollSignalResult{_currentSeq, edges, _activeSet};
}

PollSignalResult SignalContext::checkSignal(uint64_t mask) {
	uint64_t edges = 0;
	for(int sn = 0; sn < 64; sn++)
		if(_slots[sn].raiseSeq > 0)
			edges |= UINT64_C(1) << sn;

	return PollSignalResult(_currentSeq, edges, _activeSet);
}

SignalItem *SignalContext::fetchSignal(uint64_t mask) {
	int sn;
	for(sn = 0; sn < 64; sn++) {
		if(!(mask & (UINT64_C(1) << sn)))
			continue;
		if(!_slots[sn].asyncQueue.empty())
			break;
	}
	if(sn == 64)
		return nullptr;

	assert(!_slots[sn].asyncQueue.empty());
	auto item = &_slots[sn].asyncQueue.front();
	_slots[sn].asyncQueue.pop_front();
	if(_slots[sn].asyncQueue.empty())
		_activeSet &= ~(UINT64_C(1) << sn);

	return item;
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
	uintptr_t gprs[15];
	uintptr_t pcrs[2];
	siginfo_t info;
};

void SignalContext::raiseContext(SignalItem *item, Process *process, Generation *generation) {
	helix::BorrowedDescriptor thread = generation->threadDescriptor;

	SignalHandler handler = _handlers[item->signalNumber];
	assert(!(handler.flags & signalOnce));

	if(handler.disposition == SignalDisposition::none) {
		if(item->signalNumber == SIGCHLD) { // TODO: Handle default actions generically.
			// Ignore the signal.
			return;
		}else{
			std::cout << "posix: Thread killed as the result of a signal" << std::endl;
			// TODO: Make sure that we are in the current generation?
			process->terminate(item->signalNumber);
			return;
		}
	}

	assert(handler.disposition == SignalDisposition::handle);

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
	HEL_CHECK(helStoreForeign(thread.getHandle(), frame,
			sizeof(SignalFrame), &sf));

	std::cout << "posix: Saving pre-signal stack to " << (void *)frame << std::endl;
	std::cout << "posix: Calling signal handler at " << (void *)handler.handlerIp << std::endl;

	// Setup the new register image and resume.
	// TODO: Linux sets rdx to the ucontext.
	sf.gprs[kHelRegRdi] = item->signalNumber;
	sf.gprs[kHelRegRsi] = frame + offsetof(SignalFrame, info);
	sf.gprs[kHelRegRax] = 0; // Number of variable arguments.

	sf.pcrs[kHelRegIp] = handler.handlerIp;
	sf.pcrs[kHelRegSp] = frame;

	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &sf.gprs));
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsProgram, &sf.pcrs));

	delete item;
}

void SignalContext::restoreContext(helix::BorrowedDescriptor thread) {
	uintptr_t pcrs[15];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
	auto frame = pcrs[kHelRegSp] - 8;

	std::cout << "posix: Restoring post-signal stack from " << (void *)frame << std::endl;

	SignalFrame sf;
	HEL_CHECK(helLoadForeign(thread.getHandle(), frame,
			sizeof(SignalFrame), &sf));

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
std::map<ProcessId, Process *> globalPidMap;

std::shared_ptr<Process> Process::findProcess(ProcessId pid) {
	auto it = globalPidMap.find(pid);
	if(it == globalPidMap.end())
		return nullptr;
	return it->second->shared_from_this();
}

Process::Process(Process *parent)
: _parent{parent}, _pid{0}, _clientPosixLane{kHelNullHandle}, _clientFileTable{nullptr},
		_notifyType{NotifyType::null} { }

Process::~Process() {
	std::cout << "\e[33mposix: Process is destructed\e[39m" << std::endl;
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
	auto process = std::make_shared<Process>(nullptr);
	process->_path = path;
	process->_vmContext = VmContext::create();
	process->_fsContext = FsContext::create();
	process->_fileContext = FileContext::create();
	process->_signalContext = SignalContext::create();

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &thread_memory));
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
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite | kHelMapDropAtFork,
			&process->_clientThreadPage));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientClkTrackerPage));

	assert(globalPidMap.find(1) == globalPidMap.end());
	process->_pid = 1;
	globalPidMap.insert({1, process.get()});

	// TODO: Do not pass an empty argument vector?
	auto thread_or_error = co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::vector<std::string>{}, std::vector<std::string>{},
			process->_vmContext,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	auto error = std::get_if<Error>(&thread_or_error);
	if(error)
		throw std::logic_error("Could not execute() init process");

	auto generation = std::make_shared<Generation>();
	generation->threadDescriptor = std::move(std::get<helix::UniqueDescriptor>(thread_or_error));
	generation->posixLane = std::move(server_lane);

	process->_currentGeneration = generation;
	serve(process, std::move(generation));

	co_return process;
}

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> original) {
	auto process = std::make_shared<Process>(original.get());
	process->_path = original->path();
	process->_vmContext = VmContext::clone(original->_vmContext);
	process->_fsContext = FsContext::clone(original->_fsContext);
	process->_fileContext = FileContext::clone(original->_fileContext);
	process->_signalContext = SignalContext::clone(original->_signalContext);

	HelHandle thread_memory;
	HEL_CHECK(helAllocateMemory(0x1000, 0, &thread_memory));
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
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite | kHelMapDropAtFork,
			&process->_clientThreadPage));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientFileTable));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientClkTrackerPage));

	ProcessId pid = nextPid++;
	assert(globalPidMap.find(pid) == globalPidMap.end());
	process->_pid = pid;
	original->_children.push_back(process);
	globalPidMap.insert({pid, process.get()});

	auto generation = std::make_shared<Generation>();
	HelHandle new_thread;
	HEL_CHECK(helCreateThread(process->fileContext()->getUniverse().getHandle(),
			process->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
			0, 0, kHelThreadStopped, &new_thread));
	generation->threadDescriptor = helix::UniqueDescriptor{new_thread};
	generation->posixLane = std::move(server_lane);

	process->_currentGeneration = generation;
	serve(process, std::move(generation));

	return process;
}

async::result<Error> Process::exec(std::shared_ptr<Process> process,
		std::string path, std::vector<std::string> args, std::vector<std::string> env) {
	auto exec_vm_context = VmContext::create();

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
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapProtWrite | kHelMapDropAtFork,
			&exec_thread_page));
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&exec_clk_tracker_page));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&exec_client_table));

	// TODO: We should only do this if the execute succeeds.
	process->_fileContext->closeOnExec();

	// Perform the exec() in a new VM context so that we
	// can catch errors before trashing the calling process.
	auto thread_or_error = co_await execute(process->_fsContext->getRoot(),
			process->_fsContext->getWorkingDirectory(),
			path, std::move(args), std::move(env), exec_vm_context,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	auto error = std::get_if<Error>(&thread_or_error);
	if(error && (*error == Error::noSuchFile || *error == Error::badExecutable)) {
		co_return *error;
	}else if(error)
		throw std::logic_error("Unexpected error from execute()");

	// "Commit" the exec() operation.
	process->_path = std::move(path);
	process->_vmContext = std::move(exec_vm_context);
	process->_signalContext->resetHandlers();
	process->_clientThreadPage = exec_thread_page;
	process->_clientPosixLane = exec_posix_lane;
	process->_clientFileTable = exec_client_table;
	process->_clientClkTrackerPage = exec_clk_tracker_page;

	// TODO: execute() should return a stopped thread that we can start here.
	auto generation = std::make_shared<Generation>();
	generation->threadDescriptor = std::move(std::get<helix::UniqueDescriptor>(thread_or_error));
	generation->posixLane = std::move(server_lane);

	auto previous = std::exchange(process->_currentGeneration, generation);
	HEL_CHECK(helKillThread(previous->threadDescriptor.getHandle()));
	serve(process, std::move(generation));

	co_return Error::success;
}

void Process::retire(Process *process) {
	assert(process->_parent);
	process->_parent->_childrenUsage.userTime += process->_generationUsage.userTime;
}

void Process::terminate(int signo) {
	auto parent = getParent();
	assert(parent);

	// Kill the current Generation and accumulate stats.
	HEL_CHECK(helKillThread(_currentGeneration->threadDescriptor.getHandle()));

	// TODO: Also do this before switching to a new Generation in execve().
	// TODO: Do the accumulation + _currentGeneration reset after the thread has really terminated?
	HelThreadStats stats;
	HEL_CHECK(helQueryThreadStats(_currentGeneration->threadDescriptor.getHandle(), &stats));
	_generationUsage.userTime += stats.userTime;

	_vmContext = nullptr;
	_fsContext = nullptr;
	_fileContext = nullptr;
	//_signalContext = nullptr; // TODO: Migrate the notifications to PID 1.
	_currentGeneration = nullptr;

	// Notify the parent of our status change.
	assert(_notifyType == NotifyType::null);
	_notifyType = NotifyType::terminated;
	_terminationSignal = signo;
	parent->_notifyQueue.push_back(*this);
	parent->_notifyBell.ring();

	// Send SIGCHLD to the parent.
	UserSignal info;
	info.pid = pid();
	parent->signalContext()->issueSignal(SIGCHLD, info);
}

async::result<int> Process::wait(int pid, bool non_blocking, int *signo) {
	assert(pid == -1 || pid > 0);

	int result = 0;
	int termination_signo = -1;
	while(true) {
		for(auto it = _notifyQueue.begin(); it != _notifyQueue.end(); ++it) {
			if(pid > 0 && pid != it->pid())
				continue;
			_notifyQueue.erase(it);
			result = it->pid();
			termination_signo = it->_terminationSignal;
			Process::retire(&(*it));
			break;
		}

		if(result > 0 || non_blocking) {
			*signo = termination_signo;
			co_return result;
		}
		co_await _notifyBell.async_wait();
	}
}

