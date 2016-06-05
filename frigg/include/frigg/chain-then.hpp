
#ifndef FRIGG_THEN_HPP
#define FRIGG_THEN_HPP

#include <frigg/chain-common.hpp>

namespace frigg {

template<typename First, typename Follow>
struct Then {
private:
	template<typename P>
	using FirstSignature = typename First::template Signature<P>;

public:
	template<typename P>
	using Signature = typename Follow::template Signature<FirstSignature<P>>;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
		using FollowChain = typename Follow::template Chain<FirstSignature<void(Args...)>, Next>;
		using FirstChain = typename First::template Chain<void(Args...), FollowChain>;

		template<typename... E>
		Chain(const Then &bp, E &&... emplace)
		: _firstChain(bp._first, bp._follow, frigg::forward<E>(emplace)...) { }

		void operator() (Args &&... args) {
			_firstChain(frigg::forward<Args>(args)...);
		}

	private:
		FirstChain _firstChain;
	};

	Then(First first, Follow follow)
	: _first(frigg::move(first)), _follow(frigg::move(follow)) { }

private:
	First _first;
	Follow _follow;
};

template<typename First, typename Follow>
struct CanSequence<Then<First, Follow>>
: public TrueType { };

template<typename T>
using EnableSequence = frigg::EnableIfT<CanSequence<T>::value>;

template<typename First, typename Follow,
		typename = EnableSequence<First>>
Then<First, Follow> operator+ (First first, Follow follow) {
	return Then<First, Follow>(frigg::move(first), frigg::move(follow));
}

template<typename First, typename Follow>
Then<First, Follow> then(First first, Follow follow) {
	return Then<First, Follow>(frigg::move(first), frigg::move(follow));
}

} // namespace frigg

#endif // FRIGG_THEN_HPP

