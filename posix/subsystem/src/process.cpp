
#include <signal.h>
#include <string.h>
#include <sys/auxv.h>

#include <cofiber.hpp>
#include "common.hpp"
#include "clock.hpp"
#include "exec.hpp"
#include "process.hpp"

static bool logFileAttach = false;

cofiber::no_future serve(std::shared_ptr<Process> self, helix::UniqueDescriptor p);

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
	context->_areaTree = original->_areaTree; // Copy construction is sufficient here.

	return context;
}

COFIBER_ROUTINE(async::result<void *>,
VmContext::mapFile(smarter::shared_ptr<File, FileHandle> file,
		intptr_t offset, size_t size, uint32_t native_flags), ([=] {	
	size_t aligned_size = (size + 0xFFF) & ~size_t(0xFFF);

	auto memory = COFIBER_AWAIT file->accessMemory(offset);

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
	area.file = std::move(file);
	area.offset = offset;
	_areaTree.insert({address, std::move(area)});

	COFIBER_RETURN(pointer);
}))

COFIBER_ROUTINE(async::result<void *>, VmContext::remapFile(void *old_pointer,
		size_t old_size, size_t new_size), ([=] {
	size_t aligned_old_size = (old_size + 0xFFF) & ~size_t(0xFFF);
	size_t aligned_new_size = (new_size + 0xFFF) & ~size_t(0xFFF);

//	std::cout << "posix: Remapping " << old_pointer << std::endl;
	auto it = _areaTree.find(reinterpret_cast<uintptr_t>(old_pointer));
	assert(it != _areaTree.end());
	assert(it->second.areaSize == aligned_old_size);
	
	auto memory = COFIBER_AWAIT it->second.file->accessMemory(it->second.offset);

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

	COFIBER_RETURN(pointer);
}))

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

	return context;
}

std::shared_ptr<FsContext> FsContext::clone(std::shared_ptr<FsContext> original) {
	auto context = std::make_shared<FsContext>();

	context->_root = original->_root;

	return context;
}

ViewPath FsContext::getRoot() {
	return _root;
}

void FsContext::changeRoot(ViewPath root) {
	_root = std::move(root);
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

FileDescriptor FileContext::getDescriptor(int fd) {
	auto file = _fileTable.find(fd);
	assert(file != _fileTable.end());
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
	assert(it != _fileTable.end());

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

std::shared_ptr<SignalContext> SignalContext::create() {
	return std::make_shared<SignalContext>();
}

std::shared_ptr<SignalContext> SignalContext::clone(std::shared_ptr<SignalContext> original) {
	std::cout << "\e[31mposix: SignalContext is not cloned correctly\e[39m" << std::endl;
	return std::make_shared<SignalContext>();
}

void SignalContext::setSignalHandler(int number, uintptr_t handler, uintptr_t restorer) {
	assert(number < 64);
	_slots[number].handler = handler;
	_slots[number].restorer = restorer;
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

void SignalContext::restoreSignal(helix::BorrowedDescriptor thread) {
	uintptr_t pcrs[15];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
	auto frame = pcrs[kHelRegSp] - 8;

	std::cout << "posix: Restoring post-signal stack from " << (void *)frame << std::endl;

	SignalFrame sf;
	HEL_CHECK(helLoadForeign(thread.getHandle(), frame,
			sizeof(SignalFrame), &sf));
	
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &sf.gprs));
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsProgram, &sf.pcrs));
	HEL_CHECK(helResume(thread.getHandle()));
}

