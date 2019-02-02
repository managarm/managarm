#ifndef LIBKERNLET_COMPILER_HPP
#define LIBKERNLET_COMPILER_HPP

#include <async/result.hpp>
#include <helix/ipc.hpp>

enum class BindType {
	null,
	offset,
	memoryView
};

async::result<void> connectKernletCompiler();
async::result<helix::UniqueDescriptor> compile(void *code, size_t size,
		std::vector<BindType> bind_types);

#endif // LIBKERNLET_COMPILER_HPP
