#include <protocols/mbus/client.hpp>

#include "net.hpp"
#include "clock.hpp"
#include "drvcore.hpp"
#include "devices/full.hpp"
#include "devices/helout.hpp"
#include "devices/null.hpp"
#include "devices/random.hpp"
#include "devices/urandom.hpp"
#include "devices/zero.hpp"
#include "gdbserver.hpp"
#include "pts.hpp"
#include "requests.hpp"
#include "subsystem/block.hpp"
#include "subsystem/drm.hpp"
#include "subsystem/generic.hpp"
#include "subsystem/input.hpp"
#include "subsystem/pci.hpp"

#include <kerncfg.pb.h>
#include <posix.bragi.hpp>
#include <protocols/posix/data.hpp>
#include <protocols/posix/supercalls.hpp>

#include "debug-options.hpp"

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

		if(observe.observation() == kHelObserveSuperCall + posix::superAnonAllocate) {
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			size_t size = gprs[kHelRegArg0];

			void *address = co_await self->vmContext()->mapFile(0,
					{}, nullptr,
					0, size, true, kHelMapProtRead | kHelMapProtWrite);

			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = reinterpret_cast<uintptr_t>(address);
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superAnonDeallocate) {
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			self->vmContext()->unmapFile(reinterpret_cast<void *>(gprs[kHelRegArg0]), gprs[kHelRegArg1]);

			gprs[kHelRegError] = kHelErrNone;
			gprs[kHelRegOut0] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superGetProcessData) {
			posix::ManagarmProcessData data = {
				self->clientPosixLane(),
				self->fileContext()->clientMbusLane(),
				self->clientThreadPage(),
				static_cast<HelHandle *>(self->clientFileTable()),
				self->clientClkTrackerPage()
			};

			if(logRequests)
				std::cout << "posix: GET_PROCESS_DATA supercall" << std::endl;
			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto storeData = co_await helix_ng::writeMemory(thread, gprs[kHelRegArg0],
					sizeof(posix::ManagarmProcessData), &data);
			HEL_CHECK(storeData.error());
			gprs[kHelRegError] = kHelErrNone;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superFork) {
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
		}else if(observe.observation() == kHelObserveSuperCall + posix::superClone) {
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
		}else if(observe.observation() == kHelObserveSuperCall + posix::superExecve) {
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
		}else if(observe.observation() == kHelObserveSuperCall + posix::superExit) {
			if(logRequests)
				std::cout << "posix: EXIT supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto code = gprs[kHelRegArg0];

			co_await self->terminate(TerminationByExit{static_cast<int>(code & 0xFF)});
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigMask) {
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
			gprs[kHelRegOut1] = self->enteredSignalSeq();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			bool killed = false;
			if(self->checkOrRequestSignalRaise()) {
				auto active = co_await self->signalContext()->fetchSignal(
						~self->signalMask(), true);
				if(active) {
					co_await self->signalContext()->raiseContext(active, self.get(), killed);
				}
			}
			if(killed)
				break;

			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigRaise) {
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
			auto active = co_await self->signalContext()->fetchSignal(~self->signalMask(), true);
			if(active)
				co_await self->signalContext()->raiseContext(active, self.get(), killed);
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigRestore) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_RESTORE supercall" << std::endl;

			co_await self->signalContext()->restoreContext(thread);
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigKill) {
			if(logRequests || logSignals)
				std::cout << "posix: SIG_KILL supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			auto pid = (intptr_t)gprs[kHelRegArg0];
			auto sn = gprs[kHelRegArg1];

			std::shared_ptr<Process> target;
			std::shared_ptr<ProcessGroup> targetGroup;
			if(!pid) {
				if(logSignals)
					std::cout << "posix: SIG_KILL on PGRP " << self->pid()
						<< " (self)" << std::endl;
				targetGroup = self->pgPointer();
			} else if(pid == -1) {
				std::cout << "posix: SIG_KILL(-1) is ignored!" << std::endl;
				HEL_CHECK(helResume(thread.getHandle()));
				break;
			} else if(pid > 0) {
				if(logSignals)
					std::cout << "posix: SIG_KILL on PID " << pid << std::endl;
				target = Process::findProcess(pid);
			} else {
				if(logSignals)
					std::cout << "posix: SIG_KILL on PGRP " << -pid << std::endl;
				targetGroup = ProcessGroup::findProcessGroup(-pid);
			}

			// Clear the error code.
			// TODO: This should only happen is raising succeeds. Move it somewhere else?
			gprs[kHelRegError] = 0;
			if(!target && !targetGroup) {
				gprs[kHelRegOut0] = ESRCH;
				HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
				HEL_CHECK(helResume(thread.getHandle()));
				break;
			}

			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			UserSignal info;
			info.pid = self->pid();
			info.uid = 0;
			if(sn) {
				if(targetGroup) {
					targetGroup->issueSignalToGroup(sn, info);
				} else {
					target->signalContext()->issueSignal(sn, info);
				}
			}

			// If the process signalled itself, we should process the signal before resuming.
			bool killed = false;
			if(self->checkOrRequestSignalRaise()) {
				auto active = co_await self->signalContext()->fetchSignal(
						~self->signalMask(), true);
				if(active)
					co_await self->signalContext()->raiseContext(active, self.get(), killed);
			}
			if(killed)
				break;
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigAltStack) {
			// sigaltstack is implemented as a supercall because it
			// needs to access the thread's registers.

			if(logRequests || logSignals)
				std::cout << "posix: SIGALTSTACK supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			uintptr_t pcrs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));

			auto ss = gprs[kHelRegArg0];
			auto oss = gprs[kHelRegArg1];

			if (oss) {
				stack_t st{};
				st.ss_sp = reinterpret_cast<void *>(self->altStackSp());
				st.ss_size = self->altStackSize();
				st.ss_flags = (self->isOnAltStack(pcrs[kHelRegSp]) ? SS_ONSTACK : 0)
						| (self->isAltStackEnabled() ? 0 : SS_DISABLE);

				auto store = co_await helix_ng::writeMemory(self->vmContext()->getSpace(),
						oss, sizeof(stack_t), &st);
				HEL_CHECK(store.error());
			}

			int error = 0;

			if (ss) {
				stack_t st{};

				auto load = co_await helix_ng::readMemory(self->vmContext()->getSpace(),
						ss, sizeof(stack_t), &st);
				HEL_CHECK(load.error());

				if (st.ss_flags & ~SS_DISABLE) {
					error = EINVAL;
				} else if (self->isOnAltStack(pcrs[kHelRegSp])) {
					error = EPERM;
				} else {
					self->setAltStackSp(reinterpret_cast<uint64_t>(st.ss_sp), st.ss_size);
					self->setAltStackEnabled(!(st.ss_flags & SS_DISABLE));
				}
			}

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = error;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superSigSuspend) {
			if(logRequests || logSignals)
				std::cout << "posix: SIGSUSPEND supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			auto seq = gprs[kHelRegArg0];

			if (seq == self->enteredSignalSeq()) {
				auto check = self->signalContext()->checkSignal();

				if (!std::get<1>(check))
					co_await self->signalContext()->pollSignal(std::get<0>(check), UINT64_C(-1));
			}

			gprs[kHelRegError] = 0;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + posix::superGetTid){
			if(logRequests)
				std::cout << "posix: GET_TID supercall" << std::endl;

			uintptr_t gprs[kHelNumGprs];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[kHelRegError] = 0;
			gprs[kHelRegOut0] = self->tid();
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveInterrupt) {
			//printf("posix: Process %s was interrupted\n", self->path().c_str());
			bool killed = false;
			if(self->checkOrRequestSignalRaise()) {
				auto active = co_await self->signalContext()->fetchSignal(
						~self->signalMask(), true);
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

			if(debugFaults) {
				launchGdbServer(self.get());
				co_await async::suspend_indefinitely({});
			}

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

			if(debugFaults) {
				launchGdbServer(self.get());
				co_await async::suspend_indefinitely({});
			}
		}else if(observe.observation() == kHelObservePageFault) {
			printf("\e[31mposix: Page fault in process %s\n", self->path().c_str());
			dumpRegisters(self);
			printf("\e[39m");
			fflush(stdout);

			if(debugFaults) {
				launchGdbServer(self.get());
				co_await async::suspend_indefinitely({});
			}

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

			if(debugFaults) {
				launchGdbServer(self.get());
				co_await async::suspend_indefinitely({});
			}

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

			if(debugFaults) {
				launchGdbServer(self.get());
				co_await async::suspend_indefinitely({});
			}

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
	async::oneshot_event foundKerncfg;
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

	async::result<void> store(std::string) override {
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
		foundKerncfg.raise();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundKerncfg.wait();

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

	drvcore::initialize();

	charRegistry.install(createHeloutDevice());
	charRegistry.install(pts::createMasterDevice());
	charRegistry.install(createNullDevice());
	charRegistry.install(createFullDevice());
	charRegistry.install(createRandomDevice());
	charRegistry.install(createUrandomDevice());
	charRegistry.install(createZeroDevice());
	block_subsystem::run();
	drm_subsystem::run();
	generic_subsystem::run();
	input_subsystem::run();
	pci_subsystem::run();

	runInit();

	async::run_forever(helix::currentDispatcher);
}
