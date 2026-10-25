#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <map>
#include <thread>
#include <vector>

#include <async/algorithm.hpp>
#include <async/result.hpp>
#include <core/process-data.hpp>
#include <helix/ipc.hpp>

#include "testsuite.hpp"

namespace {

constexpr size_t pageSize = 0x1000;
constexpr size_t swapPages = 64;
constexpr size_t viewPages = 16;

constexpr uint64_t regionMask = (uint64_t{1} << kHelSwapRegionShift) - 1;

struct SwapRegion {
	// Emulated swap file.
	std::vector<uint8_t> disk;
	std::atomic<size_t> pagesWrittenBack{0};
};

struct SwapDaemonState {
	// Not modified while requests are served concurrently.
	std::map<uint32_t, SwapRegion> regions;
	std::atomic<size_t> pagesWrittenBack{0};
	std::atomic<size_t> pagesInitialized{0};
	std::atomic<bool> stop{false};
	std::atomic<bool> finished{false};

	SwapRegion &regionOf(uint64_t offset) {
		auto it = regions.find(offset >> kHelSwapRegionShift);
		assert(it != regions.end());
		return it->second;
	}

	// Returns the disk contents that back [offset, offset + length) of the backing memory.
	uint8_t *diskRange(uint64_t offset, size_t length) {
		auto &disk = regionOf(offset).disk;
		assert((offset & regionMask) + length <= disk.size());
		return disk.data() + (offset & regionMask);
	}
};

async::result<void> setSwapRegion(helix::BorrowedDescriptor swapSpace, uint32_t region,
		size_t numPages, SwapDaemonState *state) {
	auto result = co_await helix_ng::setSwapRegion(swapSpace, region, numPages);
	HEL_CHECK(result.error());
	state->regions[region].disk.resize(numPages * pageSize);
}

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
	// Also asserts that the request lies within a single region.
	auto disk = state->diskRange(manage.offset(), manage.length());

