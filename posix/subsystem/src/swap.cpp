#include "swap.hpp"

#include <bragi/helpers-std.hpp>
#include <protocols/mbus/client.hpp>

#include "swap.bragi.hpp"

namespace {

helix::UniqueLane swapserverLane;
helix::UniqueDescriptor swapSpaceHandle;

} // namespace

async::result<void> enumerateSwapserver() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "swapserver"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	swapserverLane = (co_await entity.getRemoteLane()).unwrap();

    managarm::swap::GetSwapSpaceRequest req;
    auto [offer, sendHead, recvHead, pullDesc] = co_await helix_ng::exchangeMsgs(
        swapserverLane,
        helix_ng::offer(
            helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
            helix_ng::recvInline(),
            helix_ng::pullDescriptor(kHelRightAssign)
        )
    );
    HEL_CHECK(offer.error());
    HEL_CHECK(sendHead.error());
    HEL_CHECK(recvHead.error());

    auto res = *bragi::parse_head_only<managarm::swap::GetSwapSpaceResponse>(recvHead);
    assert(res.error() == managarm::swap::Errors::SUCCESS);

    HEL_CHECK(pullDesc.error());
    swapSpaceHandle = pullDesc.descriptor();
}

helix::BorrowedLane getSwapServerLane() {
    return swapserverLane;
}

helix::BorrowedDescriptor getSwapSpaceHandle() {
    return swapSpaceHandle;
}
