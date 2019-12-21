
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <iomanip>
#include <iostream>

#include <async/jump.hpp>
#include <protocols/mbus/client.hpp>
#include <helix/timer.hpp>

#include "common.hpp"
#include "clock.hpp"
#include "device.hpp"
#include "drvcore.hpp"
#include "nl-socket.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "epoll.hpp"
#include "exec.hpp"
#include "extern_fs.hpp"
#include "devices/helout.hpp"
#include "fifo.hpp"
#include "inotify.hpp"
#include "procfs.hpp"
#include "pts.hpp"
#include "signalfd.hpp"
#include "subsystem/block.hpp"
#include "subsystem/drm.hpp"
#include "subsystem/input.hpp"
#include "subsystem/pci.hpp"
#include "sysfs.hpp"
#include "un-socket.hpp"
#include "timerfd.hpp"
#include "eventfd.hpp"
#include "tmp_fs.hpp"
#include <kerncfg.pb.h>
#include <posix.pb.h>

namespace {
	constexpr bool logRequests = false;
	constexpr bool logPaths = false;
	constexpr bool logSignals = false;
	constexpr bool logCleanup = false;
}

std::map<
	std::array<char, 16>,
	std::shared_ptr<Process>
> globalCredentialsMap;

std::shared_ptr<Process> findProcessWithCredentials(const char *credentials) {
	std::array<char, 16> creds;
	memcpy(creds.data(), credentials, 16);
	return globalCredentialsMap.at(creds);
}

void dumpRegisters(helix::BorrowedDescriptor thread) {
	uintptr_t pcrs[2];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));

	uintptr_t gprs[15];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, gprs));

	printf("rax: %.16lx, rbx: %.16lx, rcx: %.16lx\n", gprs[0], gprs[1], gprs[2]);
	printf("rdx: %.16lx, rdi: %.16lx, rsi: %.16lx\n", gprs[3], gprs[4], gprs[5]);
	printf(" r8: %.16lx,  r9: %.16lx, r10: %.16lx\n", gprs[6], gprs[7], gprs[8]);
	printf("r11: %.16lx, r12: %.16lx, r13: %.16lx\n", gprs[9], gprs[10], gprs[11]);
	printf("r14: %.16lx, r15: %.16lx, rbp: %.16lx\n", gprs[12], gprs[13], gprs[14]);
	printf("rip: %.16lx, rsp: %.16lx\n", pcrs[0], pcrs[1]);
}

