
#ifndef POSIX_SUBSYSTEM_EXEC_HPP
#define POSIX_SUBSYSTEM_EXEC_HPP

#include "process.hpp"

void execute(StdUnsafePtr<Process> process, frigg::StringView path);

#endif // POSIX_SUBSYSTEM_EXEC_HPP

