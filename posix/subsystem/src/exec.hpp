
#ifndef POSIX_SUBSYSTEM_EXEC_HPP
#define POSIX_SUBSYSTEM_EXEC_HPP

#include "process.hpp"

async::result<frg::expected<Error, helix::UniqueDescriptor>>
execute(ViewPath root, ViewPath workdir,
		std::string path,
		std::vector<std::string> args, std::vector<std::string> env,
		std::shared_ptr<VmContext> vm_context, helix::BorrowedDescriptor universe,
		HelHandle mbus_handle);

#endif // POSIX_SUBSYSTEM_EXEC_HPP

