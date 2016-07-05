
#ifndef FRIGG_LIFT_HPP
#define FRIGG_LIFT_HPP

#include <frigg/traits.hpp>
#include <frigg/chain-common.hpp>

namespace frigg {

template<typename T>
struct SwitchOnResult {
	static constexpr bool nullary = false;
	using Signature = void(T);
};

template<>
struct SwitchOnResult<void> {
	static constexpr bool nullary = true;
	using Signature = void();
};

template<typename Functor>
struct LiftUnary {
private:
	template<typename P>
	struct Resolve;
	
	template<typename... Args>
	struct Resolve<void(Args...)> {
		using Result = ResultOf<Functor(Args...)>;

		static constexpr bool nullary = SwitchOnResult<Result>::nullary;
		using Signature = typename SwitchOnResult<Result>::Signature;
	};

public:
	template<typename P>
	using Signature = typename Resolve<P>::Signature;

private:
	template<typename... Args>
	using EnableNullary = EnableIfT<Resolve<void(Args...)>::nullary>;

	template<typename... Args>
	using EnableUnary = EnableIfT<!Resolve<void(Args...)>::nullary>;

public:
	template<typename P, typename Next, typename = void>
	struct Chain;
	
	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next, EnableNullary<Args...>> {
		template<typename... E>
		Chain(const LiftUnary &bp, E &&...emplace)
		: _functor(bp._functor), _next(forward<E>(emplace)...) { }

		void operator() (Args &&... args) {
			_functor(forward<Args>(args)...);
			_next();
		}

	private:
		Functor _functor;
		Next _next;
	};
	
	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next, EnableUnary<Args...>> {
		template<typename... E>
		Chain(const LiftUnary &bp, E &&...emplace)
		: _functor(bp._functor), _next(forward<E>(emplace)...) { }

		void operator() (Args &&... args) {
			_next(_functor(forward<Args>(args)...));
		}

	private:
		Functor _functor;
		Next _next;
	};

	LiftUnary(Functor functor)
	: _functor(move(functor)) { }

private:
	Functor _functor;
};

template<typename Functor>
struct CanSequence<LiftUnary<Functor>>
: public TrueType { };

template<typename Functor>
LiftUnary<Functor> lift(Functor functor) {
	return LiftUnary<Functor>(move(functor));
}

} // namespace frigg

#endif // FRIGG_LIFT_HPP

