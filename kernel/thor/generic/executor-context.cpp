#include <thor-internal/executor-context.hpp>
#include <thor-internal/rcu.hpp>

namespace thor {

ExecutorContext *ExecutorContext::create() {
	return frg::construct<ExecutorContext>(*kernelAlloc);
}

void ExecutorContext::retire(ExecutorContext *ctx) {
	submitRcu(ctx, [] (RcuCallable *base) {
		auto derived = static_cast<ExecutorContext *>(base);
		frg::destruct(*kernelAlloc, derived);
	});
}

} // namespace thor
