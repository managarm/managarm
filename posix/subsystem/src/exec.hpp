
#ifndef POSIX_SUBSYSTEM_EXEC_HPP
#define POSIX_SUBSYSTEM_EXEC_HPP

#include "process.hpp"

cofiber::future<helix::UniqueDescriptor> execute(std::string path,
		std::shared_ptr<VmContext> vm_context, helix::BorrowedDescriptor universe,
		HelHandle mbus_handle);

#endif // POSIX_SUBSYSTEM_EXEC_HPP

