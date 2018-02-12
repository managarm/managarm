
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <iomanip>
#include <iostream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "common.hpp"
#include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "epoll.hpp"
#include "exec.hpp"
#include "extern_fs.hpp"
#include "devices/helout.hpp"
#include "signalfd.hpp"
#include "socket.hpp"
#include "subsystem/block.hpp"
#include "subsystem/drm.hpp"
#include "sysfs.hpp"
#include "timerfd.hpp"
#include "tmp_fs.hpp"
#include <posix.pb.h>

bool traceRequests = false;
bool logPaths = true;

cofiber::no_future serve(std::shared_ptr<Process> self, helix::UniqueDescriptor p);

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

COFIBER_ROUTINE(cofiber::no_future, observe(std::shared_ptr<Process> self,
		helix::BorrowedDescriptor thread), ([=] {
	while(true) {
		helix::Observe observe;
		auto &&submit = helix::submitObserve(thread, &observe, helix::Dispatcher::global());
		COFIBER_AWAIT(submit.async_wait());
		HEL_CHECK(observe.error());

		if(observe.observation() == kHelObserveSuperCall + 1) {
//			std::cout << "clientFileTable supercall" << std::endl;
			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			gprs[4] = kHelErrNone;
			gprs[5] = reinterpret_cast<uintptr_t>(self->clientFileTable());
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helResume(thread.getHandle()));
		}else if(observe.observation() == kHelObserveSuperCall + 2) {
//			std::cout << "fork supercall" << std::endl;
			auto child = Process::fork(self);
	
			HelHandle new_thread;
			HEL_CHECK(helCreateThread(child->fileContext()->getUniverse().getHandle(),
					child->vmContext()->getSpace().getHandle(), kHelAbiSystemV,
					0, 0, kHelThreadStopped, &new_thread));
			serve(child, helix::UniqueDescriptor(new_thread));

			// Copy registers from the current thread to the new one.
			uintptr_t pcrs[2], gprs[15], thrs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsThread, &thrs));
			
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsProgram, &pcrs));
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsThread, &thrs));

			// Setup post supercall registers in both threads and finally resume the threads.
			gprs[4] = kHelErrNone;
			gprs[5] = 1;
			HEL_CHECK(helStoreRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			gprs[5] = 0;
			HEL_CHECK(helStoreRegisters(new_thread, kHelRegsGeneral, &gprs));

			HEL_CHECK(helResume(thread.getHandle()));
			HEL_CHECK(helResume(new_thread));
		}else if(observe.observation() == kHelObserveSuperCall + 3) {
//			std::cout << "execve supercall" << std::endl;
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

			// Parse both the arguments and the environment areas.
			size_t k;

			std::vector<std::string> args;
			k = 0;
			while(k < args_area.size()) {
				auto d = args_area.find(char(0), k);
				assert(d != std::string::npos);
				args.push_back(args_area.substr(k, d - k));
				std::cout << "arg: " << args.back() << std::endl;
				k = d + 1;
			}

			std::vector<std::string> env;
			k = 0;
			while(k < env_area.size()) {
				auto d = env_area.find(char(0), k);
				assert(d != std::string::npos);
				env.push_back(env_area.substr(k, d - k));
				std::cout << "env: " << env.back() << std::endl;
				k = d + 1;
			}

			Process::exec(self, path, std::move(args), std::move(env));
		}else if(observe.observation() == kHelObserveSuperCall + 4) {
			printf("\e[35mThread exited\e[39m\n");
			HEL_CHECK(helCloseDescriptor(thread.getHandle()));
			return;
		}else if(observe.observation() == kHelObservePanic) {
			printf("\e[35mUser space panic\n");
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);
		}else if(observe.observation() == kHelObserveBreakpoint) {
			printf("\e[35mBreakpoint\n");
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);
		}else if(observe.observation() == kHelObservePageFault) {
			printf("\e[31mPage fault\n");
			dumpRegisters(thread);
			printf("\e[39m");
			fflush(stdout);
		}else{
			throw std::runtime_error("Unexpected observation");
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serve(std::shared_ptr<Process> self,
		helix::UniqueDescriptor p), ([self, lane = std::move(p)] {
	observe(self, lane);

	while(true) {
		helix::Accept accept;
		helix::RecvBuffer recv_req;

		char buffer[256];
		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req, buffer, 256));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::posix::CntRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.request_type() == managarm::posix::CntReqType::GET_PID) {
			helix::SendBuffer send_resp;

			std::cout << "\e[31mposix: Fix GET_PID\e[39m" << std::endl;
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(1);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::VM_MAP) {
			helix::SendBuffer send_resp;

			// TODO: Validate mode and flags.
			std::cout << "flags: " << req.flags() << std::endl;

			uint32_t native_flags = kHelMapCopyOnWriteAtFork;
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

				HelHandle memory;
				HEL_CHECK(helAllocateMemory(req.size(), 0, &memory));

				// Perform the actual mapping.
				HEL_CHECK(helMapMemory(memory, self->vmContext()->getSpace().getHandle(),
						nullptr, 0, req.size(), native_flags, &address));
				HEL_CHECK(helCloseDescriptor(memory));
			}else{
				auto file = self->fileContext()->getFile(req.fd());
				assert(file);
				auto memory = COFIBER_AWAIT file->accessMemory(req.rel_offset());

				// Perform the actual mapping.
				HEL_CHECK(helMapMemory(memory.getHandle(),
						self->vmContext()->getSpace().getHandle(),
						nullptr, 0 /*req.rel_offset()*/, req.size(), native_flags, &address));
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_offset(reinterpret_cast<uintptr_t>(address));

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::MOUNT) {
			helix::SendBuffer send_resp;

			auto target = COFIBER_AWAIT resolve(self->fsContext()->getRoot(), req.target_path());
			assert(target.second);
			if(req.fs_type() == "sysfs") {
				target.first->mount(target.second, getSysfs());	
			}else if(req.fs_type() == "devtmpfs") {
				target.first->mount(target.second, getDevtmpfs());	
			}else if(req.fs_type() == "tmpfs") {
				target.first->mount(target.second, tmp_fs::createRoot());	
			}else{
				assert(req.fs_type() == "ext2");
				auto source = COFIBER_AWAIT resolve(self->fsContext()->getRoot(), req.path());
				assert(source.second);
				assert(source.second->getTarget()->getType() == VfsType::blockDevice);
				auto device = blockRegistry.get(source.second->getTarget()->readDevice());
				auto link = COFIBER_AWAIT device->mount();
				target.first->mount(target.second, std::move(link));	
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::CHROOT) {
			helix::SendBuffer send_resp;

			auto path = COFIBER_AWAIT resolve(self->fsContext()->getRoot(), req.path());
			if(path.second) {
				self->fsContext()->changeRoot(path);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::ACCESS) {
			helix::SendBuffer send_resp;

			auto path = COFIBER_AWAIT resolve(self->fsContext()->getRoot(), req.path());
			if(path.second) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::STAT) {	
			if(logPaths)
				std::cout << "posix: STAT path: " << req.path() << std::endl;

			helix::SendBuffer send_resp;

			auto path = COFIBER_AWAIT resolve(self->fsContext()->getRoot(), req.path());
			if(path.second) {
				auto stats = COFIBER_AWAIT path.second->getTarget()->getStats();

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				switch(path.second->getTarget()->getType()) {
				case VfsType::regular:
					resp.set_file_type(managarm::posix::FT_REGULAR); break;
				case VfsType::directory:
					resp.set_file_type(managarm::posix::FT_DIRECTORY); break;
				case VfsType::charDevice:
					resp.set_file_type(managarm::posix::FT_CHAR_DEVICE); break;
				case VfsType::blockDevice:
					resp.set_file_type(managarm::posix::FT_BLOCK_DEVICE); break;
				}

				resp.set_inode_num(stats.inodeNumber);
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
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::READLINK) {
			if(logPaths)
				std::cout << "posix: READLINK path: " << req.path() << std::endl;

			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;
			
			auto path = COFIBER_AWAIT resolve(self->fsContext()->getRoot(),
					req.path(), resolveDontFollow);
			if(!path.second) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&send_data, nullptr, 0));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
				continue;
			}

			auto result = COFIBER_AWAIT path.second->getTarget()->readSymlink();
			if(auto error = std::get_if<Error>(&result); error) {
				assert(*error == Error::illegalOperationTarget);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&send_data, nullptr, 0));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				auto &target = std::get<std::string>(result);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
						helix::action(&send_data, target.data(), target.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::OPEN) {	
			if(logPaths)
				std::cout << "posix: OPEN path: " << req.path()	<< std::endl;

			helix::SendBuffer send_resp;

			auto file = COFIBER_AWAIT open(self->fsContext()->getRoot(), req.path());
			if(file) {
				int fd = self->fileContext()->attachFile(file);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
						helix::action(&send_resp, ser.data(), ser.size()));
				COFIBER_AWAIT transmit.async_wait();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::CLOSE) {
			self->fileContext()->closeFile(req.fd());

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP) {
			auto file = self->fileContext()->getFile(req.fd());
			assert(file);
			
			int newfd = self->fileContext()->attachFile(file);

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(newfd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP2) {
			auto file = self->fileContext()->getFile(req.fd());
			assert(file);

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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::FSTAT) {
			auto file = self->fileContext()->getFile(req.fd());
			auto stats = COFIBER_AWAIT file->associatedLink()->getTarget()->getStats();

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			switch(file->associatedLink()->getTarget()->getType()) {
			case VfsType::regular:
				resp.set_file_type(managarm::posix::FT_REGULAR); break;
			case VfsType::directory:
				resp.set_file_type(managarm::posix::FT_DIRECTORY); break;
			case VfsType::charDevice:
				resp.set_file_type(managarm::posix::FT_CHAR_DEVICE); break;
			case VfsType::blockDevice:
				resp.set_file_type(managarm::posix::FT_BLOCK_DEVICE); break;
			}

			resp.set_inode_num(stats.inodeNumber);
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::IS_TTY) {
			helix::SendBuffer send_resp;

			std::cout << "\e[31mposix: Fix IS_TTY\e[39m" << std::endl;
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TTY_NAME) {
			helix::SendBuffer send_resp;

			std::cout << "\e[31mposix: Fix TTY_NAME\e[39m" << std::endl;
			managarm::posix::SvrResponse resp;
			resp.set_path("/dev/ttyS0");
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SOCKPAIR) {
			helix::SendBuffer send_resp;

			assert(req.domain() == AF_UNIX);
			assert(req.socktype() == SOCK_DGRAM);
			assert(!req.protocol());

			auto pair = createUnixSocketPair();
			auto fd0 = self->fileContext()->attachFile(std::get<0>(pair));
			auto fd1 = self->fileContext()->attachFile(std::get<1>(pair));

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.mutable_fds()->Add(fd0);
			resp.mutable_fds()->Add(fd1);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CREATE) {
			helix::SendBuffer send_resp;

			auto file = createEpollFile();
			auto fd = self->fileContext()->attachFile(file);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CTL) {
			helix::SendBuffer send_resp;

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			assert(epfile);
			assert(file);

			epollCtl(epfile.get(), file.get(), req.flags(), req.cookie());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_WAIT) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;
			
			auto epfile = self->fileContext()->getFile(req.fd());
			assert(epfile);
			auto event = COFIBER_AWAIT epollWait(epfile.get());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, &event, sizeof(struct epoll_event)));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TIMERFD_CREATE) {
			helix::SendBuffer send_resp;

			auto file = createTimerFile();
			auto fd = self->fileContext()->attachFile(file);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SIGNALFD_CREATE) {
			helix::SendBuffer send_resp;

			auto file = createSignalFile();
			auto fd = self->fileContext()->attachFile(file);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else{
			std::cout << "posix: Illegal request" << std::endl;
			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

std::shared_ptr<sysfs::Object> cardObject;

struct UeventAttribute : sysfs::Attribute {
	static auto singleton() {
		static UeventAttribute attr;
		return &attr;
	}

private:
	UeventAttribute()
	: sysfs::Attribute("uevent") { }

public:
	virtual std::string show(sysfs::Object *) override {
		std::cout << "\e[31mposix: uevent files are static\e[39m" << std::endl;
		return std::string{"DEVNAME=dri/card0\n"};
	}
};

COFIBER_ROUTINE(cofiber::no_future, runInit(), ([] {
	auto devs_object = std::make_shared<sysfs::Object>(nullptr, "devices");
	devs_object->addObject();
	cardObject = std::make_shared<sysfs::Object>(devs_object, "card0");
	cardObject->addObject();
	cardObject->createAttribute(UeventAttribute::singleton());

	auto cls_object = std::make_shared<sysfs::Object>(nullptr, "class");
	cls_object->addObject();
	auto drm_object = std::make_shared<sysfs::Object>(cls_object, "drm");
	drm_object->addObject();
	drm_object->createSymlink("card0", cardObject);

	COFIBER_AWAIT populateRootView();
	Process::init("sbin/posix-init");
}))

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;

	charRegistry.install(createHeloutDevice());
	block_system::run();
	drm_system::run();
	runInit();

	while(true)
		helix::Dispatcher::global().dispatch();
}

