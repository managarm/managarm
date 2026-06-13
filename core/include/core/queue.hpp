#pragma once

#include <cstddef>

/**
 * Represents an index in a queue.
 *
 * This class serves to encapsulate the peculiarities of queue indices, especially their
 * wrap-around modular arithmetic.
 */
struct QueueIndex {
	constexpr QueueIndex(size_t value, size_t mod) noexcept : index_(value), mod_(mod) {
		[[assume(mod_ > 0)]];
		[[assume(index_ < mod_)]];
	}

	constexpr operator size_t() const noexcept { return index_; }

	constexpr size_t operator()() const noexcept { return index_; }

	constexpr QueueIndex operator+(int v) const noexcept {
		[[assume(mod_ > 0)]];
		QueueIndex tmp{*this};

		if (v >= 0) {
			tmp.index_ = (tmp.index_ + (static_cast<size_t>(v) % tmp.mod_)) % tmp.mod_;
		} else {
			size_t abs_v = static_cast<size_t>(-v) % tmp.mod_;
			tmp.index_ = (tmp.mod_ + tmp.index_ - abs_v) % tmp.mod_;
		}
		return tmp;
	}

	constexpr QueueIndex &operator++() noexcept {
		[[assume(mod_ > 0)]];
		index_ = (index_ + 1uz) % mod_;
		return *this;
	}

	constexpr QueueIndex operator++(int) noexcept {
		auto temp = *this;
		++*this;
		return temp;
	}

	constexpr QueueIndex &operator--() noexcept {
		[[assume(mod_ > 0)]];
		index_ = (mod_ + index_ - 1uz) % mod_;
		return *this;
	}

	constexpr QueueIndex operator--(int) noexcept {
		auto temp = *this;
		--(*this);
		return temp;
	}

	constexpr bool operator==(const QueueIndex &other) const noexcept = default;

private:
	size_t index_;
	size_t mod_;
};
