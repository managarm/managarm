#ifndef LIBKERNLET_COMPILER_HPP
#define LIBKERNLET_COMPILER_HPP

async::result<void> connectKernletCompiler();
async::result<helix::UniqueDescriptor> compile(void *code, size_t size);

#endif // LIBKERNLET_COMPILER_HPP
