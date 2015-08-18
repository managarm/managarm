
namespace frigg {
namespace async {

namespace context_impl {

template<typename FieldPtr, typename Delegate, typename InputPack>
struct SubContext;

template<typename TheContext, typename InnerContext, typename Delegate,
		typename... Inputs>
struct SubContext<InnerContext TheContext::*, Delegate, traits::TypePack<Inputs...>> {
	typedef InnerContext TheContext::*FieldPtr;

	typedef TheContext Context;
	typedef traits::TypePack<Inputs...> InputPack;
	typedef typename Delegate::OutputPack OutputPack;
	typedef typename util::CallbackFromPack<void, OutputPack>::Type Callback;

	SubContext(FieldPtr field_ptr, Delegate delegate)
	: fieldPtr(field_ptr), delegate(delegate) { }

	struct Closure {
		Closure(const SubContext &element, Context &context, Callback callback)
		: delegateClosure(element.delegate, context.*(element.fieldPtr), callback) { }

		void operator() (Inputs... inputs) {
			delegateClosure(inputs...);
		}

		typename Delegate::Closure delegateClosure;
	};
	
	FieldPtr fieldPtr;
	Delegate delegate;
};

} // namespace contex_impl

template<typename FieldPtr, typename Delegate>
auto subContext(FieldPtr field_ptr, Delegate delegate) {
	typedef context_impl::SubContext<FieldPtr, Delegate,
			typename Delegate::InputPack> SubContext;
	return SubContext(field_ptr, delegate);
}

} } // namespace frigg::async

