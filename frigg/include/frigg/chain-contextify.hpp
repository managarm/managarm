
#ifndef FRIGG_CONTEXTIFY_HPP
#define FRIGG_CONTEXTIFY_HPP

namespace frigg {

template<typename Functor, typename T>
struct Contextify {
private:
	template<typename P>
	struct ResolveSignature;
	
	template<typename... Args>
	struct ResolveSignature<void(Args...)> {
		using Type = typename ResultOf<Functor(T *)>::template Signature<void(Args...)>;
	};

public:
	template<typename P>
	using Signature = typename ResolveSignature<P>::Type;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
		using ContextChainable = ResultOf<Functor(T *)>;
		using ContextChain = typename ContextChainable::template Chain<void(Args...), Next>;

		template<typename... E>
		Chain(const Contextify &bp, E &&... emplace)
		: _context(bp._context),
				_contextChain(bp._functor(&_context), frigg::forward<E>(emplace)...) { }
		
		void operator() (Args... args) {
			_contextChain(frigg::forward<Args>(args)...);
		}

	private:
		T _context;
		ContextChain _contextChain;
	};

	Contextify(Functor functor, T context)
	: _functor(functor), _context(context) { }

private:
	Functor _functor;
	T _context;
};

template<typename Functor, typename T>
auto contextify(Functor functor, T context) {
	return Contextify<Functor, T>(frigg::move(functor), frigg::move(context));
}

} // namespace frigg

#endif // FRIGG_CONTEXTIFY_HPP


