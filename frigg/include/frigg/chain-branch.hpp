
#ifndef FRIGG_BRANCH_HPP
#define FRIGG_BRANCH_HPP

#include <frigg/chain-common.hpp>

namespace frigg {

template<typename CheckChainable, typename ThenChainable>
struct IfThen {
public:
	template<typename P>
	using Signature = typename ThenChainable::template Signature<void()>;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
	private:
		struct Check {
			Check(Chain *chain)
			: _chain(chain) { }

			void operator() (bool check) {
				if(check) {
					_chain->_thenChain();
				}else{
					_chain->_next();
				}
			}

		private:
			Chain *_chain;
		};
		
		using CheckChain = typename CheckChainable::template Chain<void(), Check>;

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

	public:
		template<typename... E>
		Chain(const IfThen &bp, E &&... emplace)
		: _checkChain(bp._checkChainable, this),
				_thenChain(bp._thenChainable, this),
				_next(forward<E>(emplace)...) { }
		
		template<typename... E>
		Chain(IfThen &&bp, E &&... emplace)
		: _checkChain(move(bp._checkChainable), this),
				_thenChain(move(bp._thenChainable), this),
				_next(forward<E>(emplace)...) { }
		
		Chain(Chain &&other) = delete;

		Chain &operator= (Chain &&other) = delete;

		void operator() () {
			_checkChain();
		}

	private:
		CheckChain _checkChain;
		ThenChain _thenChain;
		Next _next;
	};

	IfThen(CheckChainable check_chainable, ThenChainable then_chainable)
	: _checkChainable(move(check_chainable)),
			_thenChainable(move(then_chainable)) { }

private:
	CheckChainable _checkChainable;
	ThenChainable _thenChainable;
};

template<typename CheckChainable, typename ThenChainable>
struct CanSequence<IfThen<CheckChainable, ThenChainable>>
: public TrueType { };

template<typename CheckChainable, typename ThenChainable>
IfThen<CheckChainable, ThenChainable>
ifThen(CheckChainable check_chainable, ThenChainable then_chainable) {
	return IfThen<CheckChainable, ThenChainable>(move(check_chainable),
			move(then_chainable));
}

template<typename CheckChainable, typename ThenChainable, typename ElseChainable>
struct IfThenElse {
public:
	template<typename P>
	using Signature = typename ThenChainable::template Signature<void()>;

	template<typename P, typename Next>
	struct Chain;

	template<typename... Args, typename Next>
	struct Chain<void(Args...), Next> {
	private:
		struct Check {
			Check(Chain *chain)
			: _chain(chain) { }

			void operator() (bool check) {
				if(check) {
					_chain->_thenChain();
				}else{
					_chain->_elseChain();
				}
			}

		private:
			Chain *_chain;
		};
		
		using CheckChain = typename CheckChainable::template Chain<void(), Check>;

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
		Chain(const IfThenElse &bp, E &&... emplace)
		: _checkChain(bp._checkChainable, this),
				_thenChain(bp._thenChainable, this),
				_elseChain(bp._elseChainable, this),
				_next(forward<E>(emplace)...) { }
		
		template<typename... E>
		Chain(IfThenElse &&bp, E &&... emplace)
		: _checkChain(move(bp._checkChainable), this),
				_thenChain(move(bp._thenChainable), this),
				_elseChain(move(bp._elseChainable), this),
				_next(forward<E>(emplace)...) { }
		
		Chain(Chain &&other) = delete;

		Chain &operator= (Chain &&other) = delete;

		void operator() () {
			_checkChain();
		}

	private:
		CheckChain _checkChain;
		ThenChain _thenChain;
		ElseChain _elseChain;
		Next _next;
	};

	IfThenElse(CheckChainable check_chainable,
			ThenChainable then_chainable, ElseChainable else_chainable)
	: _checkChainable(move(check_chainable)),
			_thenChainable(move(then_chainable)),
			_elseChainable(move(else_chainable)) { }

private:
	CheckChainable _checkChainable;
	ThenChainable _thenChainable;
	ElseChainable _elseChainable;
};

template<typename CheckChainable, typename ThenChainable, typename ElseChainable>
struct CanSequence<IfThenElse<CheckChainable, ThenChainable, ElseChainable>>
: public TrueType { };

template<typename CheckChainable, typename ThenChainable, typename ElseChainable>
IfThenElse<CheckChainable, ThenChainable, ElseChainable>
ifThenElse(CheckChainable check_chainable,
		ThenChainable then_chainable, ElseChainable else_chainable) {
	return IfThenElse<CheckChainable, ThenChainable, ElseChainable>(move(check_chainable),
			move(then_chainable), move(else_chainable));
}

} // namespace frigg

#endif // FRIGG_BRANCH_HPP

