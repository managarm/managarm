#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/scope_exit.hpp>
#include <frg/span.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/int-call.hpp>
#include <thor-internal/ipl.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/kernel-ostrace.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ostrace.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

namespace ostrace {

std::atomic<bool> available{false};

namespace {

constexpr size_t pageSize = 32 * 1024;
constexpr size_t pagesPerCpu = 4;
// Time (in nanoseconds) after the first event of a page at which the page is sealed.
// This ensures that pages are sealed (and flushed) even if no further events come in.
constexpr uint64_t maxLatency = 50'000'000;

enum class PageState {
	free,
	active,
	sealed,
	flushing
};

struct Page {
	char *buffer = nullptr;
	size_t size = 0;
	PageState state = PageState::free;
	// Sealed pages are flushed (and discarded) in this order.
	uint64_t sealOrder = 0;
	// The page contains the events firstEvent to firstEvent + numEvents - 1.
	uint64_t firstEvent = 0;
	uint64_t numEvents = 0;
	// Time at which the sealer seals the page.
	uint64_t deadline = 0;
};

struct CpuState {
	// Taken with IRQs disabled.
	frg::ticket_spinlock mutex;
	Page pages[pagesPerCpu];
	Page *active = nullptr;
	// Bitmap indexed by term ID. Whether the active page already contains a Definition of the term.
	uint64_t *defined = nullptr;
	uint64_t nextSealOrder = 0;
	// Number of the next event. Dropped events also consume a number, such that readers see a gap.
	uint64_t nextEvent = 0;
};

// Size of CpuState::defined in words.
size_t definedWords = 0;

constinit SelfIntEvent sealerEvent;
constinit SelfIntEvent flusherEvent;

void seal(CpuState &state, Page *page) {
	assert(page == state.active);
	page->state = PageState::sealed;
	page->sealOrder = state.nextSealOrder++;
	state.active = nullptr;
}

Page *oldestSealed(CpuState &state) {
	Page *oldest = nullptr;
	for(auto &page : state.pages) {
		if(page.state != PageState::sealed)
			continue;
		if(!oldest || page.sealOrder < oldest->sealOrder)
			oldest = &page;
	}
	return oldest;
}

Page *acquirePage(CpuState &state) {
	Page *page = nullptr;
	for(auto &candidate : state.pages) {
		if(candidate.state == PageState::free) {
			page = &candidate;
			break;
		}
	}
	// Discard the oldest sealed page. Readers see a gap in the event numbers of the CPU.
	if(!page)
		page = oldestSealed(state);
	if(!page)
		return nullptr;

	page->state = PageState::active;
	page->size = 0;
	page->numEvents = 0;
	page->deadline = getClockNanos() + maxLatency;
	memset(state.defined, 0, definedWords * sizeof(uint64_t));
	state.active = page;
	return page;
}

extern PerCpu<CpuState> cpuState;
THOR_DEFINE_PERCPU(cpuState);

// Seals active pages once their deadline passes.
coroutine<void> sealExpired() {
	while(true) {
		sealerEvent.clear();

		auto now = getClockNanos();
		frg::optional<uint64_t> deadline;
		bool mustWakeFlusher = false;
		for(size_t cpu = 0; cpu < getCpuCount(); ++cpu) {
			auto &state = cpuState.getFor(cpu);
			auto irqLock = frg::guard(&irqMutex());
			auto lock = frg::guard(&state.mutex);

			auto *page = state.active;
			if(!page)
				continue;
			if(page->deadline <= now) {
				seal(state, page);
				mustWakeFlusher = true;
			}else if(!deadline || page->deadline < *deadline) {
				deadline = page->deadline;
			}
		}
		if(mustWakeFlusher) {
			StatelessIrqLock irqLock;
			flusherEvent.raise();
		}

		// Pages that are opened while we sleep have later deadlines than the one we sleep for.
		if(deadline) {
			co_await generalTimerEngine()->sleep(*deadline);
		}else{
			co_await sealerEvent.wait();
		}
	}
}

// Writes sealed pages to the ring.
coroutine<void> flush() {
	while(true) {
		flusherEvent.clear();

		for(size_t cpu = 0; cpu < getCpuCount(); ++cpu) {
			auto &state = cpuState.getFor(cpu);

			// Bounded such that a busy CPU does not starve the others.
			// Pages that remain were sealed after clear(), so a wakeup is pending for them.
			for(size_t i = 0; i < pagesPerCpu; ++i) {
				Page *page;
				{
					auto irqLock = frg::guard(&irqMutex());
					auto lock = frg::guard(&state.mutex);

					page = oldestSealed(state);
					if(!page)
						break;
					page->state = PageState::flushing;
				}

				commitFrame(0, cpu, page->firstEvent, page->numEvents, {page->buffer, page->size});

				auto irqLock = frg::guard(&irqMutex());
				auto lock = frg::guard(&state.mutex);
				page->state = PageState::free;
			}
		}

		co_await flusherEvent.wait();
	}
}

constinit ItemId nextId = 1;

} // anonymous namespace

Term::Term(const char *name)
: name_{name} {
	// setup() sizes the per-CPU bitmaps by the number of terms.
	assert(!available.load(std::memory_order_relaxed));
	id_ = nextId++;

	// Serialized upfront since emit() cannot allocate.
	managarm::ostrace::Definition<KernelAlloc> definition{*kernelAlloc};
	definition.set_id(id_);
	definition.set_name(frg::string<KernelAlloc>{*kernelAlloc, name_});
	auto tailSize = definition.size_of_tail();
	auto *buffer = static_cast<char *>(kernelAlloc->allocate(8 + tailSize));
	bool encodeSuccess = bragi::write_head_tail(definition,
			frg::span<char>(buffer, 8),
			frg::span<char>(buffer + 8, tailSize));
	assert(encodeSuccess);
	definition_ = {buffer, 8 + tailSize};
}

void setup() {
	definedWords = (nextId + 63) / 64;

	// Pages are allocated upfront since emit() cannot allocate.
	for(size_t cpu = 0; cpu < getCpuCount(); ++cpu) {
		auto &state = cpuState.getFor(cpu);
		for(auto &page : state.pages)
			page.buffer = static_cast<char *>(kernelAlloc->allocate(pageSize));
		state.defined = static_cast<uint64_t *>(
				kernelAlloc->allocate(definedWords * sizeof(uint64_t)));
	}

	KernelFiber::run([] {
		spawnOnWorkQueue(*kernelAlloc, thisFiber()->associatedWorkQueue().lock(), sealExpired());
		KernelFiber::asyncBlockCurrent(flush());
	});

	available.store(true, std::memory_order_release);
}

void writeEvent(frg::span<const Term *const> terms, size_t size,
		frg::function_ref<void(frg::span<char>)> write) {
	// NMIs could interrupt a writer while it holds the mutex.
	assert(contextIpl() < ipl::maximal);

	bool mustWakeSealer = false;
	bool mustWakeFlusher = false;
	auto irqLock = frg::guard(&irqMutex());
	// Declared before the lock, such that the wakeups happen after the lock is dropped.
	frg::scope_exit wakeOnExit{[&] {
		if(mustWakeSealer)
			sealerEvent.raise();
		if(mustWakeFlusher)
			flusherEvent.raise();
	}};
	auto &state = cpuState.get();
	auto lock = frg::guard(&state.mutex);

	auto event = state.nextEvent++;

	// Upper bound on the size of the Definitions that the page lacks.
	// Using it for the fit checks avoids determining the missing Definitions up front.
	size_t maxDefinitionsSize = 0;
	for(auto *term : terms)
		maxDefinitionsSize += term->definition().size();

	auto *page = state.active;
	if(page && page->size + maxDefinitionsSize + size > pageSize) {
		seal(state, page);
		mustWakeFlusher = true;
		page = nullptr;
	}
	if(!page) {
		// Only drop events that do not fit into any page once the active page is sealed,
		// such that the dropped event is a gap between pages.
		if(maxDefinitionsSize + size > pageSize)
			return;
		page = acquirePage(state);
		if(!page)
			return;
		mustWakeSealer = true;
	}

	for(auto *term : terms) {
		auto id = term->id();
		auto bit = uint64_t{1} << (id % 64);
		if(state.defined[id / 64] & bit)
			continue;
		auto definition = term->definition();
		memcpy(page->buffer + page->size, definition.data(), definition.size());
		page->size += definition.size();

		// Setting the bit right away also skips terms that the event uses more than once.
		state.defined[id / 64] |= bit;
	}

	write({page->buffer + page->size, size});
	page->size += size;
	if(!page->numEvents)
		page->firstEvent = event;
	++page->numEvents;
}

} // namespace ostrace

} // namespace thor
