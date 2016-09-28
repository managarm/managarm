
#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <memory>

class VfsOpenFile;
class MountSpace;

typedef int ProcessId;

struct Process {
	// creates a new process to run the "init" program
	static std::shared_ptr<Process> init();
	
	static void runServer(std::shared_ptr<Process> process);

	Process(ProcessId pid);

	// creates a new process by forking an old one
	std::shared_ptr<Process> fork();

	// unix pid of this process
	ProcessId pid;
	
	// incremented when this process calls execve()
	// ensures that we don't accept new requests from old pipes after execve()
	int iteration;

	// mount namespace and virtual memory space of this process
	MountSpace *mountSpace;
	HelHandle vmSpace;
};

#endif // POSIX_SUBSYSTEM_PROCESS_HPP

