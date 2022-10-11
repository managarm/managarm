#include <memory>

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
#include "pts.hpp"
#include "requests.hpp"
#include "subsystem/block.hpp"
#include "subsystem/drm.hpp"
#include "subsystem/generic.hpp"
#include "subsystem/input.hpp"
#include "subsystem/pci.hpp"
#include "observations.hpp"

#include <kerncfg.pb.h>
#include <posix.bragi.hpp>

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
