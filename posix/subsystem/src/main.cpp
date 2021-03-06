
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
#include <sys/stat.h>
#include <iomanip>
#include <iostream>

#include <async/algorithm.hpp>
#include <async/jump.hpp>
#include <protocols/mbus/client.hpp>
#include <helix/timer.hpp>

#include "net.hpp"
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
#include "extern_socket.hpp"
#include "devices/full.hpp"
#include "devices/helout.hpp"
#include "devices/null.hpp"
#include "devices/zero.hpp"
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
#include <bragi/helpers-std.hpp>
#include <posix.bragi.hpp>
#include <frg/std_compat.hpp>

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

void dumpRegisters(std::shared_ptr<Process> proc) {
	auto thread = proc->threadDescriptor();

	uintptr_t pcrs[2];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, pcrs));

	uintptr_t gprs[kHelNumGprs];
	HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, gprs));

	auto ip = pcrs[0];
	auto sp = pcrs[1];

#if defined(__x86_64__)
	printf("rax: %.16lx, rbx: %.16lx, rcx: %.16lx\n", gprs[0], gprs[1], gprs[2]);
	printf("rdx: %.16lx, rdi: %.16lx, rsi: %.16lx\n", gprs[3], gprs[4], gprs[5]);
	printf(" r8: %.16lx,  r9: %.16lx, r10: %.16lx\n", gprs[6], gprs[7], gprs[8]);
	printf("r11: %.16lx, r12: %.16lx, r13: %.16lx\n", gprs[9], gprs[10], gprs[11]);
	printf("r14: %.16lx, r15: %.16lx, rbp: %.16lx\n", gprs[12], gprs[13], gprs[14]);
	printf("rip: %.16lx, rsp: %.16lx\n", pcrs[0], pcrs[1]);
#elif defined(__aarch64__)
	// Registers X0-X30 have indices 0-30
	for (int i = 0; i < 31; i += 3) {
		if (i != 30) {
			printf("x%02d: %.16lx, x%02d: %.16lx, x%02d: %.16lx\n", i, gprs[i], i + 1, gprs[i + 1], i + 2, gprs[i + 2]);
		} else {
			printf("x%d: %.16lx,  ip: %.16lx,  sp: %.16lx\n", i, gprs[i], pcrs[kHelRegIp], pcrs[kHelRegSp]);
		}
	}
#endif

	printf("Mappings:\n");
	for (auto mapping : *(proc->vmContext())) {
		uintptr_t start = mapping.baseAddress();
		uintptr_t end = start + mapping.size();

		std::string path;
		if(mapping.backingFile().get()) {
			// TODO: store the ViewPath inside the mapping.
			ViewPath vp{proc->fsContext()->getRoot().first,
					mapping.backingFile()->associatedLink()};

			// TODO: This code is copied from GETCWD, factor it out into a function.
			path = "";
			while(true) {
				if(vp == proc->fsContext()->getRoot())
					break;

				// If we are at the origin of a mount point, traverse that mount point.
				ViewPath traversed;
				if(vp.second == vp.first->getOrigin()) {
					if(!vp.first->getParent())
						break;
					auto anchor = vp.first->getAnchor();
					assert(anchor); // Non-root mounts must have anchors in their parents.
					traversed = ViewPath{vp.first->getParent(), vp.second};
				}else{
					traversed = vp;
				}

				auto owner = traversed.second->getOwner();
				if(!owner) { // We did not reach the root.
					// TODO: Can we get rid of this case?
					path = "?" + path;
					break;
				}

				path = "/" + traversed.second->getName() + path;
				vp = ViewPath{traversed.first, owner->treeLink()};
			}
		}else{
			path = "anon";
		}

		printf("%016lx - %016lx %s %s%s%s %s + 0x%lx\n", start, end,
				mapping.isPrivate() ? "P" : "S",
				mapping.isExecutable() ? "x" : "-",
				mapping.isReadable() ? "r" : "-",
				mapping.isWritable() ? "w" : "-",
				path.c_str(),
				mapping.backingFileOffset());
		if(ip >= start && ip < end)
			printf("               ^ IP is 0x%lx bytes into this mapping\n", ip - start);
		if(sp >= start && sp < end)
			printf("               ^ Stack is 0x%lx bytes into this mapping\n", sp - start);
	}
}

