
#ifndef FRIGG_CLOSURE_HPP
#define FRIGG_CLOSURE_HPP

#include <frigg/traits.hpp>

namespace frigg {

template<typename Chainable, typename Allocator, typename Finally>
struct Closure {
	template<typename S>
	struct Complete;

	template<typename... Results>
	struct Complete<void(Results...)> {
		Complete(Closure *closure)
		: _closure(closure) { }

		void operator() (Results &&... results) {
			_closure->finally(frigg::forward<Results>(results)...);
			frigg::destruct(*_closure->allocator, _closure);
		}

	private:
		Closure *_closure;
	};

	using Signature = typename Chainable::template Signature<void()>;
	using Continuation = typename Chainable::template Chain<void(), Complete<Signature>>;

	Closure(Chainable chainable, Allocator *allocator, Finally finally)
	: continuation(frigg::move(chainable), this),
			allocator(allocator), finally(frigg::move(finally)) { }

	Continuation continuation;
	Allocator *allocator;
	Finally finally;
};

template<typename Chainable, typename Allocator, typename Finally>
void run(Chainable chainable, Allocator *allocator, Finally finally) {
	auto closure = frigg::construct<Closure<Chainable, Allocator, Finally>>(*allocator,
			frigg::move(chainable), allocator, frigg::move(finally));
	closure->continuation();
}

template<typename Chainable>
void run(Chainable chainable) {
	run(frigg::move(chainable), [] () { });
}

} // namespace frigg

#endif // FRIGG_CLOSURE_HPP


