
namespace frigg {
namespace async {

namespace lambda_impl {

template<typename OperatorType>
struct LambdaPtrTraits;

template<typename T, typename R, typename TheContext,
		typename CallbackR, typename... CallbackArgs, typename... OtherArgs>
struct LambdaPtrTraits<R (T::*) (TheContext &, util::Callback<CallbackR(CallbackArgs...)>, OtherArgs...) const> {
	typedef TheContext Context;
	typedef util::Callback<CallbackR(CallbackArgs...)> Callback;
	typedef TypePack<CallbackArgs...> OutputPack;
	typedef TypePack<OtherArgs...> InputPack;
};

template<typename Functor, typename Context,
		typename InputPack, typename OutputPack>
struct Lambda;

template<typename Functor, typename TheContext,
		typename... Inputs, typename... Outputs>
struct Lambda<Functor, TheContext, TypePack<Inputs...>, TypePack<Outputs...>> {
	typedef TheContext Context;
	typedef TypePack<Inputs...> InputPack;
	typedef TypePack<Outputs...> OutputPack;
	typedef typename util::CallbackFromPack<void, OutputPack>::Type Callback;
	
	struct Closure {
		Closure(const Lambda &element, Context &context, Callback callback)
		: functor(element.functor), context(context), callback(callback) { }
		
		void operator() (Inputs... inputs) {
			functor(context, callback, inputs...);
		}

		Functor functor;
		Context &context;
		Callback callback;
	};

	Lambda(Functor functor) : functor(functor) { }

	Functor functor;
};

} // namespace lambda_impl

template<typename Functor>
auto lambda(Functor functor) {
	typedef decltype(&Functor::operator()) Operator;
	typedef lambda_impl::LambdaPtrTraits<Operator> Traits;
	typedef typename Traits::Context Context;
	typedef typename Traits::OutputPack OutputPack;
	typedef typename Traits::InputPack InputPack;

	return lambda_impl::Lambda<Functor, Context, InputPack, OutputPack>(functor);
}

} } // namespace frigg::async