	if(manage.type() == kHelManageInitialize) {
		auto result = co_await helix_ng::writeMemory(backing, manage.offset(),
				manage.length(), disk);
		HEL_CHECK(result.error());
		HEL_CHECK(helUpdateMemory(backing.getHandle(), kHelManageInitialize,
				manage.offset(), manage.length()));
		state->pagesInitialized.fetch_add(manage.length() / pageSize,
				std::memory_order_relaxed);
	}else{
		assert(manage.type() == kHelManageWriteback);
		auto result = co_await helix_ng::readMemory(backing, manage.offset(),
				manage.length(), disk);
		HEL_CHECK(result.error());
		HEL_CHECK(helUpdateMemory(backing.getHandle(), kHelManageWriteback,
				manage.offset(), manage.length()));
		state->regionOf(manage.offset()).pagesWrittenBack.fetch_add(manage.length() / pageSize,
				std::memory_order_relaxed);
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
	HEL_CHECK(helCreateSwapSpace(core::getProcessHierarchy(), 0, &backingHandle, &swapHandle));
	helix::UniqueDescriptor backing{backingHandle};
	helix::UniqueDescriptor swapSpace{swapHandle};

	// The first region is too small for the view, so writeback continues in a second region.
	constexpr size_t firstPages = 4;
	constexpr uint32_t secondRegion = kHelNumSwapRegions - 1;

	SwapDaemonState state;
	co_await setSwapRegion(swapSpace, 0, firstPages, &state);

	HelHandle memoryHandle;
	HEL_CHECK(helAllocateSwappableMemory(core::getProcessHierarchy(), swapSpace.getHandle(),
			viewPages * pageSize, 0, &memoryHandle));
	helix::UniqueDescriptor memory{memoryHandle};

	// Write patterns through a real mapping.
	void *window;
	HEL_CHECK(helMapMemory(memory.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	for(size_t pg = 0; pg < viewPages; pg++)
		fillPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, 0);

	// Unmapping scans the dirty PTE bits, so all pages enter writeback.
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	// Clean pages keep their slots, so writeback stalls once the first region is full.
	while(state.pagesWrittenBack.load(std::memory_order::relaxed) < firstPages)
		co_await serveOnce(backing, &state);
	assert(state.regions.at(0).pagesWrittenBack.load(std::memory_order::relaxed) == firstPages);

	co_await setSwapRegion(swapSpace, secondRegion, swapPages, &state);
	while(state.pagesWrittenBack.load(std::memory_order::relaxed) < viewPages)
		co_await serveOnce(backing, &state);
	assert(state.pagesWrittenBack.load(std::memory_order::relaxed) == viewPages);
	assert(state.regions.at(secondRegion).pagesWrittenBack.load(std::memory_order::relaxed)
			== viewPages - firstPages);

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
	HEL_CHECK(helCreateSwapSpace(core::getProcessHierarchy(), 0, &backingHandle, &swapHandle));
	helix::UniqueDescriptor backing{backingHandle};
	helix::UniqueDescriptor swapSpace{swapHandle};

	SwapDaemonState state;
	async::run(setSwapRegion(swapSpace, 0, swapPages, &state), helix::currentDispatcher);

	HelHandle memoryHandle;
	HEL_CHECK(helAllocateSwappableMemory(core::getProcessHierarchy(), swapSpace.getHandle(),
			viewPages * pageSize, 0, &memoryHandle));
	helix::UniqueDescriptor memory{memoryHandle};

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

// Swaps out private copies of CoW memory, before and after forking it.
// Even pages become private copies and odd pages keep the content of the source.
void testSwapCopyOnWrite() {
	HelHandle backingHandle, swapHandle;
	HEL_CHECK(helCreateSwapSpace(core::getProcessHierarchy(), 0, &backingHandle, &swapHandle));
	helix::UniqueDescriptor backing{backingHandle};
	helix::UniqueDescriptor swapSpace{swapHandle};

	SwapDaemonState state;
	async::run(setSwapRegion(swapSpace, 0, swapPages, &state), helix::currentDispatcher);

	HelHandle sourceHandle;
	HEL_CHECK(helAllocateMemory(core::getProcessHierarchy(), viewPages * pageSize, 0,
			nullptr, &sourceHandle));
	helix::UniqueDescriptor source{sourceHandle};

	// Fill the allocated memory with a pattern.
	void *window;
	HEL_CHECK(helMapMemory(source.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	for(size_t pg = 0; pg < viewPages; pg++)
		fillPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, 0);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	HelHandle parentHandle;
	HEL_CHECK(helCopyOnWrite(core::getProcessHierarchy(), swapSpace.getHandle(),
			source.getHandle(), 0, viewPages * pageSize, &parentHandle));
	helix::UniqueDescriptor parent{parentHandle};

	// The daemon services all requests from its own thread until finished.
	std::thread daemon{[&] {
		async::run(async::lambda([&]() -> async::result<void> {
			while(!state.finished.load(std::memory_order::acquire))
				co_await serveOnce(backing, &state);
		})(), helix::currentDispatcher);
	}};

	// Write a new pattern into the even pages, forcing a CoW.
	HEL_CHECK(helMapMemory(parent.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	for(size_t pg = 0; pg < viewPages; pg += 2)
		fillPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, 1);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	while(state.pagesWrittenBack < viewPages / 2)
		std::this_thread::sleep_for(std::chrono::milliseconds{10});

	// Under torture uncaching the pages are dropped within a few generation,
	// give the kernel some time.
	std::this_thread::sleep_for(std::chrono::milliseconds{1000});

	// Forking turns the swapped out copies into pages shared by parent and child.
	auto forkResult = async::run(helix_ng::forkMemory(
			helix::BorrowedDescriptor{core::getProcessHierarchy()}, parent),
			helix::currentDispatcher);
	HEL_CHECK(forkResult.error());
	auto child = forkResult.descriptor();

	// Writing to the child copies from the shared pages, which are swapped out.
	auto writtenBefore = state.pagesWrittenBack.load();
	HEL_CHECK(helMapMemory(child.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	for(size_t pg = 0; pg < viewPages; pg += 2)
		fillPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, 2);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	// Make sure the parent still sees the shared pages.
	HEL_CHECK(helMapMemory(parent.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead, &window));
	for(size_t pg = 0; pg < viewPages; pg++)
		assert(checkPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, pg % 2 ? 0 : 1));
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	while(state.pagesWrittenBack < writtenBefore + viewPages / 2)
		std::this_thread::sleep_for(std::chrono::milliseconds{10});
	std::this_thread::sleep_for(std::chrono::milliseconds{1000});

	// Fault the child's copies back in.
	HEL_CHECK(helMapMemory(child.getHandle(), kHelNullHandle, nullptr,
			0, viewPages * pageSize, kHelMapProtRead, &window));
	for(size_t pg = 0; pg < viewPages; pg++)
		assert(checkPattern(reinterpret_cast<uint8_t *>(window) + pg * pageSize, pg, pg % 2 ? 0 : 2));
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, viewPages * pageSize));

	// Copy one shared page in the parent, so that exactly one further
	// writeback request arrives after stop is set.
	state.stop.store(true, std::memory_order_relaxed);
	HEL_CHECK(helMapMemory(parent.getHandle(), kHelNullHandle, nullptr,
			0, pageSize, kHelMapProtRead | kHelMapProtWrite, &window));
	fillPattern(window, 0, 0xcafebabe);
	HEL_CHECK(helUnmapMemory(kHelNullHandle, window, pageSize));
	daemon.join();

	printf("kernel-tests: swapCopyOnWrite: %zu pages swapped out, %zu pages swapped in\n",
			state.pagesWrittenBack.load(std::memory_order_relaxed),
			state.pagesInitialized.load(std::memory_order_relaxed));
	if(!state.pagesInitialized.load(std::memory_order::relaxed)) {
		printf("kernel-tests: swapCopyOnWrite: swap-in path was NOT exercised"
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

DEFINE_TEST(swapCopyOnWrite, ([] {
	testSwapCopyOnWrite();
}))
