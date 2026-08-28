#pragma once

#include <cstddef>
#include <utility>

#include <async/counting-semaphore.hpp>
#include <async/result.hpp>

namespace blockfs {

// Bounds the memory that in-flight manage requests pin.
// This is a backstop. More accurate backpressure handling should happen at the BlockDevice level.
//
// A budget belongs to one thread, so requests must be released on the thread that reserved them.
struct ServiceBudget {
	// Move-only handle to budget reserved by acquire(); releases automatically on destruction.
	struct Token {
		Token() = default;

		Token(const Token &) = delete;

		Token(Token &&other) : Token{} {
			swap(*this, other);
		}

		Token &operator=(Token other) {
			swap(*this, other);
			return *this;
		}

		~Token() {
			if(budget_)
				budget_->release_(writeback_, numPages_);
		}

		friend void swap(Token &a, Token &b) {
			using std::swap;
			swap(a.budget_, b.budget_);
			swap(a.writeback_, b.writeback_);
			swap(a.numPages_, b.numPages_);
		}

		size_t numPages() const { return numPages_; }

	private:
		friend struct ServiceBudget;

		Token(ServiceBudget *budget, bool writeback, size_t numPages)
		: budget_{budget}, writeback_{writeback}, numPages_{numPages} { }

		ServiceBudget *budget_ = nullptr;
		bool writeback_ = false;
		size_t numPages_ = 0;
	};

	explicit ServiceBudget(size_t numPages);

	// Reserves budget for a request of the given length and returns a token that releases it.
	// Requests are clamped to the maximal budget; hence, very large requests run alone.
	async::result<Token> acquire(bool writeback, size_t length);

private:
	void release_(bool writeback, size_t numPages);

	// Overall budget in pages.
	size_t capacity_;
	// Pages of the budget that writeback may occupy. Reads can always take the full budget.
	size_t writebackCapacity_;
	async::counting_semaphore semaphore_;
	async::counting_semaphore writebackSemaphore_;
};

// One budget covers all servicing because no servicer waits for another one while holding budget.
// This means that all metadata access must be done
// *before* servicing budget is acquired for operations that rely on that metadata.
ServiceBudget &servicingBudget();

} // namespace blockfs
