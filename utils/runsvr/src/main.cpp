
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

#include <async/jump.hpp>
#include <helix/memory.hpp>
#include <protocols/mbus/client.hpp>
#include <svrctl.pb.h>

// ----------------------------------------------------------------------------
// svrctl handling.
// ----------------------------------------------------------------------------

helix::UniqueLane svrctlLane;
async::jump foundSvrctl;

COFIBER_ROUTINE(async::result<void>, enumerateSvrctl(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "svrctl")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
//		std::cout << "runsvr: Found svrctl" << std::endl;

		svrctlLane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
		foundSvrctl.trigger();
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
	COFIBER_AWAIT foundSvrctl.async_wait();
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<void>, runServer(const char *name), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;

	managarm::svrctl::CntRequest req;
	req.set_req_type(managarm::svrctl::CntReqType::SVR_RUN);
	req.set_name(name);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(svrctlLane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	managarm::svrctl::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::svrctl::Error::SUCCESS);
	
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<void>, uploadFile(const char *name), ([=] {
	// First, load the whole file into a buffer.
	// TODO: stat() + read() introduces a TOCTTOU race.
	struct stat st;
	if(stat(name, &st))
		throw std::runtime_error("Could not stat file");
	
	auto fd = open(name, O_RDONLY);

	auto buffer = malloc(st.st_size);
	if(!buffer)
		throw std::runtime_error("Could not allocate buffer for file");

	off_t progress = 0;
	while(progress < st.st_size) {
		auto chunk = read(fd, buffer, st.st_size - progress);
		if(chunk <= 0)
			throw std::runtime_error("Error while reading file");
		progress += chunk;
	}

	close(fd);

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
			helix::action(&send_data, buffer, progress, kHelItemChain),
			helix::action(&recv_resp));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());

	free(buffer);

	managarm::svrctl::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::svrctl::Error::SUCCESS);

	COFIBER_RETURN();
}))

// ----------------------------------------------------------------
// Freestanding mbus functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, asyncMain(const char **args), ([=] {
	COFIBER_AWAIT enumerateSvrctl();

	if(!strcmp(args[1], "runsvr")) {
		if(!args[2])
			throw std::runtime_error("Expected at least one argument");
	
		std::cout << "svrctl: Running " << args[2] << std::endl;

		COFIBER_AWAIT runServer(args[2]);
		exit(0);
	}else if(!strcmp(args[1], "upload")) {
		if(!args[2])
			throw std::runtime_error("Expected at least one argument");
		
		std::cout << "svrctl: Uploading " << args[2] << std::endl;

		COFIBER_AWAIT uploadFile(args[2]);
		exit(0);
	}else{
		throw std::runtime_error("Unexpected command for svrctl utility");
	}
}))

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


