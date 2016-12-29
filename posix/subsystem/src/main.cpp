
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <iomanip>
#include <iostream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "common.hpp"
#include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "exec.hpp"
#include "devices/helout.hpp"
//FIXME #include "dev_fs.hpp"
//FIXME #include "pts_fs.hpp"
//FIXME #include "sysfile_fs.hpp"
//FIXME #include "extern_fs.hpp"
#include <posix.pb.h>

bool traceRequests = false;

//FIXME: helx::EventHub eventHub = helx::EventHub::create();
//FIXME: helx::Client mbusConnect;
//FIXME: helx::Pipe ldServerPipe;
//FIXME: helx::Pipe mbusPipe;

cofiber::no_future serve(std::shared_ptr<Process> self, helix::UniqueDescriptor p);

COFIBER_ROUTINE(cofiber::no_future, observe(std::shared_ptr<Process> self,
		helix::BorrowedDescriptor thread), ([=] {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Observe<M> observe;
		helix::submitObserve(thread, &observe, helix::Dispatcher::global());
		COFIBER_AWAIT(observe.future());
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
			std::cout << "execve supercall" << std::endl;
			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, &gprs));

			std::string path;
			path.resize(gprs[7]);
			HEL_CHECK(helLoadForeign(self->vmContext()->getSpace().getHandle(),
					gprs[6], gprs[7], &path[0]));

			Process::exec(self, path);
		}else if(observe.observation() == kHelObserveBreakpoint) {
			uintptr_t pcrs[2];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsProgram, &pcrs));

			uintptr_t gprs[15];
			HEL_CHECK(helLoadRegisters(thread.getHandle(), kHelRegsGeneral, gprs));

			printf("\e[35mBreakpoint fault\n");
			printf("rax: %.16lx, rbx: %.16lx, rcx: %.16lx\n", gprs[0], gprs[1], gprs[2]);
			printf("rdx: %.16lx, rdi: %.16lx, rsi: %.16lx\n", gprs[3], gprs[4], gprs[5]);
			printf(" r8: %.16lx,  r9: %.16lx, r10: %.16lx\n", gprs[6], gprs[7], gprs[8]);
			printf("r11: %.16lx, r12: %.16lx, r13: %.16lx\n", gprs[9], gprs[10], gprs[11]);
			printf("r14: %.16lx, r15: %.16lx, rbp: %.16lx\n", gprs[12], gprs[13], gprs[14]);
			printf("rip: %.16lx, rsp: %.16lx\n", pcrs[0], pcrs[1]);
			printf("\e[39m");
		}else{
			throw std::runtime_error("Unexpected observation");
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serve(std::shared_ptr<Process> self,
		helix::UniqueDescriptor p), ([self, lane = std::move(p)] {
	using M = helix::AwaitMechanism;

	observe(self, lane);

	while(true) {
		helix::Accept<M> accept;
		helix::RecvBuffer<M> recv_req;

		char buffer[256];
		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req, buffer, 256)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::posix::CntRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.request_type() == managarm::posix::CntReqType::ACCESS) {
			helix::SendBuffer<M> send_resp;

			auto path = COFIBER_AWAIT resolve(req.path());
			if(path.second) {
				std::cout << "Resolve succeeded" << std::endl;
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto ser = resp.SerializeAsString();
				helix::submitAsync(conversation, {
					helix::action(&send_resp, ser.data(), ser.size()),
				}, helix::Dispatcher::global());
				
				COFIBER_AWAIT send_resp.future();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				helix::submitAsync(conversation, {
					helix::action(&send_resp, ser.data(), ser.size()),
				}, helix::Dispatcher::global());
				
				COFIBER_AWAIT send_resp.future();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::OPEN) {
			helix::SendBuffer<M> send_resp;

			auto file = COFIBER_AWAIT open(req.path());
			if(file) {
				int fd = self->fileContext()->attachFile(file);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd);

				auto ser = resp.SerializeAsString();
				helix::submitAsync(conversation, {
					helix::action(&send_resp, ser.data(), ser.size()),
				}, helix::Dispatcher::global());
				
				COFIBER_AWAIT send_resp.future();
				HEL_CHECK(send_resp.error());
			}else{
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				helix::submitAsync(conversation, {
					helix::action(&send_resp, ser.data(), ser.size()),
				}, helix::Dispatcher::global());
				
				COFIBER_AWAIT send_resp.future();
				HEL_CHECK(send_resp.error());
			}
		}else if(req.request_type() == managarm::posix::CntReqType::CLOSE) {
			self->fileContext()->closeFile(req.fd());

			helix::SendBuffer<M> send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size()),
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::DUP2) {
			std::cout << "dup2: " << req.fd() << " -> " << req.newfd() << std::endl;
			auto file = self->fileContext()->getFile(req.fd());
			self->fileContext()->attachFile(req.newfd(), file);

			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_passthrough;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_passthrough, getPassthroughLane(file))
			}, helix::Dispatcher::global());
			
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_passthrough.future();
			
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_passthrough.error());
		}else{
			std::cout << "posix: Illegal request" << std::endl;
			helix::SendBuffer<M> send_resp;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size())
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}
	}
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

struct ExternDevice : Device {
	static VfsType getType(std::shared_ptr<Device>) {
		return VfsType::charDevice;
	}

	static std::string getName(std::shared_ptr<Device>) {
		return "sda0";
	}

	static COFIBER_ROUTINE(FutureMaybe<std::shared_ptr<File>>,
			open(std::shared_ptr<Device> device), ([=] {
		(void)device;
		assert(!"Fix this");
	}))

	static const DeviceOperations operations;

	ExternDevice()
	: Device(&operations) { }
};

const DeviceOperations ExternDevice::operations{
	&ExternDevice::getType,
	&ExternDevice::getName,
	&ExternDevice::open
};

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity device), ([=] {
	using M = helix::AwaitMechanism;

	auto lane = helix::UniqueLane(COFIBER_AWAIT device.bind());
	auto device = std::make_shared<ExternDevice>();
	deviceManager.install(device);
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.devtype", "block")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "posix: Detected UNIX device" << std::endl;
			bindDevice(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected event type");
		}
	});
}))

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;

	deviceManager.install(createHeloutDevice());
	observeDevices();

	Process::init("posix-init");

	while(true)
		helix::Dispatcher::global().dispatch();
}