async::detached observeThread(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	helix::BorrowedDescriptor thread = generation->threadDescriptor;

	uint64_t sequence = 1;
	while(true) {
		helix::Observe observe;
		auto &&submit = helix::submitObserve(thread, &observe,
				sequence, helix::Dispatcher::global());
		co_await submit.async_wait();

		if(observe.error() == kHelErrThreadTerminated) {
			if(logCleanup)
				std::cout << "\e[33mposix: Thread was killed\e[39m" << std::endl;
			generation->cancelServe.cancel();
			co_return;
		}

		HEL_CHECK(observe.error());
		sequence = observe.sequence();

		if(observe.observation() == kHelObserveSuperCall + 1) {
			struct ManagarmProcessData {
				HelHandle posixLane;
				void *threadPage;
				HelHandle *fileTable;
				void *clockTrackerPage;
			};

			ManagarmProcessData data = {
				self->clientPosixLane(),
				self->clientThreadPage(),
				static_cast<HelHandle *>(self->clientFileTable()),
				self->clientClkTrackerPage()
			};

			if(logRequests)
				std::cout << "posix: GET_PROCESS_DATA supercall" << std::endl;
			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helStoreForeign(thread.getHandle(), gprs[5],
					sizeof(ManagarmProcessData), &data));
			gprs[4] = kHelErrNone;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 2) {
			if(logRequests)
				std::cout << "posix: fork supercall" << std::endl;
			auto child = Process::fork(self);

			// Copy registers from the current thread to the new one.
			auto new_thread = child->currentGeneration()->threadDescriptor.getHandle();
			uintptr_t pcrs[2], gprs[15], thrs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsThread, &thrs));

			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsProgram, &pcrs));
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsThread, &thrs));

			// Setup post supercall registers in both threads and finally resume the threads.
			gprs[4] = kHelErrNone;
			gprs[5] = child->pid();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[5] = 0;
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsGeneral, &gprs));

			HEL_CHECK(helResume(thread.getHandle()));
			HEL_CHECK(helResume(new_thread));
		}else if(observe.observation() == kHelObserveSuperCall + 3) {
			if(logRequests)
				std::cout << "posix: execve supercall" << std::endl;
			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			std::string path;
			path.resize(gprs[3]);
			HEL_CHECK(helLoadForeign(self->vmContext()->getSpace().getHandle(),
					gprs[5], gprs[3], path.data()));

			std::string args_area;
			args_area.resize(gprs[6]);
			HEL_CHECK(helLoadForeign(self->vmContext()->getSpace().getHandle(),
					gprs[0], gprs[6], args_area.data()));

			std::string env_area;
			env_area.resize(gprs[8]);
			HEL_CHECK(helLoadForeign(self->vmContext()->getSpace().getHandle(),
					gprs[7], gprs[8], env_area.data()));

			if(logRequests || logPaths)
				std::cout << "posix: execve path: " << path << std::endl;

			// Parse both the arguments and the environment areas.
			size_t k;

			std::vector<std::string> args;
			k = 0;
			while(k < args_area.size()) {
				auto d = args_area.find(char(0), k);
				assert(d != std::string::npos);
				args.push_back(args_area.substr(k, d - k));
//				std::cout << "arg: " << args.back() << std::endl;
				k = d + 1;
			}

			std::vector<std::string> env;
			k = 0;
			while(k < env_area.size()) {
				auto d = env_area.find(char(0), k);
				assert(d != std::string::npos);
				env.push_back(env_area.substr(k, d - k));
//				std::cout << "env: " << env.back() << std::endl;
				k = d + 1;
			}

			auto error = co_await Process::exec(self,
					path, std::move(args), std::move(env));
			if(error == Error::noSuchFile) {
				gprs[4] = kHelErrNone;
				gprs[5] = ENOENT;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

				HEL_CHECK(helResume(thread.getHandle()));
			}else if(error == Error::badExecutable) {
				gprs[4] = kHelErrNone;
				gprs[5] = ENOEXEC;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

				HEL_CHECK(helResume(thread.getHandle()));
			}else
				assert(error == Error::success);
		}else if(observe.observation() == kHelObserveSuperCall + 4) {
			if(logRequests)
				std::cout << "posix: EXIT supercall" << std::endl;

			self->terminate();
		}else if(observe.observation() == kHelObserveSuperCall + 7) {
			if(logRequests)
				std::cout << "posix: SIG_MASK supercall" << std::endl;

			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto mode = gprs[kHelRegRsi];
			auto mask = gprs[kHelRegRdx];

			uint64_t former = self->signalMask();
			if(mode == SIG_SETMASK) {
				self->setSignalMask(mask);
			}else if(mode == SIG_BLOCK) {
				self->setSignalMask(former | mask);
			}else if(mode == SIG_UNBLOCK) {
				self->setSignalMask(former & ~mask);
			}else{
				assert(!mode);
			}

			gprs[kHelRegRdi] = 0;
			gprs[kHelRegRsi] = former;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 8) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_RAISE supercall" << std::endl;

			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegRdi] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"in SIG_RAISE supercall" "\e[39m" << std::endl;
			auto active = self->signalContext()->fetchSignal(~self->signalMask());
			if(active)
				self->signalContext()->raiseContext(active, self.get(), generation.get());
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 6) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_RESTORE supercall" << std::endl;

			self->signalContext()->restoreContext(thread);
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 5) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_KILL supercall" << std::endl;

			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto pid = gprs[kHelRegRsi];
			auto sn = gprs[kHelRegRdx];

			std::shared_ptr<Process> target;
			if(!pid) {
				std::cout << "\e[31mposix: SIG_KILL(0) should target "
						"the whole process group\e[39m" << std::endl;
				if(logSignals)
					std::cout << "posix: SIG_KILL on PID " << self->pid() << std::endl;
				target = self;
			}else{
				if(logSignals)
					std::cout << "posix: SIG_KILL on PID " << pid << std::endl;
				target = Process::findProcess(pid);
				assert(target);
			}

			// Clear the error code.
			// TODO: This should only happen is raising succeeds. Move it somewhere else?
			gprs[kHelRegRdi] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			UserSignal info;
			info.pid = self->pid();
			info.uid = 0;
			target->signalContext()->issueSignal(sn, info);

			// If the process signalled itself, we should process the signal before resuming.
			if(self->checkOrRequestSignalRaise()) {
				auto active = self->signalContext()->fetchSignal(~self->signalMask());
				if(active)
					self->signalContext()->raiseContext(active, self.get(), generation.get());
			}
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveInterrupt) {
			//printf("posix: Process %s was interrupted\n", self->path().c_str());
			if(self->checkOrRequestSignalRaise()) {
				auto active = self->signalContext()->fetchSignal(~self->signalMask());
				if(active)
					self->signalContext()->raiseContext(active, self.get(), generation.get());
			}
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObservePanic) {
			printf("\e[35mposix: User space panic in process %s\n", self->path().c_str());
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGABRT;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous user space panic" "\e[39m" << std::endl;
			self->signalContext()->raiseContext(item, self.get(), generation.get());
		}else if(observe.observation() == kHelObserveBreakpoint) {
			printf("\e[35mposix: Breakpoint in process %s\n", self->path().c_str());
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);
		}else if(observe.observation() == kHelObservePageFault) {
			printf("\e[31mposix: Page fault in process %s\n", self->path().c_str());
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGSEGV;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGSEGV" "\e[39m" << std::endl;
			self->signalContext()->raiseContext(item, self.get(), generation.get());
		}else if(observe.observation() == kHelObserveGeneralFault) {
			printf("\e[31mposix: General fault in process %s\n", self->path().c_str());
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGSEGV;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGSEGV" "\e[39m" << std::endl;
			self->signalContext()->raiseContext(item, self.get(), generation.get());
		}else if(observe.observation() == kHelObserveIllegalInstruction) {
			printf("\e[31mposix: Illegal instruction in process %s\n", self->path().c_str());
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGILL;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGILL" "\e[39m" << std::endl;
			self->signalContext()->raiseContext(item, self.get(), generation.get());
		}else{
			throw std::runtime_error("Unexpected observation");
		}
	}
}

async::detached serveSignals(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	helix::BorrowedDescriptor thread = generation->threadDescriptor;
	async::cancellation_token cancellation = generation->cancelServe;

	uint64_t sequence = 1;
	while(true) {
		if(cancellation.is_cancellation_requested())
			break;
		//std::cout << "Waiting for raise in " << self->pid() << std::endl;
		auto result = co_await self->signalContext()->pollSignal(sequence,
				UINT64_C(-1), cancellation);
		sequence = std::get<0>(result);
		//std::cout << "Calling helInterruptThread on " << self->pid() << std::endl;
		HEL_CHECK(helInterruptThread(thread.getHandle()));
	}

	if(logCleanup)
		std::cout << "\e[33mposix: Exiting serveSignals()\e[39m" << std::endl;
}

async::result<void> serveRequests(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	async::cancellation_token cancellation = generation->cancelServe;

	async::cancellation_callback cancel_callback{cancellation, [&] {
		HEL_CHECK(helShutdownLane(generation->posixLane.getHandle()));
	}};

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(generation->posixLane, helix::Dispatcher::global(),
				helix::action(&accept));
		co_await header.async_wait();

		if(accept.error() == kHelErrLaneShutdown)
			break;
		HEL_CHECK(accept.error());
		auto conversation = accept.descriptor();

		auto &&initiate = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_req));
		co_await initiate.async_wait();
		HEL_CHECK(recv_req.error());

		managarm::posix::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.request_type() == managarm::posix::CntReqType::GET_PID) {
			if(logRequests)
				std::cout << "posix: GET_PID" << std::endl;

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->pid());

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::WAIT) {
			if(logRequests)
				std::cout << "posix: WAIT" << std::endl;

			assert(!(req.flags() & ~WNOHANG));

			int signo;
			auto pid = co_await self->wait(req.pid(), req.flags() & WNOHANG, &signo);

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(pid);

			uint32_t mode = 0x200; // 0x200 means exited.
			if(signo >= 0)
				mode |= 0x400 | (signo << 24);
			resp.set_mode(mode);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::GET_RESOURCE_USAGE) {
			if(logRequests)
				std::cout << "posix: GET_RESOURCE_USAGE" << std::endl;

			HelThreadStats stats;
			HEL_CHECK(helQueryThreadStats(generation->threadDescriptor.getHandle(), &stats));

			uint64_t user_time;
			if(req.mode() == RUSAGE_SELF) {
				user_time = stats.userTime;
			}else if(req.mode() == RUSAGE_CHILDREN) {
				user_time = self->accumulatedUsage().userTime;
			}else{
				std::cout << "\e[31mposix: GET_RESOURCE_USAGE mode is not supported\e[39m"
						<< std::endl;
				// TODO: Return an error response.
			}

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_ru_user_time(stats.userTime);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::VM_MAP) {
			if(logRequests)
				std::cout << "posix: VM_MAP size: " << (void *)(size_t)req.size() << std::endl;
			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			// TODO: Validate req.flags().

			if(req.mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			uint32_t native_flags = 0;
			if((req.flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_PRIVATE) {
				native_flags |= kHelMapCopyOnWrite;
			}else if((req.flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_SHARED) {
				native_flags |= kHelMapShareAtFork;
			}else{
				throw std::runtime_error("posix: Handle illegal flags in VM_MAP");
			}

			if(req.mode() & PROT_READ)
				native_flags |= kHelMapProtRead;
			if(req.mode() & PROT_WRITE)
				native_flags |= kHelMapProtWrite;
			if(req.mode() & PROT_EXEC)
				native_flags |= kHelMapProtExecute;

			void *address;
			if(req.flags() & MAP_ANONYMOUS) {
				assert(req.fd() == -1);
				assert(!req.rel_offset());

				HelHandle handle;
				HEL_CHECK(helAllocateMemory(req.size(), 0, &handle));

				address = co_await self->vmContext()->mapFile(
						helix::UniqueDescriptor{handle}, nullptr,
						0, req.size(), native_flags);
			}else{
				auto file = self->fileContext()->getFile(req.fd());
				assert(file && "Illegal FD for VM_MAP");
				auto memory = co_await file->accessMemory(req.rel_offset());
				address = co_await self->vmContext()->mapFile(std::move(memory), std::move(file),
						req.rel_offset(), req.size(), native_flags);
			}

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_offset(reinterpret_cast<uintptr_t>(address));
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::VM_REMAP) {
			if(logRequests)
				std::cout << "posix: VM_REMAP" << std::endl;

			helix::SendBuffer send_resp;

			auto address = co_await self->vmContext()->remapFile(
					reinterpret_cast<void *>(req.address()), req.size(), req.new_size());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_offset(reinterpret_cast<uintptr_t>(address));

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::VM_PROTECT) {
			if(logRequests)
				std::cout << "posix: VM_PROTECT" << std::endl;
			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			if(req.mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			uint32_t native_flags = 0;
			if(req.mode() & PROT_READ)
				native_flags |= kHelMapProtRead;
			if(req.mode() & PROT_WRITE)
				native_flags |= kHelMapProtWrite;
			if(req.mode() & PROT_EXEC)
				native_flags |= kHelMapProtExecute;

			co_await self->vmContext()->protectFile(
					reinterpret_cast<void *>(req.address()), req.size(), native_flags);

			resp.set_error(managarm::posix::Errors::SUCCESS);
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::VM_UNMAP) {
			if(logRequests)
				std::cout << "posix: VM_UNMAP address: " << (void *)req.address()
						<< ", size: " << (void *)(size_t)req.size() << std::endl;

			helix::SendBuffer send_resp;

			self->vmContext()->unmapFile(reinterpret_cast<void *>(req.address()), req.size());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::MOUNT) {
			if(logRequests)
				std::cout << "posix: MOUNT " << req.fs_type() << " on " << req.path()
						<< " to " << req.target_path() << std::endl;

			helix::SendBuffer send_resp;

			auto target = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.target_path());
			if(!target.second) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			if(req.fs_type() == "procfs") {
				target.first->mount(target.second, getProcfs());
			}else if(req.fs_type() == "sysfs") {
				target.first->mount(target.second, getSysfs());
			}else if(req.fs_type() == "devtmpfs") {
				target.first->mount(target.second, getDevtmpfs());
			}else if(req.fs_type() == "tmpfs") {
				target.first->mount(target.second, tmp_fs::createRoot());
			}else if(req.fs_type() == "devpts") {
				target.first->mount(target.second, pts::getFsRoot());
			}else{
				assert(req.fs_type() == "ext2");
				auto source = co_await resolve(self->fsContext()->getRoot(),
						self->fsContext()->getWorkingDirectory(), req.path());
				assert(source.second);
				assert(source.second->getTarget()->getType() == VfsType::blockDevice);
				auto device = blockRegistry.get(source.second->getTarget()->readDevice());
				auto link = co_await device->mount();
				target.first->mount(target.second, std::move(link));
			}

			if(logRequests)
				std::cout << "posix:     MOUNT succeeds" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::CHROOT) {
			if(logRequests)
				std::cout << "posix: CHROOT" << std::endl;

			helix::SendBuffer send_resp;

			auto path = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(path.second) {
				self->fsContext()->changeRoot(path);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::CHDIR) {
			if(logRequests)
				std::cout << "posix: CHDIR" << std::endl;

			helix::SendBuffer send_resp;

			auto path = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(path.second) {
				self->fsContext()->changeWorkingDirectory(path);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::FCHDIR) {
			if(logRequests)
				std::cout << "posix: CHDIR" << std::endl;

			managarm::posix::SvrResponse resp;
			helix::SendBuffer send_resp;

			auto file = self->fileContext()->getFile(req.fd());

			if(!file) {
				resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			self->fsContext()->changeWorkingDirectory({file->associatedMount(),
					file->associatedLink()});

			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::ACCESS) {
			if(logRequests || logPaths)
				std::cout << "posix: ACCESS " << req.path() << std::endl;

			helix::SendBuffer send_resp;

			auto path = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(path.second) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::MKDIR) {
			if(logRequests || logPaths)
				std::cout << "posix: MKDIR " << req.path() << std::endl;

			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			co_await resolver.resolve(resolvePrefix);
			assert(resolver.currentLink());

			auto parent = resolver.currentLink()->getTarget();
			if(co_await parent->getLink(resolver.nextComponent())) {
				resp.set_error(managarm::posix::Errors::ALREADY_EXISTS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto result = co_await parent->mkdir(resolver.nextComponent());
			if(auto error = std::get_if<Error>(&result); error) {
				assert(*error == Error::illegalOperationTarget);

				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::SYMLINK) {
			if(logRequests || logPaths)
				std::cout << "posix: SYMLINK " << req.path() << std::endl;

			helix::SendBuffer send_resp;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			co_await resolver.resolve(resolvePrefix);
			assert(resolver.currentLink());

			auto parent = resolver.currentLink()->getTarget();
			co_await parent->symlink(resolver.nextComponent(), req.target_path());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::RENAME) {
			if(logRequests || logPaths)
				std::cout << "posix: RENAME " << req.path()
						<< " to " << req.target_path() << std::endl;

			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			co_await resolver.resolve();
			if(!resolver.currentLink()) {
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			PathResolver new_resolver;
			new_resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.target_path());
			co_await new_resolver.resolve(resolvePrefix);
			assert(new_resolver.currentLink());

			auto superblock = resolver.currentLink()->getTarget()->superblock();
			auto directory = new_resolver.currentLink()->getTarget();
			assert(superblock == directory->superblock());
			co_await superblock->rename(resolver.currentLink().get(),
					directory.get(), new_resolver.nextComponent());

			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::STAT
				|| req.request_type() == managarm::posix::CntReqType::LSTAT) {
			if(logRequests || logPaths)
				std::cout << "posix: STAT path: " << req.path() << std::endl;

			helix::SendBuffer send_resp;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(req.request_type() == managarm::posix::STAT) {
				co_await resolver.resolve();
			}else{
				assert(req.request_type() == managarm::posix::LSTAT);
				co_await resolver.resolve(resolveDontFollow);
			}

			if(resolver.currentLink()) {
				auto stats = co_await resolver.currentLink()->getTarget()->getStats();

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				DeviceId devnum;
				switch(resolver.currentLink()->getTarget()->getType()) {
				case VfsType::regular:
					resp.set_file_type(managarm::posix::FT_REGULAR);
					break;
				case VfsType::directory:
					resp.set_file_type(managarm::posix::FT_DIRECTORY);
					break;
				case VfsType::charDevice:
					resp.set_file_type(managarm::posix::FT_CHAR_DEVICE);
					devnum = resolver.currentLink()->getTarget()->readDevice();
					resp.set_ref_devnum(makedev(devnum.first, devnum.second));
					break;
				case VfsType::blockDevice:
					resp.set_file_type(managarm::posix::FT_BLOCK_DEVICE);
					devnum = resolver.currentLink()->getTarget()->readDevice();
					resp.set_ref_devnum(makedev(devnum.first, devnum.second));
					break;
				default:
					break;
				}

				resp.set_fs_inode(stats.inodeNumber);
				resp.set_mode(stats.mode);
				resp.set_num_links(stats.numLinks);
				resp.set_uid(stats.uid);
				resp.set_gid(stats.gid);
				resp.set_file_size(stats.fileSize);
				resp.set_atime_secs(stats.atimeSecs);
				resp.set_atime_nanos(stats.atimeNanos);
				resp.set_mtime_secs(stats.mtimeSecs);
				resp.set_mtime_nanos(stats.mtimeNanos);
				resp.set_ctime_secs(stats.ctimeSecs);
				resp.set_ctime_nanos(stats.ctimeNanos);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::READLINK) {
			if(logRequests || logPaths)
				std::cout << "posix: READLINK path: " << req.path() << std::endl;

			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			auto path = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path(), resolveDontFollow);
			if(!path.second) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&send_data, nullptr, 0));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto result = co_await path.second->getTarget()->readSymlink(path.second.get());
			if(auto error = std::get_if<Error>(&result); error) {
				assert(*error == Error::illegalOperationTarget);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&send_data, nullptr, 0));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				auto &target = std::get<std::string>(result);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&send_data, target.data(), target.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::OPEN) {
			if(logRequests || logPaths)
				std::cout << "posix: OPEN path: " << req.path()	<< std::endl;

			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			assert(!(req.flags() & ~(managarm::posix::OF_CREATE
					| managarm::posix::OF_EXCLUSIVE
					| managarm::posix::OF_NONBLOCK
					| managarm::posix::OF_CLOEXEC)));

			SemanticFlags semantic_flags = 0;
			if(req.flags() & managarm::posix::OF_NONBLOCK)
				semantic_flags |= semanticNonBlock;

			smarter::shared_ptr<File, FileHandle> file;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(req.flags() & managarm::posix::OF_CREATE) {
				co_await resolver.resolve(resolvePrefix);
				if(!resolver.currentLink()) {
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

					auto ser = resp.SerializeAsString();
					auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
							helix::action(&send_resp, ser.data(), ser.size()));
					co_await transmit.async_wait();
					HEL_CHECK(send_resp.error());
					continue;
				}

				if(logRequests)
					std::cout << "posix: Creating file " << req.path() << std::endl;

				auto directory = resolver.currentLink()->getTarget();
				auto tail = co_await directory->getLink(resolver.nextComponent());
				if(tail) {
					if(req.flags() & managarm::posix::OF_EXCLUSIVE) {
						resp.set_error(managarm::posix::Errors::ALREADY_EXISTS);

						auto ser = resp.SerializeAsString();
						auto &&transmit = helix::submitAsync(conversation,
								helix::Dispatcher::global(),
								helix::action(&send_resp, ser.data(), ser.size()));
						co_await transmit.async_wait();
						HEL_CHECK(send_resp.error());
						continue;
					}else{
						file = co_await tail->getTarget()->open(
								resolver.currentView(), std::move(tail),
								semantic_flags);
						assert(file);
					}
				}else{
					assert(directory->superblock());
					auto node = co_await directory->superblock()->createRegular();
					// Due to races, link() can fail here.
					// TODO: Implement a version of link() that eithers links the new node
					// or returns the current node without failing.
					auto link = co_await directory->link(resolver.nextComponent(), node);
					file = co_await node->open(resolver.currentView(), std::move(link),
							semantic_flags);
					assert(file);
				}
			}else{
				co_await resolver.resolve();

				if(resolver.currentLink()) {
					auto target = resolver.currentLink()->getTarget();
					file = co_await target->open(resolver.currentView(), resolver.currentLink(),
							semantic_flags);
				}
			}

			if(file) {
				int fd = self->fileContext()->attachFile(file,
						req.flags() & managarm::posix::OF_CLOEXEC);

				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				if(logRequests)
					std::cout << "posix:     OPEN failed: file not found" << std::endl;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::CLOSE) {
			if(logRequests)
				std::cout << "posix: CLOSE file descriptor " << req.fd() << std::endl;

			helix::SendBuffer send_resp;

			self->fileContext()->closeFile(req.fd());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP) {
			if(logRequests)
				std::cout << "posix: DUP" << std::endl;

			auto file = self->fileContext()->getFile(req.fd());
			assert(file && "Illegal FD for DUP");

			assert(!(req.flags() & ~(managarm::posix::OF_CLOEXEC)));

			int newfd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OF_CLOEXEC);

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(newfd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP2) {
			if(logRequests)
				std::cout << "posix: DUP2" << std::endl;

			auto file = self->fileContext()->getFile(req.fd());
			assert(file && "Illegal FD for DUP2");

			assert(!req.flags());

			if(req.newfd() >= 0) {
				self->fileContext()->attachFile(req.newfd(), file);
			}else{
				throw std::runtime_error("DUP2 requires a file descriptor >= 0");
			}

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::FSTAT) {
			if(logRequests)
				std::cout << "posix: FSTAT" << std::endl;

			auto file = self->fileContext()->getFile(req.fd());
			assert(file && "Illegal FD for FSTAT");
			auto stats = co_await file->associatedLink()->getTarget()->getStats();

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			DeviceId devnum;
			switch(file->associatedLink()->getTarget()->getType()) {
			case VfsType::regular:
				resp.set_file_type(managarm::posix::FT_REGULAR);
				break;
			case VfsType::directory:
				resp.set_file_type(managarm::posix::FT_DIRECTORY);
				break;
			case VfsType::charDevice:
				resp.set_file_type(managarm::posix::FT_CHAR_DEVICE);
				devnum = file->associatedLink()->getTarget()->readDevice();
				resp.set_ref_devnum(makedev(devnum.first, devnum.second));
				break;
			case VfsType::blockDevice:
				resp.set_file_type(managarm::posix::FT_BLOCK_DEVICE);
				devnum = file->associatedLink()->getTarget()->readDevice();
				resp.set_ref_devnum(makedev(devnum.first, devnum.second));
				break;
			default:
				break;
			}

			resp.set_fs_inode(stats.inodeNumber);
			resp.set_mode(stats.mode);
			resp.set_num_links(stats.numLinks);
			resp.set_uid(stats.uid);
			resp.set_gid(stats.gid);
			resp.set_file_size(stats.fileSize);
			resp.set_atime_secs(stats.atimeSecs);
			resp.set_atime_nanos(stats.atimeNanos);
			resp.set_mtime_secs(stats.mtimeSecs);
			resp.set_mtime_nanos(stats.mtimeNanos);
			resp.set_ctime_secs(stats.ctimeSecs);
			resp.set_ctime_nanos(stats.ctimeNanos);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::IS_TTY) {
			if(logRequests)
				std::cout << "posix: IS_TTY" << std::endl;

			auto file = self->fileContext()->getFile(req.fd());
			assert(file && "Illegal FD for IS_TTY");

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_mode(file->isTerminal());

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TTY_NAME) {
			if(logRequests)
				std::cout << "posix: TTY_NAME" << std::endl;

			helix::SendBuffer send_resp;

			std::cout << "\e[31mposix: Fix TTY_NAME\e[39m" << std::endl;
			managarm::posix::SvrResponse resp;
			resp.set_path("/dev/ttyS0");
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::GETCWD) {
			if(logRequests)
				std::cout << "posix: GETCWD" << std::endl;

			auto dir = self->fsContext()->getWorkingDirectory();

			std::string path = "/";
			while(true) {
				if(dir == self->fsContext()->getRoot())
					break;

				// If we are at the origin of a mount point, traverse that mount point.
				ViewPath traversed;
				if(dir.second == dir.first->getOrigin()) {
					if(!dir.first->getParent())
						break;
					auto anchor = dir.first->getAnchor();
					assert(anchor); // Non-root mounts must have anchors in their parents.
					traversed = ViewPath{dir.first->getParent(), dir.second};
				}else{
					traversed = dir;
				}

				auto owner = traversed.second->getOwner();
				assert(owner); // Otherwise, we would have been at the root.
				path = "/" + traversed.second->getName() + path;

				dir = ViewPath{traversed.first, owner->treeLink()};
			}

			helix::SendBuffer send_resp;
			helix::SendBuffer send_path;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_size(path.size());

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_path, path.data(),
							std::min(static_cast<size_t>(req.size()), path.size() + 1)));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_path.error());
		}else if(req.request_type() == managarm::posix::CntReqType::UNLINK) {
			if(logRequests || logPaths)
				std::cout << "posix: UNLINK path: " << req.path() << std::endl;

			helix::SendBuffer send_resp;

			auto path = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(path.second) {
				auto owner = path.second->getOwner();
				co_await owner->unlink(path.second->getName());

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::FD_GET_FLAGS) {
			if(logRequests)
				std::cout << "posix: FD_GET_FLAGS" << std::endl;

			helix::SendBuffer send_resp;

			auto descriptor = self->fileContext()->getDescriptor(req.fd());
			if(!descriptor) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			int flags = 0;
			if(descriptor->closeOnExec)
				flags |= O_CLOEXEC;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_flags(flags);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SIG_ACTION) {
			if(logRequests)
				std::cout << "posix: SIG_ACTION" << std::endl;

			if(req.flags() & ~(SA_SIGINFO | SA_RESETHAND | SA_NODEFER | SA_RESTART)) {
				std::cout << "\e[31mposix: Unknown SIG_ACTION flags: 0x"
						<< std::hex << req.flags()
						<< std::dec << "\e[39m" << std::endl;
				assert(!"Flags not implemented");
			}

			SignalHandler saved_handler;
			if(req.mode()) {
				SignalHandler handler;
				if(req.sig_handler() == uintptr_t(-2)) {
					handler.disposition = SignalDisposition::none;
				}else if(req.sig_handler() == uintptr_t(-3)) {
					handler.disposition = SignalDisposition::ignore;
				}else{
					handler.disposition = SignalDisposition::handle;
					handler.handlerIp = req.sig_handler();
				}

				handler.flags = 0;
				handler.mask = req.sig_mask();
				handler.restorerIp = req.sig_restorer();

				if(req.flags() & SA_SIGINFO)
					handler.flags |= signalInfo;
				if(req.flags() & SA_RESETHAND)
					handler.flags |= signalOnce;
				if(req.flags() & SA_NODEFER)
					handler.flags |= signalReentrant;
				if(req.flags() & SA_RESTART)
					std::cout << "\e[31mposix: Ignoring SA_RESTART\e[39m" << std::endl;

				saved_handler = self->signalContext()->changeHandler(req.sig_number(), handler);
			}else{
				saved_handler = self->signalContext()->getHandler(req.sig_number());
			}

			int saved_flags = 0;
			if(saved_handler.flags & signalInfo)
				saved_flags |= SA_SIGINFO;
			if(saved_handler.flags & signalOnce)
				saved_flags |= SA_RESETHAND;
			if(saved_handler.flags & signalReentrant)
				saved_flags |= SA_NODEFER;

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_flags(saved_flags);
			resp.set_sig_mask(saved_handler.mask);
			if(saved_handler.disposition == SignalDisposition::handle) {
				resp.set_sig_handler(saved_handler.handlerIp);
				resp.set_sig_restorer(saved_handler.restorerIp);
			}else if(saved_handler.disposition == SignalDisposition::none) {
				resp.set_sig_handler(-2);
			}else{
				assert(saved_handler.disposition == SignalDisposition::ignore);
				resp.set_sig_handler(-3);
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::PIPE_CREATE) {
			if(logRequests)
				std::cout << "posix: PIPE_CREATE" << std::endl;

			assert(!(req.flags() & ~(O_CLOEXEC | O_NONBLOCK)));

			if(req.flags() & O_NONBLOCK)
				std::cout << "\e[31mposix: pipe2(O_NONBLOCK)"
						" is not implemented correctly\e[39m" << std::endl;

			helix::SendBuffer send_resp;

			auto pair = fifo::createPair();
			auto r_fd = self->fileContext()->attachFile(std::get<0>(pair),
					req.flags() & O_CLOEXEC);
			auto w_fd = self->fileContext()->attachFile(std::get<1>(pair),
					req.flags() & O_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.mutable_fds()->Add(r_fd);
			resp.mutable_fds()->Add(w_fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SOCKET) {
			if(logRequests)
				std::cout << "posix: SOCKET" << std::endl;

			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));

			if(req.flags() & SOCK_NONBLOCK)
				std::cout << "\e[31mposix: socket(SOCK_NONBLOCK)"
						" is not implemented correctly\e[39m" << std::endl;

			smarter::shared_ptr<File, FileHandle> file;
			if(req.domain() == AF_UNIX) {
				assert(req.socktype() == SOCK_DGRAM || req.socktype() == SOCK_STREAM
						|| req.socktype() == SOCK_SEQPACKET);
				assert(!req.protocol());

				file = un_socket::createSocketFile();
			}else if(req.domain() == AF_NETLINK) {
				assert(req.socktype() == SOCK_RAW || req.socktype() == SOCK_DGRAM);
				file = nl_socket::createSocketFile(req.protocol());
			}else{
				throw std::runtime_error("posix: Handle unknown protocol families");
			}

			auto fd = self->fileContext()->attachFile(file,
					req.flags() & SOCK_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SOCKPAIR) {
			if(logRequests)
				std::cout << "posix: SOCKPAIR" << std::endl;

			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));

			if(req.flags() & SOCK_NONBLOCK)
				std::cout << "\e[31mposix: socketpair(SOCK_NONBLOCK)"
						" is not implemented correctly\e[39m" << std::endl;

			assert(req.domain() == AF_UNIX);
			assert(req.socktype() == SOCK_DGRAM || req.socktype() == SOCK_STREAM
					|| req.socktype() == SOCK_SEQPACKET);
			assert(!req.protocol());

			auto pair = un_socket::createSocketPair(self.get());
			auto fd0 = self->fileContext()->attachFile(std::get<0>(pair),
					req.flags() & SOCK_CLOEXEC);
			auto fd1 = self->fileContext()->attachFile(std::get<1>(pair),
					req.flags() & SOCK_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.mutable_fds()->Add(fd0);
			resp.mutable_fds()->Add(fd1);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::ACCEPT) {
			if(logRequests)
				std::cout << "posix: ACCEPT" << std::endl;

			helix::SendBuffer send_resp;

			auto sockfile = self->fileContext()->getFile(req.fd());
			assert(sockfile && "Illegal FD for ACCEPT");

			auto newfile = co_await sockfile->accept(self.get());
			auto fd = self->fileContext()->attachFile(std::move(newfile));

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SENDMSG) {
			if(logRequests)
				std::cout << "posix: SENDMSG" << std::endl;

			std::vector<uint8_t> buffer;
			buffer.resize(req.size());

			helix::RecvBuffer recv_data;
			helix::RecvInline recv_addr;
			helix::SendBuffer send_resp;

			auto &&submit_data = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_data, buffer.data(), buffer.size(), kHelItemChain),
					helix::action(&recv_addr));
			co_await submit_data.async_wait();
			HEL_CHECK(recv_data.error());

			auto sockfile = self->fileContext()->getFile(req.fd());
			assert(sockfile && "Illegal FD for SENDMSG");

			MsgFlags flags = 0;
			if(req.flags() & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_NOSIGNAL)) {
				std::cout << "\e[31mposix: Unknown SENDMSG flags: 0x" << std::hex << req.flags()
						<< std::dec << "\e[39m" << std::endl;
				assert(!"Flags not implemented");
			}
			if(req.flags() & MSG_DONTWAIT)
				flags |= msgNoWait;
			if(req.flags() & MSG_CMSG_CLOEXEC)
				flags |= msgCloseOnExec;
			if(req.flags() & MSG_NOSIGNAL) {
				static bool warned = false;
				if(!warned)
					std::cout << "\e[35mposix: Ignoring MSG_NOSIGNAL\e[39m" << std::endl;
				warned = true;
			}

			std::vector<smarter::shared_ptr<File, FileHandle>> files;
			for(int i = 0; i < req.fds_size(); i++) {
				auto file = self->fileContext()->getFile(req.fds(i));
				assert(sockfile && "Illegal FD for SENDMSG cmsg");
				files.push_back(std::move(file));
			}

			auto result_or_error = co_await sockfile->sendMsg(self.get(), flags,
					buffer.data(), recv_data.actualLength(),
					recv_addr.data(), recv_addr.length(),
					std::move(files));

			managarm::posix::SvrResponse resp;

			auto error = std::get_if<Error>(&result_or_error);
			if(error && *error == Error::brokenPipe) {
				resp.set_error(managarm::posix::Errors::BROKEN_PIPE);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_size(std::get<size_t>(result_or_error));

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::RECVMSG) {
			if(logRequests)
				std::cout << "posix: RECVMSG" << std::endl;

			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;
			helix::SendBuffer send_addr;
			helix::SendBuffer send_ctrl;

			auto sockfile = self->fileContext()->getFile(req.fd());
			assert(sockfile && "Illegal FD for SENDMSG");

			MsgFlags flags = 0;
			if(req.flags() & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) {
				std::cout << "\e[31mposix: Unknown RECVMSG flags: 0x" << std::hex << req.flags()
						<< std::dec << "\e[39m" << std::endl;
				assert(!"Flags not implemented");
			}
			if(req.flags() & MSG_DONTWAIT)
				flags |= msgNoWait;
			if(req.flags() & MSG_CMSG_CLOEXEC)
				flags |= msgCloseOnExec;

			std::vector<char> buffer;
			std::vector<char> address;
			buffer.resize(req.size());
			address.resize(req.addr_size());
			auto result_or_error = co_await sockfile->recvMsg(self.get(), flags,
					buffer.data(), req.size(),
					address.data(), req.addr_size(), req.ctrl_size());

			managarm::posix::SvrResponse resp;

			auto error = std::get_if<Error>(&result_or_error);
			if(error && *error == Error::wouldBlock) {
				resp.set_error(managarm::posix::Errors::WOULD_BLOCK);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto result = std::get<RecvResult>(result_or_error);
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_addr, address.data(), std::get<1>(result), kHelItemChain),
					helix::action(&send_data, buffer.data(), std::get<0>(result), kHelItemChain),
					helix::action(&send_ctrl, std::get<2>(result).data(),
							std::get<2>(result).size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CALL) {
			if(logRequests)
				std::cout << "posix: EPOLL_CALL" << std::endl;

			helix::SendBuffer send_resp;

			auto epfile = epoll::createFile();
			assert(req.fds_size() == req.events_size());
			for(int i = 0; i < req.fds_size(); i++) {
				auto file = self->fileContext()->getFile(req.fds(i));
				assert(file && "Illegal FD for EPOLL_ADD item");
				auto locked = file->weakFile().lock();
				assert(locked);
				epoll::addItem(epfile.get(), self.get(), std::move(locked),
						req.events(i), i);
			}

			struct epoll_event events[16];
			size_t k;
			if(req.timeout() == -1) {
				k = co_await epoll::wait(epfile.get(), events, 16);
			}else if(!req.timeout()) {
				// Do not bother to set up a timer for zero timeouts.
				async::cancellation_event cancel_wait;
				cancel_wait.cancel();
				k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
			}else if(req.timeout() > 0) {
				async::cancellation_event cancel_wait;
				helix::TimeoutCancellation timer{static_cast<uint64_t>(req.timeout()), cancel_wait};
				k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
				co_await timer.retire();
			}else{
				assert(!"posix: Illegal timeout for EPOLL_CALL");
				__builtin_unreachable();
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			for(int i = 0; i < req.fds_size(); i++)
				resp.add_events(0);
			for(size_t m = 0; m < k; m++)
				resp.set_events(events[m].data.u32, events[m].events);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CREATE) {
			if(logRequests)
				std::cout << "posix: EPOLL_CREATE" << std::endl;

			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(managarm::posix::OF_CLOEXEC)));

			auto file = epoll::createFile();
			auto fd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_ADD) {
			if(logRequests)
				std::cout << "posix: EPOLL_ADD" << std::endl;

			helix::SendBuffer send_resp;

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			assert(epfile && "Illegal FD for EPOLL_ADD");
			assert(file && "Illegal FD for EPOLL_ADD item");

			auto locked = file->weakFile().lock();
			assert(locked);
			epoll::addItem(epfile.get(), self.get(), std::move(locked),
					req.flags(), req.cookie());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_MODIFY) {
			if(logRequests)
				std::cout << "posix: EPOLL_MODIFY" << std::endl;

			helix::SendBuffer send_resp;

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			assert(epfile && "Illegal FD for EPOLL_MODIFY");
			assert(file && "Illegal FD for EPOLL_MODIFY item");

			epoll::modifyItem(epfile.get(), file.get(), req.flags(), req.cookie());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_DELETE) {
			if(logRequests)
				std::cout << "posix: EPOLL_DELETE" << std::endl;

			helix::SendBuffer send_resp;

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			assert(epfile && "Illegal FD for EPOLL_DELETE");
			assert(file && "Illegal FD for EPOLL_DELETE item");

			epoll::deleteItem(epfile.get(), file.get(), req.flags());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_WAIT) {
			if(logRequests)
				std::cout << "posix: EPOLL_WAIT request" << std::endl;

			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			auto epfile = self->fileContext()->getFile(req.fd());
			assert(epfile && "Illegal FD for EPOLL_WAIT");

			struct epoll_event events[16];
			size_t k;
			if(req.timeout() == -1) {
				k = co_await epoll::wait(epfile.get(), events,
						std::min(req.size(), uint32_t(16)));
			}else if(!req.timeout()) {
				// Do not bother to set up a timer for zero timeouts.
				async::cancellation_event cancel_wait;
				cancel_wait.cancel();
				k = co_await epoll::wait(epfile.get(), events,
						std::min(req.size(), uint32_t(16)), cancel_wait);
			}else if(req.timeout() > 0) {
				async::cancellation_event cancel_wait;
				helix::TimeoutCancellation timer{static_cast<uint64_t>(req.timeout()), cancel_wait};
				k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
				co_await timer.retire();
			}else{
				assert(!"posix: Illegal timeout for EPOLL_WAIT");
				__builtin_unreachable();
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, events, k * sizeof(struct epoll_event)));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TIMERFD_CREATE) {
			if(logRequests)
				std::cout << "posix: TIMERFD_CREATE" << std::endl;

			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(TFD_CLOEXEC | TFD_NONBLOCK)));

			auto file = timerfd::createFile(req.flags() & TFD_NONBLOCK);
			auto fd = self->fileContext()->attachFile(file, req.flags() & TFD_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TIMERFD_SETTIME) {
			if(logRequests)
				std::cout << "posix: TIMERFD_SETTIME" << std::endl;

			helix::SendBuffer send_resp;

			auto file = self->fileContext()->getFile(req.fd());
			assert(file && "Illegal FD for TIMERFD_SETTIME");
			timerfd::setTime(file.get(),
					{static_cast<time_t>(req.time_secs()), static_cast<long>(req.time_nanos())},
					{static_cast<time_t>(req.interval_secs()), static_cast<long>(req.interval_nanos())});

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SIGNALFD_CREATE) {
			if(logRequests)
				std::cout << "posix: SIGNALFD_CREATE" << std::endl;

			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(managarm::posix::OF_CLOEXEC)));

			auto file = createSignalFile(req.sigset());
			auto fd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::INOTIFY_CREATE) {
			if(logRequests)
				std::cout << "posix: INOTIFY_CREATE" << std::endl;

			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(managarm::posix::OF_CLOEXEC)));

			auto file = inotify::createFile();
			auto fd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::INOTIFY_ADD) {
			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			if(logRequests || logPaths)
				std::cout << "posix: INOTIFY_ADD" << req.path() << std::endl;

			auto ifile = self->fileContext()->getFile(req.fd());
			assert(ifile);

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			co_await resolver.resolve();
			if(!resolver.currentLink()) {
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto wd = inotify::addWatch(ifile.get(), resolver.currentLink()->getTarget());

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_wd(wd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EVENTFD_CREATE) {
			if(logRequests)
				std::cout << "posix: EVENTFD_CREATE" << std::endl;

			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			if (req.flags() & ~(managarm::posix::OF_CLOEXEC | managarm::posix::OF_NONBLOCK)) {
				std::cout << "posix: invalid flag specified (EFD_SEMAPHORE?)" << std::endl;
				std::cout << "posix: flags specified: " << req.flags() << std::endl;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {

				auto file = eventfd::createFile(req.initval(), req.flags() & managarm::posix::OF_NONBLOCK);
				auto fd = self->fileContext()->attachFile(file,
						req.flags() & managarm::posix::OF_CLOEXEC);

				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd);
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else{
			std::cout << "posix: Illegal request" << std::endl;
			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}

	if(logCleanup)
		std::cout << "\e[33mposix: Existing serveRequests()\e[39m" << std::endl;
}

void serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation) {
	helix::BorrowedDescriptor thread = generation->threadDescriptor;

	std::array<char, 16> creds;
	HEL_CHECK(helGetCredentials(thread.getHandle(), 0, creds.data()));
	auto res = globalCredentialsMap.insert({creds, self});
	assert(res.second);

	observeThread(self, generation);
	serveSignals(self, generation);
	async::detach(serveRequests(self, generation));
}

// --------------------------------------------------------

namespace {
	async::jump foundKerncfg;
	helix::UniqueLane kerncfgLane;
};

struct CmdlineNode final : public procfs::RegularNode {
	async::result<std::string> show() override {
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::RecvInline recv_cmdline;

		managarm::kerncfg::CntRequest req;
		req.set_req_type(managarm::kerncfg::CntReqType::GET_CMDLINE);

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(kerncfgLane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&recv_cmdline));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_cmdline.error());

		managarm::kerncfg::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::kerncfg::Error::SUCCESS);
		co_return std::string{(const char *)recv_cmdline.data(), recv_cmdline.length()} + '\n';
	}

	async::result<void> store(std::string buffer) override {
		throw std::runtime_error("Cannot store to /proc/cmdline");
	}
};

async::result<void> enumerateKerncfg() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "kerncfg")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		std::cout << "POSIX: Found kerncfg" << std::endl;

		kerncfgLane = helix::UniqueLane(co_await entity.bind());
		foundKerncfg.trigger();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundKerncfg.async_wait();

	auto procfs_root = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	procfs_root->directMkregular("cmdline", std::make_shared<CmdlineNode>());
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

async::detached runInit() {
	co_await enumerateKerncfg();
	co_await clk::enumerateTracker();
	co_await populateRootView();
	co_await Process::init("sbin/posix-init");
}

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;

//	HEL_CHECK(helSetPriority(kHelThisThread, 1));

	{
		async::queue_scope scope{helix::globalQueue()};

		drvcore::initialize();

		charRegistry.install(createHeloutDevice());
		charRegistry.install(pts::createMasterDevice());
		block_subsystem::run();
		drm_subsystem::run();
		input_subsystem::run();
		pci_subsystem::run();

		runInit();
	}

	helix::globalQueue()->run();
}

