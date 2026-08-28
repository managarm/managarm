#include <algorithm>
#include <cstdint>

#include "service-budget.hpp"

namespace blockfs {

namespace {
	constexpr uint32_t pageShift = 12;
	// Pages that one thread may have pinned, i.e. 4 MiB.
	constexpr size_t budgetPages = 1024;
	// Share of a budget that writeback may occupy.
	constexpr size_t writebackNumerator = 3;
	constexpr size_t writebackDenominator = 4;
}

ServiceBudget::ServiceBudget(size_t numPages)
: capacity_{numPages},
		writebackCapacity_{numPages * writebackNumerator / writebackDenominator},
		semaphore_{capacity_}, writebackSemaphore_{writebackCapacity_} { }

async::result<ServiceBudget::Token> ServiceBudget::acquire(bool writeback, size_t length) {
	auto numPages = (length + (size_t{1} << pageShift) - 1) >> pageShift;
	numPages = std::min(numPages, writeback ? writebackCapacity_ : capacity_);

	// Writebacks take *both* the writeback budget and the overall budget.
	// Initializations only take the overall budget.
	if(writeback)
		co_await writebackSemaphore_.async_acquire(numPages);
	co_await semaphore_.async_acquire(numPages);
	co_return Token{this, writeback, numPages};
}

void ServiceBudget::release_(bool writeback, size_t numPages) {
	semaphore_.release(numPages);
	if(writeback)
		writebackSemaphore_.release(numPages);
}

ServiceBudget &servicingBudget() {
	thread_local ServiceBudget budget{budgetPages};
	return budget;
}

} // namespace blockfs
