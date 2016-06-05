
#ifndef FRIGG_BRANCH_HPP
#define FRIGG_BRANCH_HPP

#include <frigg/chain-common.hpp>

namespace frigg {

template<typename ThenChainable, typename ElseChainable>
struct Branch {
public:
	template<typename P>
	using Signature = typename ThenChainable::template Signature<void()>;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
	private:	
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

		using ThenSignature = typename ThenChainable::template Signature<void()>;
		using ThenChain = typename ThenChainable::template Chain<void(), Resume<ThenSignature>>;
		using ElseSignature = typename ElseChainable::template Signature<void()>;
		using ElseChain = typename ElseChainable::template Chain<void(), Resume<ElseSignature>>;

	public:
		template<typename... E>
		Chain(const Branch &bp, E &&... emplace)
		: _thenChain(bp._thenChainable, this),
				_elseChain(bp._elseChainable, this),
				_next(forward<E>(emplace)...) { }
		
		Chain(Chain &&other) = delete;
		Chain &operator= (Chain &&other) = delete;

		void operator() (bool check) {
			if(check) {
				_thenChain();
			}else{
				_elseChain();
			}
		}

	private:
		ThenChain _thenChain;
		ElseChain _elseChain;
		Next _next;
	};

	Branch(ThenChainable then_chainable, ElseChainable else_chainable)
	: _thenChainable(move(then_chainable)),
			_elseChainable(move(else_chainable)) { }

private:
	ThenChainable _thenChainable;
	ElseChainable _elseChainable;
};

template<typename ThenChainable, typename ElseChainable>
struct CanSequence<Branch<ThenChainable, ElseChainable>>
: public TrueType { };

template<typename ThenChainable, typename ElseChainable>
Branch<ThenChainable, ElseChainable>
branch(ThenChainable then_chainable, ElseChainable else_chainable) {
	return Branch<ThenChainable, ElseChainable>(move(then_chainable),
			move(else_chainable));
}

} // namespace frigg

#endif // FRIGG_BRANCH_HPP

