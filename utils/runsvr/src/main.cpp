
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

#include <async/jump.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <svrctl.pb.h>

static std::vector<std::byte> readEntireFile(const char *path) {
	constexpr size_t bytesPerChunk = 8192;

	auto fd = open(path, O_RDONLY);
	if(fd < 0)
		throw std::runtime_error("Could not open file");

	std::vector<std::byte> buffer;

	struct stat st;
	if(!fstat(fd, &st)) {
		buffer.reserve(st.st_size);
	}else{
		std::cout << "runsvr: fstat() failed on " << path << std::endl;
	}

	off_t progress = 0;
	while(true) {
		buffer.resize(progress + bytesPerChunk);
		auto chunk = read(fd, buffer.data() + progress, bytesPerChunk);
		if(!chunk)
			break;
		if(chunk < 0)
			throw std::runtime_error("Error while reading file");
		progress += chunk;
	}

	close(fd);

	buffer.resize(progress);
	return buffer;
}

// ----------------------------------------------------------------------------
// svrctl handling.
// ----------------------------------------------------------------------------

helix::UniqueLane svrctlLane;
async::jump foundSvrctl;

async::result<void> enumerateSvrctl() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "svrctl")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
//		std::cout << "runsvr: Found svrctl" << std::endl;

		svrctlLane = helix::UniqueLane(co_await entity.bind());
		foundSvrctl.trigger();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundSvrctl.async_wait();
}

async::result<helix::UniqueLane> runServer(const char *name) {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_server;

	managarm::svrctl::CntRequest req;
	req.set_req_type(managarm::svrctl::CntReqType::SVR_RUN);
	req.set_name(name);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(svrctlLane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_server));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_server.error());

	managarm::svrctl::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::svrctl::Error::SUCCESS);
	co_return pull_server.descriptor();
}

async::result<void> uploadFile(const char *name) {
	// First, load the whole file into a buffer.
	auto buffer = readEntireFile(name);

	// Now, send the file to the kernel.
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::SendBuffer send_data;
	helix::RecvInline recv_resp;

	managarm::svrctl::CntRequest req;
	req.set_req_type(managarm::svrctl::CntReqType::FILE_UPLOAD);
	req.set_name(name);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(svrctlLane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_data, buffer.data(), buffer.size(), kHelItemChain),
			helix::action(&recv_resp));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::svrctl::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::svrctl::Error::SUCCESS);
}

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

async::detached asyncMain(const char **args) {
	co_await enumerateSvrctl();

	if(!strcmp(args[1], "runsvr")) {
		if(!args[2])
			throw std::runtime_error("Expected at least one argument");

		// TODO: Eventually remove the runsvr command in favor of run + bind.
		std::cout << "svrctl: Running " << args[2] << std::endl;

		co_await runServer(args[2]);
		exit(0);
	}else if(!strcmp(args[1], "run")) {
		if(!args[2])
			throw std::runtime_error("Expected at least one argument");
		auto buffer = readEntireFile(args[2]);

		managarm::svrctl::Description desc;
		desc.ParseFromArray(buffer.data(), buffer.size());

		std::cout << "svrctl: Running " << desc.name() << std::endl;

		co_await runServer(desc.exec().c_str());
		exit(0);
	}else if(!strcmp(args[1], "bind")) {
		if(!args[2])
			throw std::runtime_error("Expected at least one argument");
		auto buffer = readEntireFile(args[2]);

		managarm::svrctl::Description desc;
		desc.ParseFromArray(buffer.data(), buffer.size());

		auto id_str = getenv("MBUS_ID");
		std::cout << "svrctl: Binding driver " << desc.name()
				<< " to mbus ID " << id_str << std::endl;

		auto lane = co_await runServer(desc.exec().c_str());

		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;

		managarm::svrctl::CntRequest req;
		req.set_req_type(managarm::svrctl::CntReqType::CTL_BIND);
		req.set_mbus_id(std::stoi(id_str));

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp));
		co_await transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::svrctl::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::svrctl::Error::SUCCESS);

		exit(0);
	}else if(!strcmp(args[1], "upload")) {
		if(!args[2])
			throw std::runtime_error("Expected at least one argument");

		std::cout << "svrctl: Uploading " << args[2] << std::endl;

		co_await uploadFile(args[2]);
		exit(0);
	}else{
		throw std::runtime_error("Unexpected command for svrctl utility");
	}
}

int main(int argc, const char **argv) {
	int fd = open("/dev/helout", O_RDONLY);
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	{
		async::queue_scope scope{helix::globalQueue()};
		asyncMain(argv);
	}

	helix::globalQueue()->run();

	return 0;
}