async::result<void> observeThread(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	auto thread = self->threadDescriptor();

	uint64_t sequence = 1;
	while(true) {
		if(generation->inTermination)
			break;

		helix::Observe observe;
		auto &&submit = helix::submitObserve(thread, &observe,
				sequence, helix::Dispatcher::global());
		co_await submit.async_wait();

		// Usually, we should terminate via the generation->inTermination check above.
		if(observe.error() == kHelErrThreadTerminated) {
			std::cout << "\e[31m" "posix: Thread terminated unexpectedly" "\e[39m" << std::endl;
			co_return;
		}

		HEL_CHECK(observe.error());
		sequence = observe.sequence();

		if(observe.observation() == kHelObserveSuperCall + 10) {
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			size_t size = gprs[kHelRegArg0];

			// TODO: this is a waste of memory. Use some always-zero memory instead.
			HelHandle handle;
			HEL_CHECK(helAllocateMemory(size, 0, nullptr, &handle));

			void *address = co_await self->vmContext()->mapFile(0,
					helix::UniqueDescriptor{handle}, nullptr,
					0, size, true, kHelMapProtRead | kHelMapProtWrite);

			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = reinterpret_cast<uintptr_t>(address);
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 11) {
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			self->vmContext()->unmapFile(reinterpret_cast<void *>(gprs[kHelRegArg0]), gprs[kHelRegArg1]);

			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 1) {
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
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto storeData = co_await helix_ng::writeMemory(thread, gprs[kHelRegArg0],
					sizeof(ManagarmProcessData), &data);
			HEL_CHECK(storeData.error());
			gprs[kHelRegError] = kHelErrNone;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 2) {
			if(logRequests)
				std::cout << "posix: fork supercall" << std::endl;
			auto child = Process::fork(self);

			// Copy registers from the current thread to the new one.
			auto new_thread = child->threadDescriptor().getHandle();
			uintptr_t pcrs[2], gprs[kHelNumGprs], thrs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsThread, &thrs));

			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsProgram, &pcrs));
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsThread, &thrs));

			// Setup post supercall registers in both threads and finally resume the threads.
			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = child->pid();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegOut0] = 0;
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsGeneral, &gprs));

			HEL_CHECK(helResume(thread.getHandle()));
			HEL_CHECK(helResume(new_thread));
		}else if(observe.observation() == kHelObserveSuperCall + 9) {
			if(logRequests)
				std::cout << "posix: clone supercall" << std::endl;
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			void *ip = reinterpret_cast<void *>(gprs[kHelRegArg0]);
			void *sp = reinterpret_cast<void *>(gprs[kHelRegArg1]);

			auto child = Process::clone(self, ip, sp);

			auto new_thread = child->threadDescriptor().getHandle();

			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = child->pid();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			HEL_CHECK(helResume(thread.getHandle()));
			HEL_CHECK(helResume(new_thread));
		}else if(observe.observation() == kHelObserveSuperCall + 3) {
			if(logRequests)
				std::cout << "posix: execve supercall" << std::endl;
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			std::string path;
			path.resize(gprs[kHelRegArg1]);
			auto loadPath = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
					gprs[kHelRegArg0], gprs[kHelRegArg1], path.data());
			HEL_CHECK(loadPath.error());

			std::string args_area;
			args_area.resize(gprs[kHelRegArg3]);
			auto loadArgs = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
					gprs[kHelRegArg2], gprs[kHelRegArg3], args_area.data());
			HEL_CHECK(loadArgs.error());

			std::string env_area;
			env_area.resize(gprs[kHelRegArg5]);
			auto loadEnv = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
					gprs[kHelRegArg4], gprs[kHelRegArg5], env_area.data());
			HEL_CHECK(loadEnv.error());

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
				gprs[kHelRegError] = kHelErrNone;
				gprs[kHelRegOut0] = ENOENT;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

				HEL_CHECK(helResume(thread.getHandle()));
			}else if(error == Error::badExecutable) {
				gprs[kHelRegError] = kHelErrNone;
				gprs[kHelRegOut0] = ENOEXEC;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

				HEL_CHECK(helResume(thread.getHandle()));
			}else
				assert(error == Error::success);
		}else if(observe.observation() == kHelObserveSuperCall + 4) {
			if(logRequests)
				std::cout << "posix: EXIT supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto code = gprs[kHelRegArg0];

			co_await self->terminate(TerminationByExit{static_cast<int>(code & 0xFF)});
		}else if(observe.observation() == kHelObserveSuperCall + 7) {
			if(logRequests)
				std::cout << "posix: SIG_MASK supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto mode = gprs[kHelRegArg0];
			auto mask = gprs[kHelRegArg1];

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

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = former;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 8) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_RAISE supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegError] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"in SIG_RAISE supercall" "\e[39m" << std::endl;
			bool killed = false;
			auto active = self->signalContext()->fetchSignal(~self->signalMask());
			if(active)
				co_await self->signalContext()->raiseContext(active, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 6) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_RESTORE supercall" << std::endl;

			co_await self->signalContext()->restoreContext(thread);
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 5) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_KILL supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto pid = gprs[kHelRegArg0];
			auto sn = gprs[kHelRegArg1];

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
			gprs[kHelRegError] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			UserSignal info;
			info.pid = self->pid();
			info.uid = 0;
			target->signalContext()->issueSignal(sn, info);

			// If the process signalled itself, we should process the signal before resuming.
			bool killed = false;
			if(self->checkOrRequestSignalRaise()) {
				auto active = self->signalContext()->fetchSignal(~self->signalMask());
				if(active)
					co_await self->signalContext()->raiseContext(active, self.get(), killed);
			}
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveInterrupt) {
			//printf("posix: Process %s was interrupted\n", self->path().c_str());
			bool killed = false;
			if(self->checkOrRequestSignalRaise()) {
				auto active = self->signalContext()->fetchSignal(~self->signalMask());
				if(active)
					co_await self->signalContext()->raiseContext(active, self.get(), killed);
			}
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObservePanic) {
			printf("\e[35mposix: User space panic in process %s\n", self->path().c_str());
			dumpRegisters(self);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGABRT;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous user space panic" "\e[39m" << std::endl;
			bool killed;
			co_await self->signalContext()->raiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveBreakpoint) {
			printf("\e[35mposix: Breakpoint in process %s\n", self->path().c_str());
			dumpRegisters(self);
			printf("\e[39m");
			fflush(stdout);
		}else if(observe.observation() == kHelObservePageFault) {
			printf("\e[31mposix: Page fault in process %s\n", self->path().c_str());
			dumpRegisters(self);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGSEGV;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGSEGV" "\e[39m" << std::endl;
			bool killed;
			co_await self->signalContext()->raiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveGeneralFault) {
			printf("\e[31mposix: General fault in process %s\n", self->path().c_str());
			dumpRegisters(self);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGSEGV;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGSEGV" "\e[39m" << std::endl;
			bool killed;
			co_await self->signalContext()->raiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveIllegalInstruction) {
			printf("\e[31mposix: Illegal instruction in process %s\n", self->path().c_str());
			dumpRegisters(self);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGILL;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGILL" "\e[39m" << std::endl;
			bool killed;
			co_await self->signalContext()->raiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else{
			printf("\e[31mposix: Unexpected observation in process %s\n", self->path().c_str());
			dumpRegisters(self);
			printf("\e[39m");
			fflush(stdout);

			auto item = new SignalItem;
			item->signalNumber = SIGILL;
			if(!self->checkSignalRaise())
				std::cout << "\e[33m" "posix: Ignoring global signal flag "
						"during synchronous SIGILL" "\e[39m" << std::endl;
			bool killed;
			co_await self->signalContext()->raiseContext(item, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}
	}
}

async::result<void> serveSignals(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	auto thread = self->threadDescriptor();
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
	generation->signalsDone.raise();
}

async::result<void> serveRequests(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	async::cancellation_token cancellation = generation->cancelServe;

	async::cancellation_callback cancel_callback{cancellation, [&] {
		HEL_CHECK(helShutdownLane(self->posixLane().getHandle()));
	}};

	while(true) {
		auto [accept, recv_head] = co_await helix_ng::exchangeMsgs(
				self->posixLane(),
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		if(accept.error() == kHelErrLaneShutdown)
			break;
		HEL_CHECK(accept.error());

		if(recv_head.error() == kHelErrBufferTooSmall) {
			std::cout << "posix: Rejecting request due to RecvInline overflow" << std::endl;
			continue;
		}
		HEL_CHECK(recv_head.error());

		auto conversation = accept.descriptor();

		auto sendErrorResponse = [&conversation]<typename Message = managarm::posix::SvrResponse>(managarm::posix::Errors err) -> async::result<void> {
			Message resp;
			resp.set_error(err);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		};

		auto preamble = bragi::read_preamble(recv_head);
		assert(!preamble.error());

		managarm::posix::CntRequest req;
		if (preamble.id() == managarm::posix::CntRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::posix::CntRequest>(recv_head);
			if (!o) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			req = *o;
		}

		if(preamble.id() == bragi::message_id<managarm::posix::GetTidRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::GetTidRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			if(logRequests)
				std::cout << "posix: GET_TID" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->tid());

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::GET_PID) {
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
		}else if(preamble.id() == managarm::posix::GetPpidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetPpidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_PPID" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->getParent()->pid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::GetUidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetUidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_UID" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->uid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::SetUidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetUidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SET_UID" << std::endl;

			Error err = self->setUid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(preamble.id() == managarm::posix::GetEuidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetEuidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_EUID" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->euid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::SetEuidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetEuidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SET_EUID" << std::endl;

			Error err = self->setEuid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(preamble.id() == managarm::posix::GetGidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetGidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_GID" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->gid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::GetEgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetEgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_EGID" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->egid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::SetGidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetGidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SET_GID" << std::endl;

			Error err = self->setGid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(preamble.id() == managarm::posix::SetEgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetEgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SET_EGID" << std::endl;

			Error err = self->setEgid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(req.request_type() == managarm::posix::CntReqType::WAIT) {
			if(logRequests)
				std::cout << "posix: WAIT" << std::endl;

			assert(!(req.flags() & ~WNOHANG));

			TerminationState state;
			auto pid = co_await self->wait(req.pid(), req.flags() & WNOHANG, &state);

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(pid);

			uint32_t mode = 0;
			if(auto byExit = std::get_if<TerminationByExit>(&state); byExit) {
				mode |= 0x200 | byExit->code; // 0x200 = normal exit().
			}else if(auto bySignal = std::get_if<TerminationBySignal>(&state); bySignal) {
				mode |= 0x400 | (bySignal->signo << 24); // 0x400 = killed by signal.
			}else{
				assert(std::holds_alternative<std::monostate>(state));
			}
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
			HEL_CHECK(helQueryThreadStats(self->threadDescriptor().getHandle(), &stats));

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
		}else if(preamble.id() == bragi::message_id<managarm::posix::VmMapRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::VmMapRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			if(logRequests)
				std::cout << "posix: VM_MAP size: " << (void *)(size_t)req->size() << std::endl;

			// TODO: Validate req->flags().

			if(req->mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			uint32_t nativeFlags = 0;

			if(req->mode() & PROT_READ)
				nativeFlags |= kHelMapProtRead;
			if(req->mode() & PROT_WRITE)
				nativeFlags |= kHelMapProtWrite;
			if(req->mode() & PROT_EXEC)
				nativeFlags |= kHelMapProtExecute;

			bool copyOnWrite;
			if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_PRIVATE) {
				copyOnWrite = true;
			}else if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_SHARED) {
				copyOnWrite = false;
			}else{
				throw std::runtime_error("posix: Handle illegal flags in VM_MAP");
			}

			uintptr_t hint = 0;
			if(req->flags() & MAP_FIXED)
				hint = req->address_hint();

			void *address;
			if(req->flags() & MAP_ANONYMOUS) {
				assert(req->fd() == -1);
				assert(!req->rel_offset());

				// TODO: this is a waste of memory. Use some always-zero memory instead.
				HelHandle handle;
				HEL_CHECK(helAllocateMemory(req->size(), 0, nullptr, &handle));

				address = co_await self->vmContext()->mapFile(hint,
						helix::UniqueDescriptor{handle}, nullptr,
						0, req->size(), copyOnWrite, nativeFlags);
			}else{
				auto file = self->fileContext()->getFile(req->fd());
				assert(file && "Illegal FD for VM_MAP");
				auto memory = co_await file->accessMemory();
				address = co_await self->vmContext()->mapFile(hint,
						std::move(memory), std::move(file),
						req->rel_offset(), req->size(), copyOnWrite, nativeFlags);
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_offset(reinterpret_cast<uintptr_t>(address));

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
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
		}else if(preamble.id() == managarm::posix::MountRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::MountRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: MOUNT " << req->fs_type() << " on " << req->path()
						<< " to " << req->target_path() << std::endl;

			auto resolveResult = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req->target_path());
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto target = resolveResult.value();

			if(req->fs_type() == "procfs") {
				co_await target.first->mount(target.second, getProcfs());
			}else if(req->fs_type() == "sysfs") {
				co_await target.first->mount(target.second, getSysfs());
			}else if(req->fs_type() == "devtmpfs") {
				co_await target.first->mount(target.second, getDevtmpfs());
			}else if(req->fs_type() == "tmpfs") {
				co_await target.first->mount(target.second, tmp_fs::createRoot());
			}else if(req->fs_type() == "devpts") {
				co_await target.first->mount(target.second, pts::getFsRoot());
			}else{
				assert(req->fs_type() == "ext2");
				auto sourceResult = co_await resolve(self->fsContext()->getRoot(),
						self->fsContext()->getWorkingDirectory(), req->path());
				if(!sourceResult) {
					if(sourceResult.error() == protocols::fs::Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else if(sourceResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(sourceResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}
				auto source = sourceResult.value();
				assert(source.second);
				assert(source.second->getTarget()->getType() == VfsType::blockDevice);
				auto device = blockRegistry.get(source.second->getTarget()->readDevice());
				auto link = co_await device->mount();
				co_await target.first->mount(target.second, std::move(link));
			}

			if(logRequests)
				std::cout << "posix:     MOUNT succeeds" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);

			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::CHROOT) {
			if(logRequests)
				std::cout << "posix: CHROOT" << std::endl;

			helix::SendBuffer send_resp;

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto path = pathResult.value();
			self->fsContext()->changeRoot(path);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::CHDIR) {
			if(logRequests)
				std::cout << "posix: CHDIR" << std::endl;

			helix::SendBuffer send_resp;

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto path = pathResult.value();
			self->fsContext()->changeWorkingDirectory(path);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
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
		}else if(req.request_type() == managarm::posix::CntReqType::ACCESSAT) {
			if(logRequests || logPaths)
				std::cout << "posix: ACCESSAT " << req.path() << std::endl;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(req.flags()) {
				if(req.flags() & AT_SYMLINK_NOFOLLOW) {
					std::cout << "posix: ACCESSAT flag handling AT_SYMLINK_NOFOLLOW is unimplemented" << std::endl;
				} else if(req.flags() & AT_EACCESS) {
					std::cout << "posix: ACCESSAT flag handling AT_EACCESS is unimplemented" << std::endl;
				} else {
					std::cout << "posix: ACCESSAT unknown flag is unimplemented: " << req.flags() << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
			}

			if(req.fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req.fd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					relative_to, req.path());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto path = pathResult.value();

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::MKDIRAT) {
			if(logRequests || logPaths)
				std::cout << "posix: MKDIRAT " << req.path() << std::endl;

			helix::SendBuffer send_resp;
			managarm::posix::SvrResponse resp;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if (!req.path().size()) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			if(req.fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req.fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req.path());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto parent = resolver.currentLink()->getTarget();
			auto existsResult = co_await parent->getLink(resolver.nextComponent());
			assert(existsResult);
			auto exists = existsResult.value();
			if(exists) {
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
		}else if(preamble.id() == managarm::posix::MkfifoAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::MkfifoAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: MKFIFOAT " << req->fd() << " " << req->path() << std::endl;

			if (!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if (req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto parent = resolver.currentLink()->getTarget();
			if(co_await parent->getLink(resolver.nextComponent())) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			auto result = co_await parent->mkfifo(resolver.nextComponent(), req->mode());
			if(!result) {
				std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
				co_return;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::LinkAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::LinkAtRequest>(recv_head, tail);

			if(logRequests)
				std::cout << "posix: LINKAT" << std::endl;

			if(req->flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->flags() & AT_EMPTY_PATH) {
				std::cout << "posix: AT_EMPTY_PATH is unimplemented for linkat" << std::endl;
			}

			if(req->flags() & AT_SYMLINK_FOLLOW) {
				std::cout << "posix: AT_SYMLINK_FOLLOW is unimplemented for linkat" << std::endl;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path());
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			if (req->newfd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->newfd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver new_resolver;
			new_resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->target_path());
			auto new_resolveResult = co_await new_resolver.resolve(resolvePrefix);
			if(!new_resolveResult) {
				if(new_resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto target = resolver.currentLink()->getTarget();
			auto directory = new_resolver.currentLink()->getTarget();
			assert(target->superblock() == directory->superblock()); // Hard links across mount points are not allowed, return EXDEV
			auto result = co_await directory->link(new_resolver.nextComponent(), target);
			if(!result) {
				std::cout << "posix: Unexpected failure from link()" << std::endl;
				co_return;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::SymlinkAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::SymlinkAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: SYMLINK " << req->path() << std::endl;

			ViewPath relativeTo;
			smarter::shared_ptr<File, FileHandle> file;

			if (!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->fd() == AT_FDCWD) {
				relativeTo = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());
				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relativeTo = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relativeTo, req->path());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto parent = resolver.currentLink()->getTarget();
			auto result = co_await parent->symlink(resolver.nextComponent(), req->target_path());
			if(auto error = std::get_if<Error>(&result); error) {
				assert(*error == Error::illegalOperationTarget);
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
		}else if(preamble.id() == managarm::posix::RenameAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::RenameAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: RENAMEAT " << req->path()
						<< " to " << req->target_path() << std::endl;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if (req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path());
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			if (req->newfd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->newfd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver new_resolver;
			new_resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->target_path());
			auto new_resolveResult = co_await new_resolver.resolve(resolvePrefix);
			if(!new_resolveResult) {
				if(new_resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto superblock = resolver.currentLink()->getTarget()->superblock();
			auto directory = new_resolver.currentLink()->getTarget();
			assert(superblock == directory->superblock());
			auto result = co_await superblock->rename(resolver.currentLink().get(),
					directory.get(), new_resolver.nextComponent());
			if(!result) {
				assert(result.error() == Error::alreadyExists);
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::FstatAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::FstatAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: FSTATAT request" << std::endl;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if (req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			if (req->flags() & AT_EMPTY_PATH) {
				target_link = file->associatedLink();
			} else {
				PathResolver resolver;
				resolver.setup(self->fsContext()->getRoot(),
						relative_to, req->path());

				ResolveFlags resolveFlags = 0;
				if (req->flags() & AT_SYMLINK_NOFOLLOW)
				    resolveFlags |= resolveDontFollow;

				auto resolveResult = co_await resolver.resolve(resolveFlags);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				target_link = resolver.currentLink();
			}

			auto statsResult = co_await target_link->getTarget()->getStats();
			assert(statsResult);
			auto stats = statsResult.value();

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			DeviceId devnum;
			switch(target_link->getTarget()->getType()) {
			case VfsType::regular:
				resp.set_file_type(managarm::posix::FileType::FT_REGULAR);
				break;
			case VfsType::directory:
				resp.set_file_type(managarm::posix::FileType::FT_DIRECTORY);
				break;
			case VfsType::symlink:
				resp.set_file_type(managarm::posix::FileType::FT_SYMLINK);
				break;
			case VfsType::charDevice:
				resp.set_file_type(managarm::posix::FileType::FT_CHAR_DEVICE);
				devnum = target_link->getTarget()->readDevice();
				resp.set_ref_devnum(makedev(devnum.first, devnum.second));
				break;
			case VfsType::blockDevice:
				resp.set_file_type(managarm::posix::FileType::FT_BLOCK_DEVICE);
				devnum = target_link->getTarget()->readDevice();
				resp.set_ref_devnum(makedev(devnum.first, devnum.second));
				break;
			case VfsType::socket:
				resp.set_file_type(managarm::posix::FileType::FT_SOCKET);
				break;
			case VfsType::fifo:
				resp.set_file_type(managarm::posix::FileType::FT_FIFO);
				break;
			default:
				assert(target_link->getTarget()->getType() == VfsType::null);
			}

			if(stats.mode & ~0xFFFu)
				std::cout << "\e[31m" "posix: FsNode::getStats() returned illegal mode of "
						<< stats.mode << "\e[39m" << std::endl;

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

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::FchmodAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::FchmodAtRequest>(recv_head, tail);

			if(logRequests)
				std::cout << "posix: FCHMODAT request" << std::endl;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			if(req->flags()) {
				if(req->flags() & AT_SYMLINK_NOFOLLOW) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_SUPPORTED);
					continue;
				} else if(req->flags() & AT_EMPTY_PATH) {
					// Allowed, managarm extension
				} else {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
			}

			if(req->flags() & AT_EMPTY_PATH) {
				target_link = file->associatedLink();
			} else {
				PathResolver resolver;
				resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path());

				auto resolveResult = co_await resolver.resolve();

				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				target_link = resolver.currentLink();
			}

			co_await target_link->getTarget()->chmod(req->mode());

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::UtimensAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::UtimensAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: UTIMENSAT " << req->path() << std::endl;

			ViewPath relativeTo;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsNode> target = nullptr;

			if(!req->path().size()) {
				target = self->fileContext()->getFile(req->fd())->associatedLink()->getTarget();
			} else {
				if(req->flags() & ~AT_SYMLINK_NOFOLLOW) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}

				if(req->flags() & AT_SYMLINK_NOFOLLOW) {
					std::cout << "posix: AT_SYMLINK_FOLLOW is unimplemented for utimensat" << std::endl;
				}

				if(req->fd() == AT_FDCWD) {
					relativeTo = self->fsContext()->getWorkingDirectory();
				} else {
					file = self->fileContext()->getFile(req->fd());
					if (!file) {
						co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
						continue;
					}

					relativeTo = {file->associatedMount(), file->associatedLink()};
				}

				PathResolver resolver;
				resolver.setup(self->fsContext()->getRoot(),
						relativeTo, req->path());
				auto resolveResult = co_await resolver.resolve(resolvePrefix);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				target = resolver.currentLink()->getTarget();
			}

			co_await target->utimensat(req->atimeSec(), req->atimeNsec(), req->mtimeSec(), req->mtimeNsec());

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(req.request_type() == managarm::posix::CntReqType::READLINK) {
			if(logRequests || logPaths)
				std::cout << "posix: READLINK path: " << req.path() << std::endl;

			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req.path(), resolveDontFollow);
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::illegalOperationTarget) {
					managarm::posix::SvrResponse resp;
					resp.set_error(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);

					auto ser = resp.SerializeAsString();
					auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
							helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
							helix::action(&send_data, nullptr, 0));
					co_await transmit.async_wait();
					HEL_CHECK(send_resp.error());
					continue;
				} else if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					managarm::posix::SvrResponse resp;
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

					auto ser = resp.SerializeAsString();
					auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
							helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
							helix::action(&send_data, nullptr, 0));
					co_await transmit.async_wait();
					HEL_CHECK(send_resp.error());
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					managarm::posix::SvrResponse resp;
					resp.set_error(managarm::posix::Errors::NOT_A_DIRECTORY);

					auto ser = resp.SerializeAsString();
					auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
							helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
							helix::action(&send_data, nullptr, 0));
					co_await transmit.async_wait();
					HEL_CHECK(send_resp.error());
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto path = pathResult.value();

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
		}else if(preamble.id() == bragi::message_id<managarm::posix::OpenAtRequest>) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recvTail.error());

			auto req = bragi::parse_head_tail<managarm::posix::OpenAtRequest>(recv_head, tail);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			if(logRequests || logPaths)
				std::cout << "posix: OPENAT path: " << req->path() << std::endl;

			if((req->flags() & ~(managarm::posix::OpenFlags::OF_CREATE
					| managarm::posix::OpenFlags::OF_EXCLUSIVE
					| managarm::posix::OpenFlags::OF_NONBLOCK
					| managarm::posix::OpenFlags::OF_CLOEXEC
					| managarm::posix::OpenFlags::OF_TRUNC
					| managarm::posix::OpenFlags::OF_RDONLY
					| managarm::posix::OpenFlags::OF_WRONLY
					| managarm::posix::OpenFlags::OF_RDWR
					| managarm::posix::OpenFlags::OF_PATH))) {
				std::cout << "posix: OPENAT flags not recognized: " << req->flags() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			SemanticFlags semantic_flags = 0;
			if(req->flags() & managarm::posix::OpenFlags::OF_NONBLOCK)
				semantic_flags |= semanticNonBlock;

			if (req->flags() & managarm::posix::OpenFlags::OF_RDONLY)
				semantic_flags |= semanticRead;
			else if (req->flags() & managarm::posix::OpenFlags::OF_WRONLY)
				semantic_flags |= semanticWrite;
			else if (req->flags() & managarm::posix::OpenFlags::OF_RDWR)
				semantic_flags |= semanticRead | semanticWrite;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path());
			if(req->flags() & managarm::posix::OpenFlags::OF_CREATE) {
				auto resolveResult = co_await resolver.resolve(resolvePrefix);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				if(logRequests)
					std::cout << "posix: Creating file " << req->path() << std::endl;

				auto directory = resolver.currentLink()->getTarget();
				auto tailResult = co_await directory->getLink(resolver.nextComponent());
				assert(tailResult);
				auto tail = tailResult.value();
				if(tail) {
					if(req->flags() & managarm::posix::OpenFlags::OF_EXCLUSIVE) {
						co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
						continue;
					}else{
						auto fileResult = co_await tail->getTarget()->open(
											resolver.currentView(), std::move(tail),
											semantic_flags);
						assert(fileResult);
						file = fileResult.value();
						assert(file);
					}
				}else{
					assert(directory->superblock());
					auto node = co_await directory->superblock()->createRegular();
					// Due to races, link() can fail here.
					// TODO: Implement a version of link() that eithers links the new node
					// or returns the current node without failing.
					auto linkResult = co_await directory->link(resolver.nextComponent(), node);
					assert(linkResult);
					auto link = linkResult.value();
					auto fileResult = co_await node->open(resolver.currentView(), std::move(link),
										semantic_flags);
					assert(fileResult);
					file = fileResult.value();
					assert(file);
				}
			}else{
				auto resolveResult = co_await resolver.resolve();
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				auto target = resolver.currentLink()->getTarget();

				if(req->flags() & managarm::posix::OpenFlags::OF_PATH) {
					auto dummyFile = smarter::make_shared<DummyFile>(resolver.currentView(), resolver.currentLink());
					DummyFile::serve(dummyFile);
					file = File::constructHandle(std::move(dummyFile));
				} else {
					auto fileResult = co_await target->open(resolver.currentView(), resolver.currentLink(), semantic_flags);
					if(!fileResult) {
						if(fileResult.error() == Error::noBackingDevice) {
							co_await sendErrorResponse(managarm::posix::Errors::SPECIAL_DEVICE);
							continue;
						} else {
							std::cout << "posix: Unexpected failure from open()" << std::endl;
							co_return;
						}
					}
					assert(fileResult);
					file = fileResult.value();
				}
			}

			if(!file) {
				if(logRequests)
					std::cout << "posix:     OPEN failed: file not found" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
			}

			if(req->flags() & managarm::posix::OpenFlags::OF_TRUNC) {
				auto result = co_await file->truncate(0);
				assert(result);
			}
			int fd = self->fileContext()->attachFile(file,
					req->flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
		}else if(preamble.id() == bragi::message_id<managarm::posix::CloseRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::CloseRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			if(logRequests)
				std::cout << "posix: CLOSE file descriptor " << req->fd() << std::endl;

			self->fileContext()->closeFile(req->fd());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP) {
			if(logRequests)
				std::cout << "posix: DUP" << std::endl;

			auto file = self->fileContext()->getFile(req.fd());

			if (!file) {
				helix::SendBuffer send_resp;

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::BAD_FD);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)) {
				helix::SendBuffer send_resp;

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			int newfd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

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

			if (!file || req.newfd() < 0) {
				helix::SendBuffer send_resp;

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::BAD_FD);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			if(req.flags()) {
				helix::SendBuffer send_resp;

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			self->fileContext()->attachFile(req.newfd(), file);

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == bragi::message_id<managarm::posix::IsTtyRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::IsTtyRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			if(logRequests)
				std::cout << "posix: IS_TTY" << std::endl;

			auto file = self->fileContext()->getFile(req->fd());
			if(!file) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_mode(file->isTerminal());

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
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
					traversed = ViewPath{dir.first->getParent(), anchor};
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
		}else if(preamble.id() == managarm::posix::UnlinkAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::UnlinkAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: UNLINKAT path: " << req->path() << std::endl;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if(req->flags()) {
				if(req->flags() & AT_REMOVEDIR) {
					std::cout << "posix: UNLINKAT flag AT_REMOVEDIR handling unimplemented" << std::endl;
				} else {
					std::cout << "posix: UNLINKAT flag handling unimplemented with unknown flag: " << req->flags() << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				}
			} 

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path());

			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			target_link = resolver.currentLink();

			auto owner = target_link->getOwner();
			if(!owner) {
				co_await sendErrorResponse(managarm::posix::Errors::RESOURCE_IN_USE);
				continue;
			}
			auto result = co_await owner->unlink(target_link->getName());
			if(!result) {
				if(result.error() == Error::noSuchFile) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				}else{
					std::cout << "posix: Unexpected failure from unlink()" << std::endl;
					co_return;
				}
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::RmdirRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::RmdirRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: RMDIR " << req->path() << std::endl;

			std::cout << "\e[31mposix: RMDIR: always removes the directory, even when not empty\e[39m" << std::endl;
			std::shared_ptr<FsLink> target_link;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(), self->fsContext()->getWorkingDirectory(),
					req->path());

			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			target_link = resolver.currentLink();

			auto owner = target_link->getOwner();
			auto result = co_await owner->rmdir(target_link->getName());
			if(!result) {
				std::cout << "posix: Unexpected failure from rmdir()" << std::endl;
				co_return;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
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
				flags |= FD_CLOEXEC;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_flags(flags);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::FD_SET_FLAGS) {
			if(logRequests)
				std::cout << "posix: FD_SET_FLAGS" << std::endl;

			if(req.flags() & ~FD_CLOEXEC) {
				std::cout << "posix: FD_SET_FLAGS unknown flags: " << req.flags() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}
			int closeOnExec = req.flags() & FD_CLOEXEC;
			if(self->fileContext()->setDescriptor(req.fd(), closeOnExec) != Error::success) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SIG_ACTION) {
			if(logRequests)
				std::cout << "posix: SIG_ACTION" << std::endl;

			if(req.flags() & ~(SA_ONSTACK | SA_SIGINFO | SA_RESETHAND | SA_NODEFER | SA_RESTART | SA_NOCLDSTOP)) {
				std::cout << "\e[31mposix: Unknown SIG_ACTION flags: 0x"
						<< std::hex << req.flags()
						<< std::dec << "\e[39m" << std::endl;
				assert(!"Flags not implemented");
			}

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;

			if(req.sig_number() <= 0 || req.sig_number() > 64) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				co_await transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
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
				if(req.flags() & SA_ONSTACK)
					std::cout << "\e[31mposix: Ignoring SA_ONSTACK\e[39m" << std::endl;
				if(req.flags() & SA_RESTART)
					std::cout << "\e[31mposix: Ignoring SA_RESTART\e[39m" << std::endl;
				if(req.flags() & SA_NOCLDSTOP)
					std::cout << "\e[31mposix: Ignoring SA_NOCLDSTOP\e[39m" << std::endl;

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

			bool nonBlock = false;

			if(req.flags() & O_NONBLOCK)
				nonBlock = true;

			helix::SendBuffer send_resp;

			auto pair = fifo::createPair(nonBlock);
			auto r_fd = self->fileContext()->attachFile(std::get<0>(pair),
					req.flags() & O_CLOEXEC);
			auto w_fd = self->fileContext()->attachFile(std::get<1>(pair),
					req.flags() & O_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.add_fds(r_fd);
			resp.add_fds(w_fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SETSID) {
			if(logRequests)
				std::cout << "posix: SETSID" << std::endl;

			auto session = TerminalSession::initializeNewSession(self.get());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_sid(session->getSessionId());

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::SocketRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SocketRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SOCKET" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			assert(!(req->flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));

			smarter::shared_ptr<File, FileHandle> file;
			if(req->domain() == AF_UNIX) {
				assert(req->socktype() == SOCK_DGRAM || req->socktype() == SOCK_STREAM
						|| req->socktype() == SOCK_SEQPACKET);
				assert(!req->protocol());

				file = un_socket::createSocketFile(req->flags() & SOCK_NONBLOCK);
			}else if(req->domain() == AF_NETLINK) {
				assert(req->socktype() == SOCK_RAW || req->socktype() == SOCK_DGRAM);
				file = nl_socket::createSocketFile(req->protocol());
			} else if (req->domain() == AF_INET) {
				file = co_await extern_socket::createSocket(
					co_await net::getNetLane(),
					req->domain(),
					req->socktype(), req->protocol(),
					req->flags() & SOCK_NONBLOCK);
			}else{
				throw std::runtime_error("posix: Handle unknown protocol families");
			}

			auto fd = self->fileContext()->attachFile(file,
					req->flags() & SOCK_CLOEXEC);

			resp.set_fd(fd);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::SockpairRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SockpairRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SOCKPAIR" << std::endl;

			assert(!(req->flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));

			if(req->flags() & SOCK_NONBLOCK)
				std::cout << "\e[31mposix: socketpair(SOCK_NONBLOCK)"
						" is not implemented correctly\e[39m" << std::endl;

			assert(req->domain() == AF_UNIX);
			assert(req->socktype() == SOCK_DGRAM || req->socktype() == SOCK_STREAM
					|| req->socktype() == SOCK_SEQPACKET);
			assert(!req->protocol());

			auto pair = un_socket::createSocketPair(self.get());
			auto fd0 = self->fileContext()->attachFile(std::get<0>(pair),
					req->flags() & SOCK_CLOEXEC);
			auto fd1 = self->fileContext()->attachFile(std::get<1>(pair),
					req->flags() & SOCK_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.add_fds(fd0);
			resp.add_fds(fd1);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::AcceptRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::AcceptRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: ACCEPT" << std::endl;

			auto sockfile = self->fileContext()->getFile(req->fd());
			if(!sockfile) {
				co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
				continue;
			}

			auto newfileResult = co_await sockfile->accept(self.get());
			if(!newfileResult) {
				assert(newfileResult.error() == Error::wouldBlock);
				co_await sendErrorResponse(managarm::posix::Errors::WOULD_BLOCK);
				continue;
			}
			auto newfile = newfileResult.value();
			auto fd = self->fileContext()->attachFile(std::move(newfile));

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

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

			assert(!(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)));

			auto file = epoll::createFile();
			auto fd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

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
			if(!file || !epfile) {
				co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
				continue;
			}

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
			if(!epfile || !file) {
				std::cout << "posix: Illegal FD for EPOLL_DELETE" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
				continue;
			}

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
			uint64_t former = self->signalMask();

			auto epfile = self->fileContext()->getFile(req.fd());
			if(!epfile) {
				co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
				continue;
			}
			if(req.sigmask_needed()) {
				self->setSignalMask(req.sigmask());
			}

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
			if(req.sigmask_needed()) {
				self->setSignalMask(former);
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

			assert(!(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)));

			auto file = createSignalFile(req.sigset());
			auto fd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::InotifyCreateRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::InotifyCreateRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: INOTIFY_CREATE" << std::endl;

			assert(!(req->flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)));

			auto file = inotify::createFile();
			auto fd = self->fileContext()->attachFile(file,
					req->flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::InotifyAddRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::InotifyAddRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			managarm::posix::SvrResponse resp;

			if(logRequests || logPaths)
				std::cout << "posix: INOTIFY_ADD" << req->path() << std::endl;

			auto ifile = self->fileContext()->getFile(req->fd());
			if(!ifile) {
				co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
				continue;
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req->path());
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto wd = inotify::addWatch(ifile.get(), resolver.currentLink()->getTarget(),
					req->flags());

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_wd(wd);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::EventfdCreateRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::EventfdCreateRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: EVENTFD_CREATE" << std::endl;

			managarm::posix::SvrResponse resp;

			if (req->flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC | managarm::posix::OpenFlags::OF_NONBLOCK)) {
				std::cout << "posix: invalid flag specified (EFD_SEMAPHORE?)" << std::endl;
				std::cout << "posix: flags specified: " << req->flags() << std::endl;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {

				auto file = eventfd::createFile(req->initval(), req->flags() & managarm::posix::OpenFlags::OF_NONBLOCK);
				auto fd = self->fileContext()->attachFile(file,
						req->flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::MknodAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::MknodAtRequest>(recv_head, tail);

			if(!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: MKNODAT for path " << req->path() << " with mode " << req->mode() << " and device " << req->device() << std::endl;

			managarm::posix::SvrResponse resp;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->dirfd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->dirfd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::BAD_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto parent = resolver.currentLink()->getTarget();
			auto existsResult = co_await parent->getLink(resolver.nextComponent());
			assert(existsResult);
			auto exists = existsResult.value();
			if(exists) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			VfsType type;
			DeviceId dev;
			if(S_ISDIR(req->mode())) {
				type = VfsType::directory;
			} else if(S_ISCHR(req->mode())) {
				type = VfsType::charDevice;
			} else if(S_ISBLK(req->mode())) {
				type = VfsType::blockDevice;
			} else if(S_ISREG(req->mode())) {
				type = VfsType::regular;
			} else if(S_ISFIFO(req->mode())) {
				type = VfsType::fifo;
			} else if(S_ISLNK(req->mode())) {
				type = VfsType::symlink;
			} else if(S_ISSOCK(req->mode())) {
				type = VfsType::socket;
			} else {
				type = VfsType::null;
			}

			if(type == VfsType::charDevice || type == VfsType::blockDevice) {
				dev.first = major(req->device());
				dev.second = minor(req->device());

				auto result = co_await parent->mkdev(resolver.nextComponent(), type, dev);
				if(!result) {
					if(result.error() == Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from mkdev()" << std::endl;
						co_return;
					}
				}
			} else if(type == VfsType::fifo) {
				auto result = co_await parent->mkfifo(resolver.nextComponent(), req->mode());
				if(!result) {
					if(result.error() == Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
						co_return;
					}
				}
			} else if(type == VfsType::socket) {
				auto result = co_await parent->mksocket(resolver.nextComponent());
				if(!result) {
					if(result.error() == Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from mksocket()" << std::endl;
						co_return;
					}
				}
			} else {
				// TODO: Handle regular files.
				std::cout << "\e[31mposix: Creating regular files with mknod is not supported.\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

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
		std::cout << "\e[33mposix: Exiting serveRequests()\e[39m" << std::endl;
	generation->requestsDone.raise();
}

async::result<void> serve(std::shared_ptr<Process> self, std::shared_ptr<Generation> generation) {
	auto thread = self->threadDescriptor();

	std::array<char, 16> creds;
	HEL_CHECK(helGetCredentials(thread.getHandle(), 0, creds.data()));
	auto res = globalCredentialsMap.insert({creds, self});
	assert(res.second);

	co_await async::when_all(
		observeThread(self, generation),
		serveSignals(self, generation),
		serveRequests(self, generation)
	);
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
	async::detach(net::enumerateNetserver());
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
		charRegistry.install(createNullDevice());
		charRegistry.install(createFullDevice());
		charRegistry.install(createZeroDevice());
		block_subsystem::run();
		drm_subsystem::run();
		input_subsystem::run();
		pci_subsystem::run();

		runInit();
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);
}
