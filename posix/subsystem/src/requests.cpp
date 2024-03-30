#include <linux/netlink.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <helix/timer.hpp>

#include "net.hpp"
#include "nl-socket.hpp"
#include "epoll.hpp"
#include "extern_socket.hpp"
#include "fifo.hpp"
#include "inotify.hpp"
#include "memfd.hpp"
#include "pts.hpp"
#include "requests.hpp"
#include "signalfd.hpp"
#include "sysfs.hpp"
#include "un-socket.hpp"
#include "timerfd.hpp"
#include "eventfd.hpp"
#include "tmp_fs.hpp"

#include <bragi/helpers-std.hpp>
#include <posix.bragi.hpp>

#include "debug-options.hpp"

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
		recv_head.reset();

		managarm::posix::CntRequest req;
		if (preamble.id() == managarm::posix::CntRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::posix::CntRequest>(recv_head);
			if (!o) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			req = *o;
		}

		if(preamble.id() == bragi::message_id<managarm::posix::GetPidRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::GetPidRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			if(logRequests)
				std::cout << "posix: GET_PID" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->pid());

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
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
		}else if(preamble.id() == managarm::posix::WaitIdRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::WaitIdRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: WAIT_ID" << std::endl;

			if(req->flags() & ~(WNOHANG | WCONTINUED | WEXITED | WSTOPPED | WNOWAIT)) {
				std::cout << "posix: WAIT_ID invalid flags: " << req->flags() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->flags() & WSTOPPED)
				std::cout << "\e[31mposix: WAIT_ID flag WSTOPPED is silently ignored\e[39m" << std::endl;

			if(req->flags() & WCONTINUED)
				std::cout << "\e[31mposix: WAIT_ID flag WCONTINUED is silently ignored\e[39m" << std::endl;

			if(req->flags() & WNOWAIT)
				std::cout << "\e[31mposix: WAIT_ID flag WNOWAIT is silently ignored\e[39m" << std::endl;

			TerminationState state;
			int pid;
			if(req->idtype() == P_PID) {
				pid = co_await self->wait(req->id(), req->flags() & WNOHANG, &state);
			} else if(req->idtype() == P_ALL) {
				pid = co_await self->wait(-1, req->flags() & WNOHANG, &state);
			} else {
				std::cout << "\e[31mposix: WAIT_ID idtype other than P_PID and P_ALL are not implemented\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			helix::SendBuffer send_resp;

			managarm::posix::WaitIdResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(pid);
			resp.set_uid(self->findProcess(pid)->uid());

			if(auto byExit = std::get_if<TerminationByExit>(&state); byExit) {
				resp.set_sig_status(W_EXITCODE(byExit->code, 0));
				resp.set_sig_code(CLD_EXITED);
			}else if(auto bySignal = std::get_if<TerminationBySignal>(&state); bySignal) {
				resp.set_sig_status(W_EXITCODE(0, bySignal->signo));
				resp.set_sig_code(CLD_KILLED);
			}else{
				resp.set_sig_status(0);
				resp.set_sig_code(0);
				assert(std::holds_alternative<std::monostate>(state));
			}

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::WAIT) {
			if(logRequests)
				std::cout << "posix: WAIT" << std::endl;

			if(req.flags() & ~(WNOHANG | WUNTRACED | WCONTINUED)) {
				std::cout << "posix: WAIT invalid flags: " << req.flags() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req.flags() & WUNTRACED)
				std::cout << "\e[31mposix: WAIT flag WUNTRACED is silently ignored\e[39m" << std::endl;

			if(req.flags() & WCONTINUED)
				std::cout << "\e[31mposix: WAIT flag WCONTINUED is silently ignored\e[39m" << std::endl;

			TerminationState state;
			auto pid = co_await self->wait(req.pid(), req.flags() & WNOHANG, &state);

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(pid);

			uint32_t mode = 0;
			if(auto byExit = std::get_if<TerminationByExit>(&state); byExit) {
				mode |= W_EXITCODE(byExit->code, 0);
			}else if(auto bySignal = std::get_if<TerminationBySignal>(&state); bySignal) {
				mode |= W_EXITCODE(0, bySignal->signo);
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

			int32_t mode = static_cast<int32_t>(req.mode());
			uint64_t user_time;
			if(mode == RUSAGE_SELF) {
				user_time = stats.userTime;
			}else if(mode == RUSAGE_CHILDREN) {
				user_time = self->accumulatedUsage().userTime;
			}else{
				std::cout << "\e[31mposix: GET_RESOURCE_USAGE mode is not supported\e[39m"
						<< std::endl;
				user_time = 0;
				// TODO: Return an error response.
			}

			helix::SendBuffer send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_ru_user_time(user_time);

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

			if(req->rel_offset() & 0xFFF) {
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

			if(req->flags() & MAP_FIXED_NOREPLACE)
				nativeFlags |= kHelMapFixedNoReplace;
			else if(req->flags() & MAP_FIXED)
				nativeFlags |= kHelMapFixed;

			bool copyOnWrite;
			if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_PRIVATE) {
				copyOnWrite = true;
			}else if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_SHARED) {
				copyOnWrite = false;
			}else{
				throw std::runtime_error("posix: Handle illegal flags in VM_MAP");
			}

			uintptr_t hint = req->address_hint();

			frg::expected<Error, void *> result;
			if(req->flags() & MAP_ANONYMOUS) {
				assert(!req->rel_offset());

				if(copyOnWrite) {
					result = co_await self->vmContext()->mapFile(hint,
							{}, nullptr,
							0, req->size(), true, nativeFlags);
				}else{
					HelHandle handle;
					HEL_CHECK(helAllocateMemory(req->size(), 0, nullptr, &handle));

					result = co_await self->vmContext()->mapFile(hint,
							helix::UniqueDescriptor{handle}, nullptr,
							0, req->size(), false, nativeFlags);
				}
			}else{
				auto file = self->fileContext()->getFile(req->fd());
				assert(file && "Illegal FD for VM_MAP");
				auto memory = co_await file->accessMemory();
				assert(memory);
				result = co_await self->vmContext()->mapFile(hint,
						std::move(memory), std::move(file),
						req->rel_offset(), req->size(), copyOnWrite, nativeFlags);
			}

			if(!result) {
				assert(result.error() == Error::alreadyExists || result.error() == Error::noMemory);
				if(result.error() == Error::alreadyExists)
					co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				else if(result.error() == Error::noMemory)
					co_await sendErrorResponse(managarm::posix::Errors::NO_MEMORY);
				continue;
			}

			void *address = result.unwrap();

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
					self->fsContext()->getWorkingDirectory(), req->target_path(), self.get());
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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
						self->fsContext()->getWorkingDirectory(), req->path(), self.get());
				if(!sourceResult) {
					if(sourceResult.error() == protocols::fs::Error::fileNotFound) {
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
					self->fsContext()->getWorkingDirectory(), req.path(), self.get());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
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
					self->fsContext()->getWorkingDirectory(), req.path(), self.get());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
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
		}else if(preamble.id() == managarm::posix::AccessAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::AccessAtRequest>(recv_head, tail);

			if(!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: ACCESSAT " << req->path() << std::endl;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(req->flags()) {
				if(req->flags() & AT_SYMLINK_NOFOLLOW) {
					std::cout << "posix: ACCESSAT flag handling AT_SYMLINK_NOFOLLOW is unimplemented" << std::endl;
				} else if(req->flags() & AT_EACCESS) {
					std::cout << "posix: ACCESSAT flag handling AT_EACCESS is unimplemented" << std::endl;
				} else {
					std::cout << "posix: ACCESSAT unknown flag is unimplemented: " << req->flags() << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
			}

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

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
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

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::MkdirAtRequest::message_id) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::MkdirAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: MKDIRAT " << req->path() << std::endl;

			if(!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

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
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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

			if(!resolver.hasComponent()) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			auto parent = resolver.currentLink()->getTarget();
			auto existsResult = co_await parent->getLink(resolver.nextComponent());
			assert(existsResult);
			auto exists = existsResult.value();
			if(exists) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			auto result = co_await parent->mkdir(resolver.nextComponent());

			if(auto error = std::get_if<Error>(&result); error) {
				assert(*error == Error::illegalOperationTarget);
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
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
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(resolvePrefix | resolveNoTrailingSlash);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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
			if((co_await parent->getLink(resolver.nextComponent())).unwrap()) {
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
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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
					relative_to, req->target_path(), self.get());
			auto new_resolveResult = co_await new_resolver.resolve(
					resolvePrefix | resolveNoTrailingSlash);
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
					relativeTo, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(
					resolvePrefix | resolveNoTrailingSlash);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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
				if(*error == Error::alreadyExists) {
					co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
					continue;
				} else {
					assert(*error == Error::illegalOperationTarget);
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
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
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::isDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
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

			// TODO: Add resolveNoTrailingSlash if source is not a directory?
			PathResolver new_resolver;
			new_resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->target_path(), self.get());
			auto new_resolveResult = co_await new_resolver.resolve(resolvePrefix);
			if(!new_resolveResult) {
				if(new_resolveResult.error() == protocols::fs::Error::isDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
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
						relative_to, req->path(), self.get());

				ResolveFlags resolveFlags = 0;
				if (req->flags() & AT_SYMLINK_NOFOLLOW)
				    resolveFlags |= resolveDontFollow;

				auto resolveResult = co_await resolver.resolve(resolveFlags);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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

			// This catches cases where associatedLink is called on a file, but the file doesn't implement that.
			// Instead of blowing up, return ENOENT.
			if(target_link == nullptr) {
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
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
					relative_to, req->path(), self.get());

				auto resolveResult = co_await resolver.resolve();

				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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
						relativeTo, req->path(), self.get());
				auto resolveResult = co_await resolver.resolve();
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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
		}else if(preamble.id() == bragi::message_id<managarm::posix::ReadlinkAtRequest>) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recvTail.error());

			auto req = bragi::parse_head_tail<managarm::posix::ReadlinkAtRequest>(recv_head, tail);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests || logPaths)
				std::cout << "posix: READLINKAT path: " << req->path() << std::endl;

			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

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

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get(), resolveDontFollow);
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
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

			auto result = co_await path.second->getTarget()->readSymlink(path.second.get(), self.get());
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
					| managarm::posix::OpenFlags::OF_PATH
					| managarm::posix::OpenFlags::OF_NOCTTY
					| managarm::posix::OpenFlags::OF_APPEND))) {
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

			if(req->flags() & managarm::posix::OpenFlags::OF_APPEND)
				semantic_flags |= semanticAppend;

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
					relative_to, req->path(), self.get());
			if(req->flags() & managarm::posix::OpenFlags::OF_CREATE) {
				auto resolveResult = co_await resolver.resolve(
						resolvePrefix | resolveNoTrailingSlash);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::isDirectory) {
						// TODO: Verify additional constraints for sending EISDIR.
						co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
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
					auto node = co_await directory->superblock()->createRegular(self.get());
					if (!node) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					}
					auto chmodResult = co_await node->chmod(req->mode());
					if (chmodResult != Error::success) {
						std::cout << "posix: chmod failed when creating file for OpenAtRequest!" << std::endl;
						co_await sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
						continue;
					}
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
					if(resolveResult.error() == protocols::fs::Error::isDirectory) {
						// TODO: Verify additional constraints for sending EISDIR.
						co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
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
							co_await sendErrorResponse(managarm::posix::Errors::NO_BACKING_DEVICE);
							continue;
						} else if(fileResult.error() == Error::illegalArguments) {
							co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
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

			if(file->isTerminal() &&
				!(req->flags() & managarm::posix::OpenFlags::OF_NOCTTY) &&
				self->pgPointer()->getSession()->getSessionId() == (pid_t)self->pid() &&
				self->pgPointer()->getSession()->getControllingTerminal() == nullptr) {
				// POSIX 1003.1-2017 11.1.3
				auto cts = co_await file->getControllingTerminal();
				if(!cts) {
					std::cout << "posix: Unable to get controlling terminal (" << (int)cts.error() << ")" << std::endl;
				} else {
					cts.value()->assignSessionOf(self.get());
				}
			}

			if(req->flags() & managarm::posix::OpenFlags::OF_TRUNC) {
				auto result = co_await file->truncate(0);
				assert(result || result.error() == protocols::fs::Error::illegalOperationTarget);
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

			auto closeErr = self->fileContext()->closeFile(req->fd());

			if(closeErr != Error::success) {
				if(closeErr == Error::noSuchFile) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				} else {
					std::cout << "posix: Unhandled error returned from closeFile" << std::endl;
					break;
				}
			}

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

			auto file = self->fileContext()->getFile(req.fd());
			if(!file) {
			    co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
			    continue;
			}

			auto ttynameResult = co_await file->ttyname();
			if(!ttynameResult) {
			    assert(ttynameResult.error() == Error::notTerminal);
			    co_await sendErrorResponse(managarm::posix::Errors::NOT_A_TTY);
			    continue;
			}

			resp.set_path(ttynameResult.value());
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::GETCWD) {
			if(logRequests)
				std::cout << "posix: GETCWD" << std::endl;

			std::string path = self->fsContext()->getWorkingDirectory().getPath(
					self->fsContext()->getRoot());

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
					relative_to, req->path(), self.get());

			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::isDirectory) {
					// TODO: Only when AT_REMOVEDIR is not specified, fix this when flag handling is implemented.
					co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
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
					req->path(), self.get());

			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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
		}else if(preamble.id() == bragi::message_id<managarm::posix::IoctlFioclexRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::IoctlFioclexRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: FIOCLEX" << std::endl;

			if(self->fileContext()->setDescriptor(req->fd(), true) != Error::success) {
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
				if(req.sig_handler() == uintptr_t(SIG_DFL)) {
					handler.disposition = SignalDisposition::none;
				}else if(req.sig_handler() == uintptr_t(SIG_IGN)) {
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
					handler.flags |= signalOnStack;
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
			if(saved_handler.flags & signalOnStack)
				saved_flags |= SA_ONSTACK;

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_flags(saved_flags);
			resp.set_sig_mask(saved_handler.mask);
			if(saved_handler.disposition == SignalDisposition::handle) {
				resp.set_sig_handler(saved_handler.handlerIp);
				resp.set_sig_restorer(saved_handler.restorerIp);
			}else if(saved_handler.disposition == SignalDisposition::none) {
				resp.set_sig_handler((uint64_t)SIG_DFL);
			}else{
				assert(saved_handler.disposition == SignalDisposition::ignore);
				resp.set_sig_handler((uint64_t)SIG_IGN);
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

			managarm::posix::SvrResponse resp;

			if(self->pgPointer()->getSession()->getSessionId() == self->pid()) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
				continue;
			}

			auto session = TerminalSession::initializeNewSession(self.get());

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

				file = un_socket::createSocketFile(req->flags() & SOCK_NONBLOCK, req->socktype());
			}else if(req->domain() == AF_NETLINK) {
				assert(req->socktype() == SOCK_RAW || req->socktype() == SOCK_DGRAM);
				// NL_ROUTE gets handled by the netserver.
				if(req->protocol() == NETLINK_ROUTE)
					file = co_await extern_socket::createSocket(
						co_await net::getNetLane(),
						req->domain(),
						req->socktype(), req->protocol(),
						req->flags() & SOCK_NONBLOCK
					);
				else
					file = nl_socket::createSocketFile(req->protocol(), req->flags() & SOCK_NONBLOCK);
			} else if (req->domain() == AF_INET) {
				file = co_await extern_socket::createSocket(
					co_await net::getNetLane(),
					req->domain(),
					req->socktype(), req->protocol(),
					req->flags() & SOCK_NONBLOCK);
			}else{
				std::cout << "posix: SOCKET: Handle unknown protocols families, this is: " << req->domain() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
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

			if(req->domain() != AF_UNIX) {
				std::cout << "\e[31mposix: socketpair() with domain " << req->domain() <<
						" is not implemented correctly\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ADDRESS_FAMILY_NOT_SUPPORTED);
				continue;
			}
			if(req->socktype() != SOCK_DGRAM && req->socktype() != SOCK_STREAM
					&& req->socktype() != SOCK_SEQPACKET) {
				std::cout << "\e[31mposix: socketpair() with socktype " << req->socktype() <<
						" is not implemented correctly\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}
			if(req->protocol() && req->protocol() != PF_UNSPEC) {
				std::cout << "\e[31mposix: socketpair() with protocol " << req->protocol() <<
						" is not implemented correctly\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::PROTOCOL_NOT_SUPPORTED);
				continue;
			}

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

			// Since file descriptors may appear multiple times in a poll() call,
			// we need to de-duplicate them here.
			std::unordered_map<int, unsigned int> fdsToEvents;

			auto epfile = epoll::createFile();
			assert(req.fds_size() == req.events_size());

			bool errorOut = false;
			for(size_t i = 0; i < req.fds_size(); i++) {
				auto [mapIt, inserted] = fdsToEvents.insert({req.fds(i), 0});
				if(!inserted)
					continue;

				auto file = self->fileContext()->getFile(req.fds(i));
				if(!file) {
					// poll() is supposed to fail on a per-FD basis.
					mapIt->second = POLLNVAL;
					continue;
				}
				auto locked = file->weakFile().lock();
				assert(locked);

				// Translate POLL events to EPOLL events.
				if(req.events(i) & ~(POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP
						| POLLNVAL | POLLWRNORM)) {
					std::cout << "\e[31mposix: Unexpected events for poll()\e[39m" << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					errorOut = true;
					break;
				}

				unsigned int mask = 0;
				if(req.events(i) & POLLIN) mask |= EPOLLIN;
				if(req.events(i) & POLLOUT) mask |= EPOLLOUT;
				if(req.events(i) & POLLWRNORM) mask |= EPOLLOUT;
				if(req.events(i) & POLLPRI) mask |= EPOLLPRI;
				if(req.events(i) & POLLRDHUP) mask |= EPOLLRDHUP;
				if(req.events(i) & POLLERR) mask |= EPOLLERR;
				if(req.events(i) & POLLHUP) mask |= EPOLLHUP;

				// addItem() can fail with EEXIST but we check for duplicate FDs above
				// so that cannot happen here.
				Error ret = epoll::addItem(epfile.get(), self.get(),
						std::move(locked), req.fds(i), mask, req.fds(i));
				assert(ret == Error::success);
			}
			if(errorOut)
				continue;

			struct epoll_event events[16];
			size_t k;
			if(req.timeout() < 0) {
				k = co_await epoll::wait(epfile.get(), events, 16);
			}else if(!req.timeout()) {
				// Do not bother to set up a timer for zero timeouts.
				async::cancellation_event cancel_wait;
				cancel_wait.cancel();
				k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
			}else{
				assert(req.timeout() > 0);
				async::cancellation_event cancel_wait;
				helix::TimeoutCancellation timer{static_cast<uint64_t>(req.timeout()), cancel_wait};
				k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
				co_await timer.retire();
			}

			// Assigned the returned events to each FD.
			for(size_t j = 0; j < k; ++j) {
				auto it = fdsToEvents.find(events[j].data.fd);
				assert(it != fdsToEvents.end());

				// Translate EPOLL events back to POLL events.
				assert(!it->second);
				if(events[j].events & EPOLLIN) it->second |= POLLIN;
				if(events[j].events & EPOLLOUT) it->second |= POLLOUT;
				if(events[j].events & EPOLLPRI) it->second |= POLLPRI;
				if(events[j].events & EPOLLRDHUP) it->second |= POLLRDHUP;
				if(events[j].events & EPOLLERR) it->second |= POLLERR;
				if(events[j].events & EPOLLHUP) it->second |= POLLHUP;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			for(size_t i = 0; i < req.fds_size(); ++i) {
				auto it = fdsToEvents.find(req.fds(i));
				assert(it != fdsToEvents.end());
				resp.add_events(it->second);
			}

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
			Error ret = epoll::addItem(epfile.get(), self.get(),
					std::move(locked), req.newfd(), req.flags(), req.cookie());
			if(ret == Error::alreadyExists) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}
			assert(ret == Error::success);

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

			Error ret = epoll::modifyItem(epfile.get(), file.get(), req.newfd(),
					req.flags(), req.cookie());
			if(ret == Error::noSuchFile) {
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
			}
			assert(ret == Error::success);

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

			Error ret = epoll::deleteItem(epfile.get(), file.get(), req.newfd(), req.flags());
			if(ret == Error::noSuchFile) {
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
			}
			assert(ret == Error::success);

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
			if(req.timeout() < 0) {
				k = co_await epoll::wait(epfile.get(), events,
						std::min(req.size(), uint32_t(16)));
			}else if(!req.timeout()) {
				// Do not bother to set up a timer for zero timeouts.
				async::cancellation_event cancel_wait;
				cancel_wait.cancel();
				k = co_await epoll::wait(epfile.get(), events,
						std::min(req.size(), uint32_t(16)), cancel_wait);
			}else{
				assert(req.timeout() > 0);
				async::cancellation_event cancel_wait;
				helix::TimeoutCancellation timer{static_cast<uint64_t>(req.timeout()), cancel_wait};
				k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
				co_await timer.retire();
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

			if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC
					| managarm::posix::OpenFlags::OF_NONBLOCK)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto file = createSignalFile(req.sigset(),
					req.flags() & managarm::posix::OpenFlags::OF_NONBLOCK);
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

			assert(!(req->flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC | managarm::posix::OpenFlags::OF_NONBLOCK)));

			// TODO: Implement blocking reads
			if(!(req->flags() & managarm::posix::OpenFlags::OF_NONBLOCK))
				std::cout << "posix: INOTIFY_CREATE doesn't do blocking reads" << std::endl;

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
					self->fsContext()->getWorkingDirectory(), req->path(), self.get());
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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

			if (req->flags() & ~(managarm::posix::EventFdFlags::CLOEXEC
					| managarm::posix::EventFdFlags::NONBLOCK
					| managarm::posix::EventFdFlags::SEMAPHORE)) {
				std::cout << "posix: invalid flag specified" << std::endl;
				std::cout << "posix: flags specified: " << req->flags() << std::endl;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				auto file = eventfd::createFile(req->initval(),
					req->flags() & managarm::posix::EventFdFlags::NONBLOCK,
					req->flags() & managarm::posix::EventFdFlags::SEMAPHORE);
				auto fd = self->fileContext()->attachFile(file,
						req->flags() & managarm::posix::EventFdFlags::CLOEXEC);

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

			// TODO: Add resolveNoTrailingSlash if not making a directory?
			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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

			// TODO: Verify the proper error return here.
			if(type == VfsType::charDevice || type == VfsType::blockDevice) {
				dev.first = major(req->device());
				dev.second = minor(req->device());

				auto result = co_await parent->mkdev(resolver.nextComponent(), type, dev);
				if(!result) {
					if(result.error() == Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
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
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
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
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
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
		}else if(preamble.id() == managarm::posix::GetPgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetPgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_PGID" << std::endl;

			std::shared_ptr<Process> target;
			if(req->pid()) {
				target = Process::findProcess(req->pid());
				if(!target) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			} else {
				target = self;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(target->pgPointer()->getHull()->getPid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::SetPgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetPgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SET_PGID" << std::endl;

			std::shared_ptr<Process> target;
			if(req->pid()) {
				target = Process::findProcess(req->pid());
				if(!target) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			} else {
				target = self;
			}

			if(target->pgPointer()->getSession() != self->pgPointer()->getSession()) {
				co_await sendErrorResponse(managarm::posix::Errors::INSUFFICIENT_PERMISSION);
				continue;
			}

			// We can't change the process group ID of the session leader
			if(target->pid() == target->pgPointer()->getSession()->getSessionId()) {
				co_await sendErrorResponse(managarm::posix::Errors::INSUFFICIENT_PERMISSION);
				continue;
			}

			if(target->getParent()->pid() == self->pid()) {
				if(target->didExecute()) {
					co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
					continue;
				}
			}

			std::shared_ptr<ProcessGroup> group;
			// If pgid is 0, we're going to set it to the calling process's pid, use the target group.
			if(req->pgid()) {
				group = target->pgPointer()->getSession()->getProcessGroupById(req->pgid());
			} else {
				group = target->pgPointer();
			}

			if(group) {
				// Found, do permission checking and join
				group->reassociateProcess(target.get());
			} else {
				// Not found, making it if pgid and pid match, or if pgid is 0, indicating that we should make one
				if(target->pid() == req->pgid() || !req->pgid()) {
					target->pgPointer()->getSession()->spawnProcessGroup(target.get());
				} else {
					// Doesn't exists, and not requesting a make?
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::GetSidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetSidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_SID on pid " << req->pid() << std::endl;

			std::shared_ptr<Process> target;
			if(req->pid()) {
				target = Process::findProcess(req->pid());
				if(!target) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			} else {
				target = self;
			}
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(target->pgPointer()->getSession()->getSessionId());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
		}else if(preamble.id() == managarm::posix::MemFdCreateRequest::message_id) {
			managarm::posix::SvrResponse resp;
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::MemFdCreateRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: MEMFD_CREATE " << req->name() << std::endl;

			if(req->flags() & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto link = SpecialLink::makeSpecialLink(VfsType::regular, 0777);
			auto memFile = smarter::make_shared<MemoryFile>(nullptr, link, (req->flags() & MFD_ALLOW_SEALING) == true);
			MemoryFile::serve(memFile);
			auto file = File::constructHandle(std::move(memFile));

			int flags = 0;

			if(req->flags() & MFD_CLOEXEC) {
				flags |= managarm::posix::OpenFlags::OF_CLOEXEC;
			}

			int fd = self->fileContext()->attachFile(file, flags);

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
		}else if(preamble.id() == managarm::posix::SetAffinityRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			auto req = bragi::parse_head_tail<managarm::posix::SetAffinityRequest>(recv_head, tail);
			
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: SET_AFFINITY" << std::endl;

			auto handle = self->threadDescriptor().getHandle();

			if(self->pid() != req->pid()) {
				// TODO: permission checking
				auto target_process = self->findProcess(req->pid());
				if(target_process == nullptr) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
				handle = target_process->threadDescriptor().getHandle();
			}

			HelError e = helSetAffinity(handle, req->mask().data(), req->mask().size());

			if(e == kHelErrIllegalArgs) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			} else if(e != kHelErrNone) {
				std::cout << "posix: SET_AFFINITY hel call returned unexpected error: " << e << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
		}else if(preamble.id() == managarm::posix::GetAffinityRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetAffinityRequest>(recv_head);
			
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if(logRequests)
				std::cout << "posix: GET_AFFINITY" << std::endl;

			if(!req->size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			std::vector<uint8_t> affinity(req->size());

			auto handle = self->threadDescriptor().getHandle();

			if(self->pid() != req->pid()) {
				// TODO: permission checking
				auto target_process = self->findProcess(req->pid());
				if(target_process == nullptr) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
				handle = target_process->threadDescriptor().getHandle();
			}

			size_t actual_size;
			HelError e = helGetAffinity(handle, affinity.data(), req->size(), &actual_size);

			if(e == kHelErrBufferTooSmall) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			} else if(e != kHelErrNone) {
				std::cout << "posix: GET_AFFINITY hel call returned unexpected error: " << e << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->pid());

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(affinity.data(), affinity.size())
			);
			HEL_CHECK(sendResp.error());
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
