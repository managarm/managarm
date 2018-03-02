
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <iomanip>
#include <iostream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "common.hpp"
#include "device.hpp"
#include "nl-socket.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "epoll.hpp"
#include "exec.hpp"
#include "extern_fs.hpp"
#include "devices/helout.hpp"
#include "signalfd.hpp"
#include "subsystem/block.hpp"
#include "subsystem/drm.hpp"
#include "subsystem/input.hpp"
#include "sysfs.hpp"
#include "un-socket.hpp"
#include "timerfd.hpp"
#include "tmp_fs.hpp"
#include <posix.pb.h>

bool logRequests = false;
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
			
			if(logPaths)
				std::cout << "posix: execve path: " << path << std::endl;

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
		}else if(observe.observation() == kHelObserveGeneralFault) {
			printf("\e[31mGeneral fault\n");
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

			uint32_t native_flags = 0;
			if((req.flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_PRIVATE) {
				native_flags |= kHelMapCopyOnWriteAtFork;
				std::cout << "posix: Private mappings are not really supported" << std::endl;
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

				HelHandle memory;
				HEL_CHECK(helAllocateMemory(req.size(), 0, &memory));

				// Perform the actual mapping.
				HEL_CHECK(helMapMemory(memory, self->vmContext()->getSpace().getHandle(),
						nullptr, 0, req.size(), native_flags, &address));
				HEL_CHECK(helCloseDescriptor(memory));
			}else{
				auto file = self->fileContext()->getFile(req.fd());
				assert(file && "Illegal FD for VM_MAP");
				address = COFIBER_AWAIT self->vmContext()->mapFile(std::move(file),
						req.rel_offset(), req.size(), native_flags);
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_offset(reinterpret_cast<uintptr_t>(address));

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::VM_REMAP) {
			helix::SendBuffer send_resp;

			auto address = COFIBER_AWAIT self->vmContext()->remapFile(
					reinterpret_cast<void *>(req.address()), req.size(), req.new_size());

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

				DeviceId devnum;
				switch(path.second->getTarget()->getType()) {
				case VfsType::regular:
					resp.set_file_type(managarm::posix::FT_REGULAR); break;
				case VfsType::directory:
					resp.set_file_type(managarm::posix::FT_DIRECTORY); break;
				case VfsType::charDevice:
					resp.set_file_type(managarm::posix::FT_CHAR_DEVICE);
					devnum = path.second->getTarget()->readDevice();
					resp.set_ref_devnum(makedev(devnum.first, devnum.second));
					break;
				case VfsType::blockDevice:
					resp.set_file_type(managarm::posix::FT_BLOCK_DEVICE);
					devnum = path.second->getTarget()->readDevice();
					resp.set_ref_devnum(makedev(devnum.first, devnum.second));
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

			auto result = COFIBER_AWAIT path.second->getTarget()->readSymlink(path.second.get());
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
			
			assert(!(req.flags() & ~(managarm::posix::OF_CREATE
					| managarm::posix::OF_EXCLUSIVE
					| managarm::posix::OF_NONBLOCK
					| managarm::posix::OF_CLOEXEC)));

			ResolveFlags resolve_flags = 0;
			if(req.flags() & managarm::posix::OF_CREATE)
				resolve_flags |= resolveCreate;
			if(req.flags() & managarm::posix::OF_EXCLUSIVE)
				resolve_flags |= resolveExclusive;

			SemanticFlags semantic_flags = 0;
			if(req.flags() & managarm::posix::OF_NONBLOCK)
				semantic_flags |= semanticNonBlock;

			auto file = COFIBER_AWAIT open(self->fsContext()->getRoot(), req.path(),
					resolve_flags, semantic_flags);
			if(file) {
				int fd = self->fileContext()->attachFile(file,
						req.flags() & managarm::posix::OF_CLOEXEC);

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
			if(logRequests)
				std::cout << "posix: CLOSE file descriptor " << req.fd() << std::endl;

			helix::SendBuffer send_resp;

			self->fileContext()->closeFile(req.fd());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP) {
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP2) {
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::FSTAT) {
			auto file = self->fileContext()->getFile(req.fd());
			assert(file && "Illegal FD for FSTAT");
			auto stats = COFIBER_AWAIT file->associatedLink()->getTarget()->getStats();

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			DeviceId devnum;
			switch(file->associatedLink()->getTarget()->getType()) {
			case VfsType::regular:
				resp.set_file_type(managarm::posix::FT_REGULAR); break;
			case VfsType::directory:
				resp.set_file_type(managarm::posix::FT_DIRECTORY); break;
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
		}else if(req.request_type() == managarm::posix::CntReqType::UNLINK) {
			if(logPaths)
				std::cout << "posix: UNLINK path: " << req.path() << std::endl;
			
			helix::SendBuffer send_resp;
			
			auto path = COFIBER_AWAIT resolve(self->fsContext()->getRoot(), req.path());
			if(path.second) {
				auto owner = path.second->getOwner();
				COFIBER_AWAIT owner->unlink(path.second->getName());
			
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
		}else if(req.request_type() == managarm::posix::CntReqType::FD_GET_FLAGS) {
			helix::SendBuffer send_resp;
			
			auto descriptor = self->fileContext()->getDescriptor(req.fd());
			
			int flags = 0;
			if(descriptor.closeOnExec)
				flags |= O_CLOEXEC;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_flags(flags);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SOCKET) {
			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));
			
			if(req.flags() & SOCK_NONBLOCK)
				std::cout << "\e[31mposix: socket(SOCK_NONBLOCK)"
						" is not implemented correctly\e[39m" << std::endl;

			std::shared_ptr<File> file;
			if(req.domain() == AF_UNIX) {
				assert(req.socktype() == SOCK_DGRAM || req.socktype() == SOCK_STREAM
						|| req.socktype() == SOCK_SEQPACKET);
				assert(!req.protocol());

				file = un_socket::createSocketFile();
			}else if(req.domain() == AF_NETLINK) {
				file = nl_socket::createSocketFile();
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SOCKPAIR) {
			helix::SendBuffer send_resp;

			assert(!(req.flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));

			if(req.flags() & SOCK_NONBLOCK)
				std::cout << "\e[31mposix: socketpair(SOCK_NONBLOCK)"
						" is not implemented correctly\e[39m" << std::endl;

			assert(req.domain() == AF_UNIX);
			assert(req.socktype() == SOCK_DGRAM || req.socktype() == SOCK_STREAM
					|| req.socktype() == SOCK_SEQPACKET);
			assert(!req.protocol());

			auto pair = un_socket::createSocketPair();
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SENDMSG) {
			helix::RecvInline recv_data;
			helix::SendBuffer send_resp;
		
			auto &&submit_data = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_data));
			COFIBER_AWAIT submit_data.async_wait();
			HEL_CHECK(recv_data.error());
			
			auto sockfile = self->fileContext()->getFile(req.fd());
			assert(sockfile && "Illegal FD for SENDMSG");

			std::vector<std::shared_ptr<File>> files;
			for(int i = 0; i < req.fds_size(); i++) {
				auto file = self->fileContext()->getFile(req.fds(i));
				assert(sockfile && "Illegal FD for SENDMSG cmsg");
				files.push_back(std::move(file));
			}

			auto bytes_written = COFIBER_AWAIT sockfile->sendMsg(recv_data.data(),
					recv_data.length(), std::move(files));

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_size(bytes_written);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::RECVMSG) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;
			
			auto sockfile = self->fileContext()->getFile(req.fd());
			assert(sockfile && "Illegal FD for SENDMSG");

			std::vector<char> buffer;
			buffer.resize(req.size());
			auto result = COFIBER_AWAIT sockfile->recvMsg(buffer.data(), req.size());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			for(auto &file : std::get<1>(result))
				resp.add_fds(self->fileContext()->attachFile(std::move(file)));

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, buffer.data(), std::get<0>(result)));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CREATE) {
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_ADD) {
			helix::SendBuffer send_resp;

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			assert(epfile && "Illegal FD for EPOLL_ADD");
			assert(file && "Illegal FD for EPOLL_ADD item");

			epoll::addItem(epfile.get(), file.get(), req.flags(), req.cookie());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_MODIFY) {
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_DELETE) {
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_WAIT) {
			if(logRequests)
				std::cout << "posix: EPOLL_WAIT request" << std::endl;

			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;
			
			auto epfile = self->fileContext()->getFile(req.fd());
			assert(epfile && "Illegal FD for EPOLL_WAIT");
			struct epoll_event events[16];
			auto k = COFIBER_AWAIT epoll::wait(epfile.get(), events,
					std::min(req.size(), uint32_t(16)));
			
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, events, k * sizeof(struct epoll_event)));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TIMERFD_CREATE) {
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
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TIMERFD_SETTIME) {
			helix::SendBuffer send_resp;

			auto file = self->fileContext()->getFile(req.fd());
			assert(file && "Illegal FD for TIMERFD_SETTIME");
			timerfd::setTime(file.get(), {req.time_secs(), req.time_nanos()},
					{req.interval_secs(), req.interval_nanos()});

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::SIGNALFD_CREATE) {
			helix::SendBuffer send_resp;
			
			assert(!(req.flags() & ~(managarm::posix::OF_CLOEXEC)));
			
			auto file = createSignalFile();
			auto fd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OF_CLOEXEC);

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
std::shared_ptr<sysfs::Object> eventObject;

