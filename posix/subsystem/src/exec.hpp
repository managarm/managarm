
#ifndef POSIX_SUBSYSTEM_EXEC_HPP
#define POSIX_SUBSYSTEM_EXEC_HPP

#include "process.hpp"

void execute(std::shared_ptr<Process> process, std::string path);

#endif // POSIX_SUBSYSTEM_EXEC_HPP

