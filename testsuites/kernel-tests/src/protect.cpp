#include <cassert>
#include <cstddef>

#include <async/result.hpp>
#include <helix/ipc.hpp>

#include <hel.h>
#include <hel-syscalls.h>

#include "testsuite.hpp"

namespace {

// helLog() reads user memory through the kernel and reports kHelErrFault on inaccessible
// pages, letting us probe access without faulting the test itself.

async::result<void> testProtectProtNone() {
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &handle));
	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, 0x1000,
			kHelMapProtRead | kHelMapProtWrite, &window));

	// Touch the page so that it is actually present before we drop access.
	auto p = reinterpret_cast<volatile std::byte *>(window);
	p[0] = static_cast<std::byte>(42);
	assert(p[0] == static_cast<std::byte>(42));

	// Drop all access; the previously present page must become inaccessible.
	helix::ProtectMemory dropProtect;
	auto &&dropSubmit = helix::submitProtectMemory(helix::BorrowedDescriptor{kHelNullHandle},
			&dropProtect, window, 0x1000, 0, helix::Dispatcher::global());
	co_await dropSubmit.async_wait();
	HEL_CHECK(dropProtect.error());

	assert(helLog(kHelLogSeverityInfo, static_cast<char *>(window), 0x1000) == kHelErrFault);

	// Restoring access faults the page back in on demand.
	helix::ProtectMemory restoreProtect;
	auto &&restoreSubmit = helix::submitProtectMemory(helix::BorrowedDescriptor{kHelNullHandle},
			&restoreProtect, window, 0x1000, kHelMapProtRead | kHelMapProtWrite,
			helix::Dispatcher::global());
	co_await restoreSubmit.async_wait();
	HEL_CHECK(restoreProtect.error());

	p[0] = static_cast<std::byte>(43);
	assert(p[0] == static_cast<std::byte>(43));

	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, 0x1000));
}

} // anonymous namespace

DEFINE_TEST(mapProtNone, ([] {
	HelHandle handle;
	HEL_CHECK(helAllocateMemory(0x1000, 0, nullptr, &handle));
	void *window;
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, 0x1000, 0, &window));

	// A mapping without any access permits no access at all.
	assert(helLog(kHelLogSeverityInfo, static_cast<char *>(window), 0x1000) == kHelErrFault);

	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, 0x1000));
}))

DEFINE_TEST(protectProtNone, ([] {
	async::run(testProtectProtNone(), helix::currentDispatcher);
}))
