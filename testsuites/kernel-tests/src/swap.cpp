#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>

#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>

#include "testsuite.hpp"

namespace {

constexpr size_t pageSize = 0x1000;
constexpr size_t swapPages = 64;
constexpr size_t viewPages = 16;

struct SwapDaemonState {
	std::vector<uint8_t> disk;
	std::atomic<size_t> pagesWrittenBack{0};
	std::atomic<size_t> pagesInitialized{0};
	std::atomic<bool> stop{false};
	std::atomic<bool> finished{false};
};

// Emulates the swap daemon, serves a single manage request. Services
// initialize requests by reading from the disk buffer and writeback
// requests by writing to it.
async::result<void> serveOnce(helix::BorrowedDescriptor backing, SwapDaemonState *state) {
	helix::ManageMemory manage;
	auto submit = helix::submitManageMemory(backing, &manage,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(manage.error());

	assert(!(manage.offset() & (pageSize - 1)));
	assert(!(manage.length() & (pageSize - 1)));
	assert(manage.offset() + manage.length() <= state->disk.size());

	if(manage.type() == kHelManageInitialize) {
		auto result = co_await helix_ng::writeMemory(backing, manage.offset(),
				manage.length(), state->disk.data() + manage.offset());
		HEL_CHECK(result.error());
		HEL_CHECK(helUpdateMemory(backing.getHandle(), kHelManageInitialize,
				manage.offset(), manage.length()));
		state->pagesInitialized.fetch_add(manage.length() / pageSize,
				std::memory_order_relaxed);
	}else{
		assert(manage.type() == kHelManageWriteback);
		auto result = co_await helix_ng::readMemory(backing, manage.offset(),
				manage.length(), state->disk.data() + manage.offset());
		HEL_CHECK(result.error());
		HEL_CHECK(helUpdateMemory(backing.getHandle(), kHelManageWriteback,
				manage.offset(), manage.length()));
		state->pagesWrittenBack.fetch_add(manage.length() / pageSize,
				std::memory_order_relaxed);
		if(state->stop.load(std::memory_order_relaxed))
			state->finished.store(true, std::memory_order_release);
	}
}

void fillPattern(void *p, size_t page, uint32_t salt) {
	auto words = reinterpret_cast<uint32_t *>(p);
	for(size_t i = 0; i < pageSize / sizeof(uint32_t); i++)
		words[i] = (static_cast<uint32_t>(page) * 2654435761u + static_cast<uint32_t>(i)) ^ salt;
}

bool checkPattern(const void *p, size_t page, uint32_t salt) {
	auto words = reinterpret_cast<const uint32_t *>(p);
	for(size_t i = 0; i < pageSize / sizeof(uint32_t); i++) {
		if(words[i] != ((static_cast<uint32_t>(page) * 2654435761u + static_cast<uint32_t>(i)) ^ salt))
			return false;
	}
	return true;
}

async::result<void> testSwapRoundtrip() {
	HelHandle backingHandle, swapHandle;
	HEL_CHECK(helCreateSwapSpace(0, &backingHandle, &swapHandle));
	helix::UniqueDescriptor backing{backingHandle};
	helix::UniqueDescriptor swapSpace{swapHandle};

	HEL_CHECK(helSetSwapBudget(swapSpace.getHandle(), swapPages));

	HelHandle memoryHandle;
	HEL_CHECK(helAllocateSwappableMemory(swapSpace.getHandle(),
			viewPages * pageSize, 0, &memoryHandle));
	helix::UniqueDescriptor memory{memoryHandle};

	SwapDaemonState state;
	state.disk.resize(swapPages * pageSize);

	// Write patterns through a real mapping.
	void *window;
	HEL_CHECK(helMapMemory(memory.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	for(size_t pg = 0; pg < viewPages; pg++)
		fillPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, 0);

	// Unmapping scans the dirty PTE bits, so all pages enter writeback.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	while(state.pagesWrittenBack.load(std::memory_order::relaxed) < viewPages)
		co_await serveOnce(backing, &state);
	assert(state.pagesWrittenBack.load(std::memory_order::relaxed) == viewPages);

	// Under torture uncaching the pages are dropped within a few generation,
	// give the kernel some time.
	std::this_thread::sleep_for(std::chrono::milliseconds{1000});

	// Read the pages back and verify that the patterns survived. We read via
	// the kernel (helSubmitReadMemory) instead of a mapping as page faults would
	// block this thread.
	co_await async::when_all(
		async::lambda([&]() -> async::result<void> {
			std::vector<uint8_t> buffer(pageSize);
			for(size_t pg = 0; pg < viewPages; pg++) {
				auto result = co_await helix_ng::readMemory(memory,
						pg * pageSize, pageSize, buffer.data());
				HEL_CHECK(result.error());
				assert(checkPattern(buffer.data(), pg, 0));
			}
			state.stop.store(true, std::memory_order::relaxed);
			std::vector<uint8_t> sentinel(pageSize);
			fillPattern(sentinel.data(), 0, 0xdeadbeef);
			auto result = co_await helix_ng::writeMemory(memory, 0, pageSize,
					sentinel.data());
			HEL_CHECK(result.error());
		})(),
		async::lambda([&]() -> async::result<void> {
			while(!state.finished.load(std::memory_order::relaxed))
				co_await serveOnce(backing, &state);
		})()
	);

	printf("kernel-tests: swapRoundtrip: %zu pages swapped out, %zu pages swapped in\n",
			state.pagesWrittenBack.load(std::memory_order_relaxed),
			state.pagesInitialized.load(std::memory_order_relaxed));
	if(!state.pagesInitialized.load(std::memory_order::relaxed)) {
		printf("kernel-tests: swapRoundtrip: swap-in path was NOT exercised"
				" (boot with thor.torture-uncaching)\n");
	}
}

// Like testSwapRoundtrip, but the swapped-out pages are swapped back in by
// faulting on a real mapping.
void testSwapFaultIn() {
HelHandle backingHandle, swapHandle;
	HEL_CHECK(helCreateSwapSpace(0, &backingHandle, &swapHandle));
	helix::UniqueDescriptor backing{backingHandle};
	helix::UniqueDescriptor swapSpace{swapHandle};

	HEL_CHECK(helSetSwapBudget(swapSpace.getHandle(), swapPages));

	HelHandle memoryHandle;
	HEL_CHECK(helAllocateSwappableMemory(swapSpace.getHandle(),
			viewPages * pageSize, 0, &memoryHandle));
	helix::UniqueDescriptor memory{memoryHandle};

	SwapDaemonState state;
	state.disk.resize(swapPages * pageSize);

	// Write patterns through a real mapping.
	void *window;
	HEL_CHECK(helMapMemory(memory.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	for(size_t pg = 0; pg < viewPages; pg++)
		fillPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, 0);

	// Unmapping scans the dirty PTE bits, so all pages enter writeback.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	// The daemon services all requests from its own thread until finished.
	std::thread daemon{[&] {
		async::run(async::lambda([&]() -> async::result<void> {
			while(!state.finished.load(std::memory_order::acquire))
				co_await serveOnce(backing, &state);
		})(), helix::currentDispatcher);
	}};

	while(state.pagesWrittenBack < viewPages)
		std::this_thread::sleep_for(std::chrono::milliseconds{10});

	// Under torture uncaching the pages are dropped within a few generation,
	// give the kernel some time.
	std::this_thread::sleep_for(std::chrono::milliseconds{1000});

	// Fault the pages back in by reading through a fresh mapping and verify
	// the patterns.
	HEL_CHECK(helMapMemory(memory.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	for(size_t pg = 0; pg < viewPages; pg++)
		assert(checkPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, 0));

	// Dirty one page through the mapping and then unmap, so that exactly one
	// further writeback request arrives after stop is set.
	state.stop.store(true, std::memory_order_relaxed);
	fillPattern(window, 0, 0xcafebabe);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));
	daemon.join();

	printf("kernel-tests: swapFaultIn: %zu pages swapped out, %zu pages swapped in\n",
			state.pagesWrittenBack.load(std::memory_order_relaxed),
			state.pagesInitialized.load(std::memory_order_relaxed));
	if(!state.pagesInitialized.load(std::memory_order::relaxed)) {
		printf("kernel-tests: swapFaultIn: swap-in path was NOT exercised"
				" (boot with thor.torture-uncaching)\n");
	}
}

} // anonymous namespace

DEFINE_TEST(swapRoundtrip, ([] {
	async::run(testSwapRoundtrip(), helix::currentDispatcher);
}))

DEFINE_TEST(swapFaultIn, ([] {
	testSwapFaultIn();
}))
