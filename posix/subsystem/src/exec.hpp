#pragma once

#include "process.hpp"

struct ExecuteResult {
	helix::UniqueDescriptor thread;
	void *auxBegin = nullptr;
	void *auxEnd = nullptr;
};

async::result<frg::expected<Error, ExecuteResult>>
execute(ViewPath root, ViewPath workdir,
		std::string path,
		std::vector<std::string> args, std::vector<std::string> env,
		std::shared_ptr<VmContext> vm_context, helix::BorrowedDescriptor universe,
		HelHandle mbus_handle, Process *self);
