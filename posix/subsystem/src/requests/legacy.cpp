#include <format>
#include <print>
#include <set>
#include <limits.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <frg/scope_exit.hpp>

#include "common.hpp"
#include "../epoll.hpp"

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::CntRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process>, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	frg::scope_exit traceOnExit{[&] {
		if(posix::ostContext.isActive()) {
			posix::ostContext.emit(
				posix::ostEvtLegacyRequest,
				posix::ostAttrRequest(req.request_type()),
				posix::ostAttrTime(timer.elapsed())
			);
		}
	}};

	std::cout << "posix: Illegal request" << std::endl;

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());

	co_return {};
}

} // namespace requests
