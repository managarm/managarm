
#include <frigg/cxx-support.hpp>
#include <frigg/algorithm.hpp>
#include <helx.hpp>

#include "common.hpp"
#include "process.hpp"
#include "vfs.hpp"

void acceptLoop(helx::Server server, StdSharedPtr<Process> process, int iteration);

StdSharedPtr<Process> Process::init() {
	auto new_process = frigg::makeShared<Process>(*allocator);
	new_process->mountSpace = frigg::construct<MountSpace>(*allocator);
	new_process->nextFd = 3; // reserve space for stdio

	return new_process;
}

helx::Directory Process::runServer(StdSharedPtr<Process> process) {
	int iteration = process->iteration;

	auto directory = helx::Directory::create();
	auto localDirectory = helx::Directory::create();
	auto configDirectory = helx::Directory::create();
	
	directory.mount(configDirectory.getHandle(), "config");
	directory.mount(localDirectory.getHandle(), "local");

	configDirectory.publish(ldServerConnect.getHandle(), "rtdl-server");
	
	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(frigg::move(server), frigg::move(process), iteration);
	localDirectory.publish(client.getHandle(), "posix");

	return directory;
}

Process::Process()
: iteration(0), mountSpace(nullptr), vmSpace(kHelNullHandle),
		allOpenFiles(frigg::DefaultHasher<int>(), *allocator), nextFd(-1) { }

StdSharedPtr<Process> Process::fork() {
	auto new_process = frigg::makeShared<Process>(*allocator);
	
	new_process->mountSpace = mountSpace;
	HEL_CHECK(helForkSpace(vmSpace, &new_process->vmSpace));

	for(auto it = allOpenFiles.iterator(); it; ++it)
		new_process->allOpenFiles.insert(it->get<0>(), it->get<1>());
	new_process->nextFd = nextFd;

	return new_process;
}

