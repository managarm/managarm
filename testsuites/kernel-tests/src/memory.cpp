#include <cassert>
#include <cstddef>

#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <core/process-data.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include "testsuite.hpp"

namespace {

// Processes an exact number of notifications.
async::result<void> handleManageRequests(helix::BorrowedDescriptor backingMemory, int count) {
	std::byte buffer[0x1000]{};

	for(int i = 0; i < count; i++) {
		helix::ManageMemory manage;
		auto submit = helix::submitManageMemory(backingMemory, &manage,
				helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());
		if(manage.type() == kHelManageInitialize) {
			auto result = co_await helix_ng::writeMemory(backingMemory, manage.offset(), 0x1000, buffer);
			HEL_CHECK(result.error());
			HEL_CHECK(helUpdateMemory(backingMemory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		} else {
			assert(manage.type() == kHelManageWriteback);
			HEL_CHECK(helUpdateMemory(backingMemory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::result<void> testWritebackFence() {
	constexpr size_t memorySize = size_t{1} << 40;
	constexpr size_t pageOffset = memorySize / 2;
	HelHandle backingHandle, frontalHandle;
	HEL_CHECK(helCreateManagedMemory(core::getProcessHierarchy(), memorySize, 0,
			&backingHandle, &frontalHandle));
	helix::UniqueDescriptor backingMemory{backingHandle};
	helix::UniqueDescriptor frontalMemory{frontalHandle};

	// An entirely absent range must not scan every possible page.
	auto emptyFence = co_await helix_ng::writebackFence(backingMemory, 0, memorySize);
	HEL_CHECK(emptyFence.error());

	// Pin an initialized page so mapped stores need no synchronous page service.
	helix::LockMemoryView lockMemory;
	co_await async::when_all(
		handleManageRequests(backingMemory, 1),
		async::lambda([&]() -> async::result<void> {
			auto submit = helix::submitLockMemoryView(frontalMemory, &lockMemory,
					pageOffset, 0x1000, helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lockMemory.error());
		})()
	);
	auto lock = lockMemory.descriptor();
	helix::Mapping first{frontalMemory, pageOffset, 0x1000, kHelMapProtRead | kHelMapProtWrite};
	helix::Mapping second{frontalMemory, pageOffset, 0x1000, kHelMapProtRead | kHelMapProtWrite};

	// No writeMemory() or synchronizeSpace(): only the mapping's PTE is dirty.
	// Repeat through another alias to check that the fence collects all mappings
	// and that stores after the first shootdown are picked up again.
	for(auto mapping : {first.get(), second.get()}) {
		auto expected = mapping == first.get() ? std::byte{42} : std::byte{43};
		*static_cast<volatile std::byte *>(mapping) = expected;
		bool writtenBack = false;
		co_await async::when_all(
			async::lambda([&]() -> async::result<void> {
				helix::ManageMemory manage;
				auto submit = helix::submitManageMemory(backingMemory, &manage,
						helix::Dispatcher::global());
				co_await submit.async_wait();
				HEL_CHECK(manage.error());
				assert(manage.type() == kHelManageWriteback);
				assert(manage.offset() == pageOffset && manage.length() == 0x1000);
				std::byte data;
				auto read = co_await helix_ng::readMemory(backingMemory, pageOffset, 1, &data);
				HEL_CHECK(read.error());
				assert(data == expected);
				writtenBack = true;
				HEL_CHECK(helUpdateMemory(backingMemory.getHandle(), kHelManageWriteback,
						manage.offset(), manage.length()));
			})(),
			async::lambda([&]() -> async::result<void> {
				auto result = co_await helix_ng::writebackFence(backingMemory, 0, memorySize);
				HEL_CHECK(result.error());
				assert(writtenBack);
			})()
		);
	}

	// The existing page at the exclusive end must be outside this empty range.
	auto prefixFence = co_await helix_ng::writebackFence(backingMemory, 0, pageOffset);
	HEL_CHECK(prefixFence.error());

	// A clean range needs no further writeback notification.
	auto result = co_await helix_ng::writebackFence(backingMemory, 0, memorySize);
	HEL_CHECK(result.error());
}

} // anonymous namespace

DEFINE_TEST(writebackFence, ([] {
	async::run(testWritebackFence(), helix::currentDispatcher);
}))

namespace {

async::result<void> testInvalidateRange() {
	HelHandle backingHandle, frontalHandle;
	HEL_CHECK(helCreateManagedMemory(core::getProcessHierarchy(), 0x1000, 0, &backingHandle, &frontalHandle));
	helix::UniqueDescriptor backingMemory{backingHandle};
	helix::UniqueDescriptor frontalMemory{frontalHandle};

	std::byte buffer[0x1000]{};

	// Trigger initialization, then writeback.
	co_await async::when_all(
		handleManageRequests(backingMemory, 2),
		async::lambda([&]() -> async::result<void> {
			auto result = co_await helix_ng::writeMemory(frontalMemory, 0, 0x1000, buffer);
			HEL_CHECK(result.error());
			co_return;
		})()
	);

	auto invalidateResult = co_await helix_ng::invalidateMemory(backingMemory, 0, 0x1000);
	HEL_CHECK(invalidateResult.error());

	// A subsequent write to the frontal memory must trigger initialization again.
	co_await async::when_all(
		handleManageRequests(backingMemory, 2),
		async::lambda([&]() -> async::result<void> {
			auto result = co_await helix_ng::writeMemory(frontalMemory, 0, 0x1000, buffer);
			HEL_CHECK(result.error());
			co_return;
		})()
	);
}

} // anonymous namespace

DEFINE_TEST(invalidateRange, ([] {
	async::run(testInvalidateRange(), helix::currentDispatcher);
}))