void SignalContext::raiseSynchronousSignal(int number, SignalInfo info,
		helix::BorrowedDescriptor thread) {
	assert(number < 64);

	SignalFrame sf;
	memset(&sf, 0, sizeof(SignalFrame));
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &sf.gprs));
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &sf.pcrs));

	sf.returnAddress = _slots[number].restorer;

	// TODO: Only do this if SA_SIGINFO is set.
	sf.info.si_signo = number;
	std::visit(CompileSignalInfo{&sf.info}, info);

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

	// Setup the new register image and resume.
	// TODO: Linux sets rdx to the ucontext.
	sf.gprs[kHelRegRdi] = number;
	sf.gprs[kHelRegRsi] = frame + offsetof(SignalFrame, info);
	sf.gprs[kHelRegRax] = 0; // Number of variable arguments.

	sf.pcrs[kHelRegIp] = _slots[number].handler;
	sf.pcrs[kHelRegSp] = frame;
	
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &sf.gprs));
	HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsProgram, &sf.pcrs));
	HEL_CHECK(helResume(thread.getHandle()));
}

// ----------------------------------------------------------------------------
// Process.
// ----------------------------------------------------------------------------

// PID 1 is reserved for the init process, therefore we start at 2.
int nextPid = 2;
std::map<int, Process *> globalPidMap;

Process::Process()
: _pid{0}, _clientFileTable{nullptr} { }

COFIBER_ROUTINE(async::result<std::shared_ptr<Process>>, Process::init(std::string path),
		([=] {
	auto process = std::make_shared<Process>();
	process->_path = path;
	process->_vmContext = VmContext::create();
	process->_fsContext = FsContext::create();
	process->_fileContext = FileContext::create();
	process->_signalContext = SignalContext::create();

	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientClkTrackerPage));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientFileTable));
	
	assert(globalPidMap.find(1) == globalPidMap.end());
	process->_pid = 1;
	globalPidMap.insert({1, process.get()});

	// TODO: Do not pass an empty argument vector?
	auto thread = COFIBER_AWAIT execute(process->_fsContext->getRoot(), path,
			std::vector<std::string>{}, std::vector<std::string>{},
			process->_vmContext,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	serve(process, std::move(thread));

	COFIBER_RETURN(process);
}))

std::shared_ptr<Process> Process::fork(std::shared_ptr<Process> original) {
	auto process = std::make_shared<Process>();
	process->_vmContext = VmContext::clone(original->_vmContext);
	process->_fsContext = FsContext::clone(original->_fsContext);
	process->_fileContext = FileContext::clone(original->_fileContext);
	process->_signalContext = SignalContext::clone(original->_signalContext);

	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientClkTrackerPage));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			process->_vmContext->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&process->_clientFileTable));
	
	int pid = nextPid++;
	assert(globalPidMap.find(pid) == globalPidMap.end());
	process->_pid = pid;
	globalPidMap.insert({pid, process.get()});

	return process;
}

COFIBER_ROUTINE(async::result<void>, Process::exec(std::shared_ptr<Process> process,
		std::string path, std::vector<std::string> args, std::vector<std::string> env), ([=] {
	auto exec_vm_context = VmContext::create();

	void *exec_clk_tracker;
	void *exec_client_table;
	HEL_CHECK(helMapMemory(clk::trackerPageMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&exec_clk_tracker));
	HEL_CHECK(helMapMemory(process->_fileContext->fileTableMemory().getHandle(),
			exec_vm_context->getSpace().getHandle(),
			nullptr, 0, 0x1000, kHelMapProtRead | kHelMapDropAtFork,
			&exec_client_table));
	
	// TODO: We should only do this if the execute succeeds.
	process->_fileContext->closeOnExec();

	// Perform the exec() in a new VM context so that we
	// can catch errors before trashing the calling process.
	auto thread = COFIBER_AWAIT execute(process->_fsContext->getRoot(),
			path, std::move(args), std::move(env), exec_vm_context,
			process->_fileContext->getUniverse(),
			process->_fileContext->clientMbusLane());
	serve(process, std::move(thread));

	// "Commit" the exec() operation.
	process->_path = std::move(path);
	process->_vmContext = std::move(exec_vm_context);
	process->_clientClkTrackerPage = exec_clk_tracker;
	process->_clientFileTable = exec_client_table;

	// TODO: execute() should return a stopped thread that we can start here.

	COFIBER_RETURN();
}))

