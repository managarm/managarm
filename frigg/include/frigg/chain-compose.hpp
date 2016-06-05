
#ifndef FRIGG_COMPOSE_HPP
#define FRIGG_COMPOSE_HPP

#include <assert.h>

#include <frigg/chain-common.hpp>
#include <frigg/chain-run.hpp>

namespace frigg {

template<typename Functor>
struct ComposeDynamic {
private:
	template<typename P>
	struct ResolveSignature;
	
	template<typename... Args>
	struct ResolveSignature<void(Args...)> {
		using Type = typename ResultOf<Functor(Args...)>::template Signature<void()>;
	};

public:
	template<typename P>
	using Signature = typename ResolveSignature<P>::Type;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
	private:
		using ComposedChainable = ResultOf<Functor(Args...)>;
		using ComposedSignature = typename ComposedChainable::template Signature<void()>;
	
		template<typename S>
		struct Resume;
		
		template<typename... Results>
		struct Resume<void(Results...)> {
			Resume(Chain *chain)
			: _chain(chain) { }

			void operator() (Results &&... results) {
				_chain->_next(forward<Results>(results)...);
			}

		private:
			Chain *_chain;
		};

	public:
		template<typename... E>
		Chain(const ComposeDynamic &bp, E &&... emplace)
		: _functor(bp._functor), _next(forward<E>(emplace)...) { }
		
		void operator() (Args &&... args) {
			auto chainable = _functor(forward<Args>(args)...);
			run(chainable, Resume<ComposedSignature>(this));
		}

	private:
		Functor _functor;
		Next _next;
	};

	ComposeDynamic(Functor functor)
	: _functor(move(functor)) { }

private:
	Functor _functor;
};

template<typename Functor>
struct CanSequence<ComposeDynamic<Functor>>
: public TrueType { };

template<typename Functor>
struct ComposeOnce {
private:
	template<typename P>
	struct ResolveSignature;
	
	template<typename... Args>
	struct ResolveSignature<void(Args...)> {
		using Type = typename ResultOf<Functor(Args...)>::template Signature<void()>;
	};

public:
	template<typename P>
	using Signature = typename ResolveSignature<P>::Type;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
	private:
		using ComposedChainable = ResultOf<Functor(Args...)>;
		using ComposedChain = typename ComposedChainable::template Chain<void(), Next>;
	
	public:
		template<typename... E>
		Chain(const ComposeOnce &bp, E &&... emplace)
		: _constructData(bp._functor, forward<E>(emplace)...),
				_hasConstructData(true), _hasComposedChain(false) { }
	
		~Chain() {
			if(_hasConstructData)
				_constructData.~ConstructData();
			if(_hasComposedChain)
				_composedChain.~ComposedChain();
		}

		void operator() (Args &&... args) {
			assert(_hasConstructData);

			// destruct the construction data to make room for the chainable
			Functor functor = move(_constructData.functor);
			Next next = move(_constructData.next);
			_hasConstructData = false;
			_constructData.~ConstructData();

			// construct the chain in-place and invoke it
			auto chainable = functor(forward<Args>(args)...);
			auto chain = new (&_composedChain) ComposedChain(chainable, move(next));
			_hasComposedChain = true;

			(*chain)();
		}

	private:
		struct ConstructData {
			template<typename... E>
			ConstructData(Functor functor, E &&... emplace)
			: functor(move(functor)), next(forward<E>(emplace)...) { }

			Functor functor;
			Next next;
		};

		union {
			ConstructData _constructData;
			ComposedChain _composedChain;
		};

		bool _hasConstructData, _hasComposedChain;
	};

	ComposeOnce(Functor functor)
	: _functor(move(functor)) { }

private:
	Functor _functor;
};

template<typename Functor>
struct CanSequence<ComposeOnce<Functor>>
: public TrueType { };

struct Once { };

constexpr Once once;

template<typename Functor>
auto compose(Functor functor) {
	return ComposeDynamic<Functor>(move(functor));
}

template<typename Functor>
auto compose(Functor functor, Once tag) {
	return ComposeOnce<Functor>(move(functor));
}

} // namespace frigg

#endif // FRIGG_COMPOSE_HPP

