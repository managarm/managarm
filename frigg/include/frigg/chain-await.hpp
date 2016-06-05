
#ifndef FRIGG_AWAIT_HPP
#define FRIGG_AWAIT_HPP

#include <frigg/callback.hpp>
#include <frigg/chain-common.hpp>

namespace frigg {

template<typename S, typename Functor>
struct Await;

template<typename... Results, typename Functor>
struct Await<void(Results...), Functor> {
	template<typename P>
	using Signature = void(Results...);

	template<typename P, typename Next>
	struct Chain;
	
	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
		template<typename... E>
		Chain(const Await &bp, E &&... emplace)
		: _functor(bp._functor), _next(forward<E>(emplace)...) { }

		void operator() (Args &&... args) {
			_functor(CALLBACK_MEMBER(this, &Chain::callback), forward<Args>(args)...);
		}

	private:
		void callback(Results... results) {
			_next(forward<Results>(results)...);
		}
		
		Functor _functor;
		Next _next;
	};

	Await(Functor functor)
	: _functor(move(functor)) { }

private:
	Functor _functor;
};

template<typename S, typename Functor>
struct CanSequence<Await<S, Functor>>
: public TrueType { };

template<typename S, typename Functor>
Await<S, Functor> await(Functor functor) {
	return Await<S, Functor>(move(functor));
}

} // namespace frigg

#endif // FRIGG_AWAIT_HPP