struct UeventAttribute : sysfs::Attribute {
	static auto singleton() {
		static UeventAttribute attr;
		return &attr;
	}

private:
	UeventAttribute()
	: sysfs::Attribute("uevent") { }

public:
	virtual std::string show(sysfs::Object *object) override {
		std::cout << "\e[31mposix: uevent files are static\e[39m" << std::endl;
		if(object == cardObject.get()) {
			return std::string{"DEVNAME=dri/card0\n"};
		}else if(object == eventObject.get()) {
			return std::string{"DEVNAME=input/event0\n"
				"MAJOR=13\n"
				"MINOR=64\n"};
		}else{
			throw std::runtime_error("posix: Unexpected object for UeventAttribute::show()");
		}
	}
};

COFIBER_ROUTINE(cofiber::no_future, runInit(), ([] {
	auto devs_object = std::make_shared<sysfs::Object>(nullptr, "devices");
	devs_object->addObject();

	cardObject = std::make_shared<sysfs::Object>(devs_object, "card0");
	cardObject->addObject();
	cardObject->createAttribute(UeventAttribute::singleton());

	eventObject = std::make_shared<sysfs::Object>(devs_object, "event0");
	eventObject->addObject();
	eventObject->createAttribute(UeventAttribute::singleton());

	auto cls_object = std::make_shared<sysfs::Object>(nullptr, "class");
	cls_object->addObject();
	
	auto drm_object = std::make_shared<sysfs::Object>(cls_object, "drm");
	drm_object->addObject();
	drm_object->createSymlink("card0", cardObject);

	auto input_object = std::make_shared<sysfs::Object>(cls_object, "input");
	input_object->addObject();
	input_object->createSymlink("event0", eventObject);
	
	auto devnum_object = std::make_shared<sysfs::Object>(nullptr, "dev");
	devnum_object->addObject();
	
	auto chardev_object = std::make_shared<sysfs::Object>(devnum_object, "char");
	chardev_object->addObject();
	chardev_object->createSymlink("13:64", eventObject);

	COFIBER_AWAIT populateRootView();
	Process::init("sbin/posix-init");
}))

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;

	charRegistry.install(createHeloutDevice());
	block_subsystem::run();
	drm_subsystem::run();
	input_subsystem::run();
	runInit();

	while(true)
		helix::Dispatcher::global().dispatch();
}

