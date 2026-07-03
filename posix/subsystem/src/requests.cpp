#include <bragi/helpers-std.hpp>
#include <posix.bragi.hpp>

#include <core/dispatch.hpp>

#include "debug-options.hpp"
#include "requests/common.hpp"

async::result<void> serveRequests(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	async::cancellation_token cancellation = generation->cancelServe;

	async::cancellation_callback cancel_callback{cancellation, [&] {
		HEL_CHECK(helShutdownLane(self->posixLane().getHandle()));
	}};

	while(true) {
		auto res = co_await dispatchRequest<
			// From fd.cpp
			managarm::posix::Dup2Request,
			managarm::posix::IsTtyRequest,
			managarm::posix::IoctlFioclexRequest,
			managarm::posix::CloseRequest,
			managarm::posix::EpollCallRequest,
			managarm::posix::EpollCtlRequest,
			managarm::posix::EpollWaitRequest,
			managarm::posix::FdGetFlagsRequest,
			managarm::posix::FdSetFlagsRequest,
			// From filesystem.cpp
			managarm::posix::ChrootRequest,
			managarm::posix::ChdirRequest,
			managarm::posix::AccessAtRequest,
			managarm::posix::MkdirAtRequest,
			managarm::posix::MkfifoAtRequest,
			managarm::posix::LinkAtRequest,
			managarm::posix::SymlinkAtRequest,
			managarm::posix::ReadlinkAtRequest,
			managarm::posix::RenameAtRequest,
			managarm::posix::UnlinkAtRequest,
			managarm::posix::RmdirRequest,
			managarm::posix::FstatAtRequest,
			managarm::posix::FstatfsRequest,
			managarm::posix::FchmodAtRequest,
			managarm::posix::FchownAtRequest,
			managarm::posix::UtimensAtRequest,
			managarm::posix::OpenAtRequest,
			managarm::posix::MknodAtRequest,
			managarm::posix::UmaskRequest,
			managarm::posix::GetCwdRequest,
			managarm::posix::TtyNameRequest,
			// From special-files.cpp
			managarm::posix::InotifyCreateRequest,
			managarm::posix::InotifyAddRequest,
			managarm::posix::InotifyRmRequest,
			managarm::posix::EventfdCreateRequest,
			managarm::posix::TimerFdCreateRequest,
			managarm::posix::TimerFdSetRequest,
			managarm::posix::TimerFdGetRequest,
			managarm::posix::PidfdOpenRequest,
			managarm::posix::PidfdSendSignalRequest,
			managarm::posix::PidfdGetPidRequest,
			managarm::posix::EpollCreateRequest,
			managarm::posix::PipeCreateRequest,
			managarm::posix::SignalfdCreateRequest,
			// From memory.cpp
			managarm::posix::VmMapRequest,
			managarm::posix::VmRemapRequest,
			managarm::posix::VmProtectRequest,
			managarm::posix::VmUnmapRequest,
			managarm::posix::MemFdCreateRequest,
			// From uid-gid.cpp
			managarm::posix::GetPidRequest,
			managarm::posix::GetPpidRequest,
			managarm::posix::GetUidRequest,
			managarm::posix::SetUidRequest,
			managarm::posix::GetEuidRequest,
			managarm::posix::SetEuidRequest,
			managarm::posix::GetGidRequest,
			managarm::posix::GetEgidRequest,
			managarm::posix::SetGidRequest,
			managarm::posix::SetEgidRequest,
			managarm::posix::GetGroupsRequest,
			managarm::posix::SetGroupsRequest,
			// From process.cpp
			managarm::posix::WaitIdRequest,
			managarm::posix::SetAffinityRequest,
			managarm::posix::GetAffinityRequest,
			managarm::posix::GetPgidRequest,
			managarm::posix::SetPgidRequest,
			managarm::posix::GetSidRequest,
			managarm::posix::SetSidRequest,
			managarm::posix::ParentDeathSignalRequest,
			managarm::posix::ProcessDumpableRequest,
			managarm::posix::SetResourceLimitRequest,
			managarm::posix::SigactionRequest,
			// From socket.cpp
			managarm::posix::NetserverRequest,
			managarm::posix::SocketRequest,
			managarm::posix::SockpairRequest,
			managarm::posix::AcceptRequest,
			// From system.cpp
			managarm::posix::RebootRequest,
			managarm::posix::MountRequest,
			managarm::posix::SysconfRequest,
			managarm::posix::GetMemoryInformationRequest,
			// From timer.cpp
			managarm::posix::SetIntervalTimerRequest,
			managarm::posix::TimerCreateRequest,
			managarm::posix::TimerSetRequest,
			managarm::posix::TimerGetRequest,
			managarm::posix::TimerDeleteRequest,
			// From legacy.cpp
			managarm::posix::CntRequest
		>(self->posixLane(), requests::HandleRequest{}, self, generation);

		if(!res) {
			if(res.error() == DispatchError::shutdown)
				break;
			std::cout << "posix: dispatch error" << std::endl;
			continue;
		}
	}

	if(logCleanup)
		std::cout << "\e[33mposix: Exiting serveRequests()\e[39m" << std::endl;
	generation->requestsDone.raise();
}
