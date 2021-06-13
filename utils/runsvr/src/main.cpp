
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

#include <async/oneshot-event.hpp>
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
async::oneshot_event foundSvrctl;

async::result<void> enumerateSvrctl() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "svrctl")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
//		std::cout << "runsvr: Found svrctl" << std::endl;

		svrctlLane = helix::UniqueLane(co_await entity.bind());
		foundSvrctl.raise();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundSvrctl.wait();
}

async::result<helix::UniqueLane> runServer(const char *name) {
	managarm::svrctl::CntRequest req;
	req.set_req_type(managarm::svrctl::CntReqType::SVR_RUN);
	req.set_name(name);

	auto ser = req.SerializeAsString();
	auto [offer, send_req, recv_resp, pull_server] = co_await helix_ng::exchangeMsgs(
		svrctlLane,
		helix_ng::offer(
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor())
	);
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
	std::vector<std::byte> buffer;

	auto optimisticUpload = [&] () -> async::result<bool> {
		managarm::svrctl::CntRequest req;
		req.set_req_type(managarm::svrctl::CntReqType::FILE_UPLOAD);
		req.set_name(name);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
			svrctlLane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::recvInline())
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());

		managarm::svrctl::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		if(resp.error() == managarm::svrctl::Error::DATA_REQUIRED)
			co_return false;
		assert(resp.error() == managarm::svrctl::Error::SUCCESS);
		co_return true;
	};

	auto uploadWithData = [&] () -> async::result<void> {
		managarm::svrctl::CntRequest req;
		req.set_req_type(managarm::svrctl::CntReqType::FILE_UPLOAD_DATA);
		req.set_name(name);

		auto ser = req.SerializeAsString();
		auto [offer, send_req, send_data, recv_resp] = co_await helix_ng::exchangeMsgs(
			svrctlLane,
			helix_ng::offer(
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::sendBuffer(buffer.data(), buffer.size()),
				helix_ng::recvInline())
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_data.error());
		HEL_CHECK(recv_resp.error());

		managarm::svrctl::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::svrctl::Error::SUCCESS);
	};

	// Try to avoid reading the file at first.
	if(co_await optimisticUpload())
		co_return;

	// The kernel does not know the file; we have to read the entire contents.
	buffer = readEntireFile(name);
	co_await uploadWithData();
}

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

enum class Action: uint8_t {
	runsvr,
	run,
	bind,
	upload,
};

static const std::unordered_map<std::string_view, Action> actionMap {
	{ { "runsvr",   6 }, Action::runsvr },
	{ { "run",	  	3 }, Action::run	},
	{ { "bind",	 	4 }, Action::bind   },
	{ { "upload",   6 }, Action::upload },
};

async::detached asyncMain(const char *args[]) {
	co_await enumerateSvrctl();

	auto it = actionMap.find(std::string_view(args[1], strlen(args[1])));
	if (it != actionMap.end())
		switch(it->second) {
		case Action::runsvr: {
			if(!args[2])
				throw std::runtime_error("Expected at least one argument");

			// TODO: Eventually remove the runsvr command in favor of run + bind.
			std::cout << "svrctl: Running " << args[2] << std::endl;

			co_await runServer(args[2]);
			break;
		}
		case Action::run: {
			if(!args[2])
				throw std::runtime_error("Expected at least one argument");
			auto buffer = readEntireFile(args[2]);

			managarm::svrctl::Description desc;
			desc.ParseFromArray(buffer.data(), buffer.size());

			std::cout << "svrctl: Running " << desc.name() << std::endl;

			for(const auto &file : desc.files())
				co_await uploadFile(file.path().c_str());

			co_await runServer(desc.exec().c_str());
			break;
		}
		case Action::bind: {
			if(!args[2])
				throw std::runtime_error("Expected at least one argument");
			auto buffer = readEntireFile(args[2]);

			managarm::svrctl::Description desc;
			desc.ParseFromArray(buffer.data(), buffer.size());

			auto id_str = getenv("MBUS_ID");
			std::cout << "svrctl: Binding driver " << desc.name()
					<< " to mbus ID " << id_str << std::endl;

			for(const auto &file : desc.files())
				co_await uploadFile(file.path().c_str());

			auto lane = co_await runServer(desc.exec().c_str());

			managarm::svrctl::CntRequest req;
			req.set_req_type(managarm::svrctl::CntReqType::CTL_BIND);
			req.set_mbus_id(std::stoi(id_str));

			auto ser = req.SerializeAsString();
			auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline())
			);
			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(recv_resp.error());

			managarm::svrctl::SvrResponse resp;
			resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			assert(resp.error() == managarm::svrctl::Error::SUCCESS);
			break;
		}
		case Action::upload: {
			if(!args[2])
				throw std::runtime_error("Expected at least one argument");

			std::cout << "svrctl: Uploading " << args[2] << std::endl;

			co_await uploadFile(args[2]);
			break;
		}
		default:;
		}
	else
		throw std::runtime_error("Unexpected command for svrctl utility");

	exit(0);
}

int main(int, const char **argv) {
	int fd = open("/dev/helout", O_RDONLY);
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	{
		async::queue_scope scope{helix::globalQueue()};
		asyncMain(argv);
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);

	return 0;
}

