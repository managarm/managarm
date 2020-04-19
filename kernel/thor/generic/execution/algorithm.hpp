#pragma once

#include <frg/tuple.hpp>
#include "basics.hpp"
#include "cancellation.hpp"

template<typename Sender, typename Receiver>
struct connect_helper {
	using operation = execution::operation_t<Sender, Receiver>;

	operator operation () {
		return connect(std::move(s), std::move(r));
	}

	Sender s;
	Receiver r;
};

template<typename Sender, typename Receiver>
connect_helper<Sender, Receiver> make_connect_helper(Sender s, Receiver r) {
	return {std::move(s), std::move(r)};
}

template<typename Receiver, typename Tuple, typename S>
struct race_and_cancel_operation;

template<typename... Functors>
struct race_and_cancel_sender {
	template<typename Receiver>
	friend race_and_cancel_operation<Receiver, frg::tuple<Functors...>,
			std::index_sequence_for<Functors...>>
	connect(race_and_cancel_sender s, Receiver r) {
		return {std::move(s), std::move(r)};
	}

	frg::tuple<Functors...> fs;
};

template<typename Receiver, typename... Functors, size_t... Is>
struct race_and_cancel_operation<Receiver, frg::tuple<Functors...>, std::index_sequence<Is...>> {
private:
	using functor_tuple = frg::tuple<Functors...>;

	template<size_t I>
	struct internal_receiver {
		internal_receiver(race_and_cancel_operation *self)
		: self_{self} { }

		void set_done() {
			auto n = self_->n_done_.fetch_add(1, std::memory_order_acq_rel);
			if(!n) {
				for(unsigned int j = 0; j < sizeof...(Is); ++j)
					if(j != I)
						self_->cs_[j].cancel();
			}
			if(n + 1 == sizeof...(Is))
				self_->r_.set_done();
		}

	private:
		race_and_cancel_operation *self_;
	};

	template<size_t I>
	using internal_sender = std::invoke_result_t<
		typename std::tuple_element<I, functor_tuple>::type,
		cancellation_token
	>;

	template<size_t I>
	using internal_operation = execution::operation_t<internal_sender<I>, internal_receiver<I>>;

	using operation_tuple = frg::tuple<internal_operation<Is>...>;

	auto make_operations_tuple(race_and_cancel_sender<Functors...> s) {
		return frg::make_tuple(
			make_connect_helper(
				(s.fs.template get<Is>())(cancellation_token{cs_[Is]}),
				internal_receiver<Is>{this}
			)...
		);
	}

public:
	race_and_cancel_operation(race_and_cancel_sender<Functors...> s, Receiver r)
	: r_{std::move(r)}, ops_{make_operations_tuple(std::move(s))}, n_done_{0} { }

	void start() {
		(ops_.template get<Is>().start(), ...);
	}

private:
	Receiver r_;
	operation_tuple ops_;
	cancellation_event cs_[sizeof...(Is)];
	std::atomic<unsigned int> n_done_;
};

template<typename... Functors>
execution::sender_awaiter<race_and_cancel_sender<Functors...>>
operator co_await(race_and_cancel_sender<Functors...> s) {
	return {std::move(s)};
}

template<typename... Functors>
race_and_cancel_sender<Functors...> race_and_cancel(Functors... fs) {
	return {{fs...}};
}
