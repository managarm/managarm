
#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <frigg/hashmap.hpp>

class VfsOpenFile;
class MountSpace;

struct Process {
	// creates a new process to run the "init" program
	static StdSharedPtr<Process> init();
	
	static helx::Directory runServer(StdSharedPtr<Process> process);

	Process();

	// creates a new process by forking an old one
	StdSharedPtr<Process> fork();
	
	// incremented when this process calls execve()
	// ensures that we don't accept new requests from old pipes after execve()
	int iteration;

	// mount namespace and virtual memory space of this process
	MountSpace *mountSpace;
	HelHandle vmSpace;

	frigg::Hashmap<int, StdSharedPtr<VfsOpenFile>,
			frigg::DefaultHasher<int>, Allocator> allOpenFiles;
	int nextFd;
};

#endif // POSIX_SUBSYSTEM_PROCESS_HPP

