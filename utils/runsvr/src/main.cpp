#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <async/oneshot-event.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <svrctl.bragi.hpp>

#include <CLI/CLI.hpp>

// ----------------------------------------------------------------------------
// I/O functions
// ----------------------------------------------------------------------------

int heloutFd;

template <typename ...Ts>
void print_to(int fd, const char *format, Ts &&...ts) {
	dprintf(heloutFd, format, ts...);
	dprintf(fd, format, ts...);
}

template <typename ...Ts>
void log(const char *format, Ts &&...ts) {
	print_to(STDOUT_FILENO, format, std::forward<Ts>(ts)...);
}

template <typename ...Ts>
void err(const char *format, Ts &&...ts) {
	print_to(STDERR_FILENO, format, std::forward<Ts>(ts)...);
}

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
		log("runsvr: fstat() failed on %s\n", path);
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
	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter("class", "svrctl")
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	svrctlLane = (co_await entity.getRemoteLane()).unwrap();
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
	recv_resp.reset();
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
		recv_resp.reset();
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
		recv_resp.reset();
		assert(resp.error() == managarm::svrctl::Error::SUCCESS);
	};

	// Try to avoid reading the file at first.
	if(co_await optimisticUpload())
		co_return;

	// The kernel does not know the file; we have to read the entire contents.
	buffer = readEntireFile(name);
	co_await uploadWithData();
}

async::result<void> bindServer(helix::UniqueLane &lane, int mbusId) {
	managarm::svrctl::CntRequest req;
	req.set_req_type(managarm::svrctl::CntReqType::CTL_BIND);
	req.set_mbus_id(mbusId);

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
	recv_resp.reset();
	assert(resp.error() == managarm::svrctl::Error::SUCCESS);
}

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

enum class action {
	runsvr, run, bind, upload
};

async::result<void> asyncMain(action act, std::string path) {
	co_await enumerateSvrctl();

	switch (act) {
		case action::runsvr: {
			log("runsvr: Running %s\n", path.c_str());
			co_await runServer(path.c_str());

			break;
		}

		case action::run: {
			auto buffer = readEntireFile(path.c_str());

			managarm::svrctl::Description desc;
			bragi::limited_reader rd{buffer.data(), buffer.size()};
			auto deser = bragi::deserializer{};
			desc.decode_body(rd, deser);

			log("runsvr: Running %s\n", desc.name().c_str());

			for(auto &file : desc.files())
				co_await uploadFile(file.path().c_str());

			co_await runServer(desc.exec().c_str());

			break;
		}

		case action::bind: {
			auto buffer = readEntireFile(path.c_str());

			managarm::svrctl::Description desc;
			bragi::limited_reader rd{buffer.data(), buffer.size()};
			auto deser = bragi::deserializer{};
			desc.decode_body(rd, deser);

			auto id_str = getenv("MBUS_ID");
			log("runsvr: Binding driver %s to mbus ID %s\n", desc.name().c_str(), id_str);

			for(auto &file : desc.files())
				co_await uploadFile(file.path().c_str());

			auto lane = co_await runServer(desc.exec().c_str());
			co_await bindServer(lane, std::stoi(id_str));

			break;
		}

		case action::upload: {
			log("runsvr: Uploading %s\n", path.c_str());
			co_await uploadFile(path.c_str());

			break;
		}

		default: {
			err("runsvr: Invalid action (this should be unreachable)\n");
			abort();
		}
	}
}

int main(int argc, const char **argv) {
	heloutFd = open("/dev/helout", O_RDWR);

	bool do_fork = false;
	std::string path;
	action act;

	CLI::App app{"runsvr"};
	app.add_flag("-f,--fork", do_fork, "Fork off before continuing");

	CLI::App *sub_runsvr = app.add_subcommand("runsvr", "Run a server (deprecated)");
	sub_runsvr->add_option("path", path, "Path to executable")->required();

	CLI::App *sub_run = app.add_subcommand("run", "Run a server (used in conjunction with bind)");
	sub_run->add_option("path", path, "Path to description")->required();

	CLI::App *sub_bind = app.add_subcommand("bind", "Bind an mbus ID to a server");
	sub_bind->add_option("path", path, "Path to description")->required();

	CLI::App *sub_upload = app.add_subcommand("upload", "Upload a file");
	sub_upload->add_option("path", path, "Path to file")->required();

	app.require_subcommand(1);

	CLI11_PARSE(app, argc, argv);

	if (*sub_runsvr)
		act = action::runsvr;
	else if (*sub_run)
		act = action::run;
	else if (*sub_bind)
		act = action::bind;
	else if (*sub_upload)
		act = action::upload;
	else {
		err("runsvr: No subcommand specified\n");
		return 1;
	}

	if (do_fork) {
		auto pid = fork();
		if (pid < 0) {
			err("runsvr: Failed to fork: %s\n", strerror(errno));
			return 2;
		} else if (pid) {
			log("runsvr: Forking off to %d\n", pid);
			return 0;
		}
		mbus_ng::recreateInstance();
	}

	async::run(asyncMain(act, std::move(path)), helix::currentDispatcher);
}
