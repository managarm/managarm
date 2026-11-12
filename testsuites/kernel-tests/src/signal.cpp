#include <cassert>
#include <cerrno>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <hel-syscalls.h>
#include <hel.h>
#include <protocols/posix/supercalls.hpp>

#include "testsuite.hpp"

DEFINE_TEST(signal_stack_request_validation, ([] {
	            // The libc wrapper inspects ss before issuing its supercall. Use the
	            // server interface directly to test invalid input and minimum-size checks.
	            auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
	            auto mapping =
	                mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	            assert(mapping != MAP_FAILED);
	            assert(!munmap(mapping, pageSize));
	            auto request = [](const stack_t *ss, stack_t *oss) {
		            HelWord error;
		            HEL_CHECK(helSyscall2_1(
		                kHelObserveSuperCall + posix::superSigAltStack,
		                reinterpret_cast<HelWord>(ss),
		                reinterpret_cast<HelWord>(oss),
		                &error
		            ));
		            return error;
	            };
	            assert(request(static_cast<const stack_t *>(mapping), nullptr) == EFAULT);
	            assert(request(nullptr, static_cast<stack_t *>(mapping)) == EFAULT);
	            stack_t stack{};
	            stack.ss_size = 1;
	            assert(request(&stack, nullptr) == ENOMEM);
            }))
