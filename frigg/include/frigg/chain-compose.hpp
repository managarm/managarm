
#ifndef FRIGG_COMPOSE_HPP
#define FRIGG_COMPOSE_HPP

#include <assert.h>

#include <frigg/traits.hpp>
#include <frigg/tuple.hpp>
#include <frigg/chain-common.hpp>
#include <frigg/chain-run.hpp>

namespace frigg {

template<typename Functor, typename... T>
struct Compose {
private:
	template<typename P>
	struct ResolveSignature;
	
	template<typename... Args>
	struct ResolveSignature<void(Args...)> {
		using Type = typename ResultOf<Functor(Args..., T *...)>::template Signature<void()>;
	};

public:
	template<typename P>
	using Signature = typename ResolveSignature<P>::Type;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
	private:
		struct Resume {
			Resume(Chain *chain)
			: _chain(chain) { }

			template<typename... Results>
			void operator() (Results &&... results) {
				assert(_chain->_hasComposedChain);
				_chain->_composedChain.~ComposedChain();
				_chain->_hasComposedChain = false;

				_chain->_next(forward<Results>(results)...);
			}

		private:
			Chain *_chain;
		};

		using ComposedChainable = ResultOf<Functor(Args..., T *...)>;
		using ComposedChain = typename ComposedChainable::template Chain<void(), Resume>;
	
	public:
		template<typename... E>
		Chain(Compose bp, E &&... emplace)
		: _functor(move(bp._functor)), _context(move(bp._context)),
				_next(forward<E>(emplace)...), _hasComposedChain(false) { }

		Chain(const Chain &other) = delete;

		~Chain() {
			assert(!_hasComposedChain);
		}

		Chain &operator= (Chain other) = delete;

		void operator() (Args &&... args) {
			invoke(IndexSequenceFor<T...>(), forward<Args>(args)...);
		}

	private:
		template<size_t... I>
		void invoke(IndexSequence<I...>, Args &&... args) {
			assert(!_hasComposedChain);

			// construct the chain in-place and invoke it
			auto chainable = _functor(forward<Args>(args)..., &_context.template get<I>()...);
			new (&_composedChain) ComposedChain(move(chainable), this);
			_hasComposedChain = true;

			_composedChain();
		}

		Functor _functor;
		Tuple<T...> _context;
		Next _next;

		union {
			ComposedChain _composedChain;
		};

		bool _hasComposedChain;
	};

	Compose(Functor functor, T... contexts)
	: _functor(move(functor)), _context(move(contexts)...) { }

private:
	Functor _functor;
	Tuple<T...> _context;
};

template<typename Functor, typename... T>
struct CanSequence<Compose<Functor, T...>>
: public TrueType { };

template<typename Functor, typename... T>
Compose<Functor, T...> compose(Functor functor, T... contexts) {
	return Compose<Functor, T...>(move(functor), move(contexts)...);
}

} // namespace frigg

#endif // FRIGG_COMPOSE_HPP

