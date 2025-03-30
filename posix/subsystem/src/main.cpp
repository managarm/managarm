#include <memory>

#include <protocols/mbus/client.hpp>

#include "net.hpp"
#include <core/clock.hpp>
#include "drvcore.hpp"
#include "devices/full.hpp"
#include "devices/helout.hpp"
#include "devices/kmsg.hpp"
#include "devices/null.hpp"
#include "devices/random.hpp"
#include "devices/urandom.hpp"
#include "devices/zero.hpp"
#include "fuse.hpp"
#include "pts.hpp"
#include "requests.hpp"
#include "subsystem/acpi.hpp"
#include "subsystem/block.hpp"
#include "subsystem/drm.hpp"
#include "subsystem/generic.hpp"
#include "subsystem/input.hpp"
#include "subsystem/net.hpp"
#include "subsystem/nvme.hpp"
#include "subsystem/pci.hpp"
#include "subsystem/power_supply.hpp"
#include "subsystem/usb/usb.hpp"
#include "subsystem/usbmisc.hpp"
#include "observations.hpp"
#include "ostrace.hpp"

#include <bragi/helpers-std.hpp>
#include <kerncfg.bragi.hpp>
#include <posix.bragi.hpp>

#include "debug-options.hpp"

std::map<
	std::array<char, 16>,
	Process *
> globalCredentialsMap;

std::shared_ptr<Process> findProcessWithCredentials(helix_ng::CredentialsView credentials) {
	std::array<char, 16> creds;
	memcpy(creds.data(), credentials.data(), 16);
	globalCredentialsMap.at(creds)->shared_from_this();
	return globalCredentialsMap.at(creds)->shared_from_this();
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
	auto res = globalCredentialsMap.insert({creds, self.get()});
	assert(res.second);

	co_await async::when_all(
		observeThread(self, generation),
		serveSignals(self, generation),
		serveRequests(self, generation)
	);

	std::erase_if(globalCredentialsMap, [&](const auto &p) {
		return !memcmp(p.first.data(), creds.data(), 16);
	});
}

// --------------------------------------------------------

namespace {
	helix::UniqueLane kerncfgLane;
	helix::UniqueLane pmLane;
};

helix::UniqueLane &getKerncfgLane() {
	return kerncfgLane;
}

helix::UniqueLane &getPmLane() {
	return pmLane;
}

struct CmdlineNode final : public procfs::RegularNode {
	async::result<std::string> show(Process *) override {
		managarm::kerncfg::GetCmdlineRequest req;

		auto [offer, sendReq, recvResp] =
			co_await helix_ng::exchangeMsgs(
				kerncfgLane,
				helix_ng::offer(
					helix_ng::want_lane,
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		auto resp = *bragi::parse_head_only<managarm::kerncfg::SvrResponse>(recvResp);
		assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

		std::vector<char> recvCmdline(resp.size());
		auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(recvCmdline.data(), recvCmdline.size())
		);

		HEL_CHECK(recv_tail.error());

		co_return std::string{(const char *)recvCmdline.data(), recvCmdline.size()} + '\n';
	}

	async::result<void> store(std::string) override {
		throw std::runtime_error("Cannot store to /proc/cmdline");
	}
};

async::result<void> enumerateKerncfg() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "kerncfg"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	kerncfgLane = (co_await entity.getRemoteLane()).unwrap();

	auto procfsRoot = std::static_pointer_cast<procfs::DirectoryNode>(getProcfs()->getTarget());
	procfsRoot->directMkregular("cmdline", std::make_shared<CmdlineNode>());
}

async::result<void> enumeratePm() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "pm-interface"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	pmLane = (co_await entity.getRemoteLane()).unwrap();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

async::detached runInit() {
	co_await posix::initOstrace();
	co_await enumerateKerncfg();
	async::detach(enumeratePm());
	async::detach(net::enumerateNetserver());
	co_await populateRootView();
	co_await Process::init("usr/bin/posix-init");
}

int main() {
	std::cout << "Starting posix-subsystem" << std::endl;

	async::run(clk::enumerateTracker(), helix::currentDispatcher);

//	HEL_CHECK(helSetPriority(kHelThisThread, 1));

	drvcore::initialize();

	charRegistry.install(createHeloutDevice());
	charRegistry.install(pts::createMasterDevice());
	charRegistry.install(createNullDevice());
	charRegistry.install(createFullDevice());
	charRegistry.install(fuse::createFuseDevice());
	charRegistry.install(createRandomDevice());
	charRegistry.install(createUrandomDevice());
	charRegistry.install(createZeroDevice());
	charRegistry.install(createKmsgDevice());

	acpi_subsystem::run();
	drm_subsystem::run();
	input_subsystem::run();
	net_subsystem::run();
	nvme_subsystem::run();
	usbmisc_subsystem::run();
	power_supply_subsystem::run();

	block_subsystem::run();
	generic_subsystem::run();

	pci_subsystem::run();
	usb_subsystem::run();

	runInit();

	async::run_forever(helix::currentDispatcher);
}
