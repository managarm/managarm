
#ifndef POSIX_SUBSYSTEM_EXEC_HPP
#define POSIX_SUBSYSTEM_EXEC_HPP

#include "process.hpp"

void execute(StdSharedPtr<Process> process, frigg::String<Allocator> path);

#endif // POSIX_SUBSYSTEM_EXEC_HPP

