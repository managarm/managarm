
namespace frigg {
namespace async {

namespace seq_impl {

template<typename... Elements>
struct RecursiveElements;

template<typename Element, typename... Tail>
struct RecursiveElements<Element, Tail...> {
	RecursiveElements(const Element &element, const Tail &... tail)
	: element(element), tail(tail...) { }
	
	Element element;
	RecursiveElements<Tail...> tail;
};

template<>
struct RecursiveElements<> { };

template<typename ElementPack, typename CurrentInputPack, typename CurrentOutputPack, typename LastOutputPack>
struct RecursiveClosure;

// if there is another element, we pass an artificial callback
template<typename Element, typename Next, typename... Tail,
		typename... CurrentInputs, typename... CurrentOutputs, typename... LastOutputs>
struct RecursiveClosure<traits::TypePack<Element, Next, Tail...>,
		traits::TypePack<CurrentInputs...>, traits::TypePack<CurrentOutputs...>,
		traits::TypePack<LastOutputs...>> {
	typedef typename Element::Context Context;
	typedef util::Callback<void(LastOutputs...)> Callback;

	RecursiveClosure(const RecursiveElements<Element, Next, Tail...> &elements,
			Context &context, Callback callback)
	: elementClosure(elements.element, context, CALLBACK_MEMBER(this, &RecursiveClosure::onElement)),
			recursiveTail(elements.tail, context, callback) { }

	void invoke(CurrentInputs... inputs) {
		elementClosure(inputs...);
	}

	void onElement(CurrentOutputs... outputs) {
		recursiveTail.invoke(outputs...);
	}

	typename Element::Closure elementClosure;
	RecursiveClosure<traits::TypePack<Next, Tail...>,
			typename Next::InputPack, typename Next::OutputPack,
			traits::TypePack<LastOutputs...>> recursiveTail;
};

// the last element gets the real callback
template<typename Element, typename... CurrentInputs, typename... CurrentOutputs, typename... LastOutputs>
struct RecursiveClosure<traits::TypePack<Element>,
		traits::TypePack<CurrentInputs...>, traits::TypePack<CurrentOutputs...>,
		traits::TypePack<LastOutputs...>> {
	typedef typename Element::Context Context;
	typedef util::Callback<void(LastOutputs...)> Callback;

	RecursiveClosure(const RecursiveElements<Element> &elements,
			Context &context, Callback callback)
	: elementClosure(elements.element, context, callback) { }

	void invoke(CurrentInputs... inputs) {
		elementClosure(inputs...);
	}

	typename Element::Closure elementClosure;
};

template<typename ElementPack, typename Context,
		typename FirstInputPack, typename FirstOutputPack, typename LastOutputPack>
struct Seq;

template<typename... Elements, typename TheContext,
		typename... FirstInputs, typename... FirstOutputs, typename... LastOutputs>
struct Seq<traits::TypePack<Elements...>, TheContext,
		traits::TypePack<FirstInputs...>, traits::TypePack<FirstOutputs...>,
		traits::TypePack<LastOutputs...>> {
	typedef TheContext Context;
	typedef traits::TypePack<FirstInputs...> InputPack;
	typedef traits::TypePack<LastOutputs...> OutputPack;
	typedef typename util::CallbackFromPack<void, OutputPack>::Type Callback;

	struct Closure {
		Closure(Seq element, Context &context, Callback callback)
		: recursiveClosure(element.recursiveElements, context, callback) { }

		void operator() (FirstInputs... first_inputs) {
			recursiveClosure.invoke(first_inputs...);
		}

		seq_impl::RecursiveClosure<traits::TypePack<Elements...>,
				traits::TypePack<FirstInputs...>, traits::TypePack<FirstOutputs...>,
				traits::TypePack<LastOutputs...>> recursiveClosure;
	};

	Seq(const Elements &... elements) : recursiveElements(elements...) { }
	
	seq_impl::RecursiveElements<Elements...> recursiveElements;
};

template<typename... Elements>
struct InspectFirst;

template<typename FirstElement, typename... Elements>
struct InspectFirst<FirstElement, Elements...> {
	typedef typename FirstElement::Context Context;
	typedef typename FirstElement::InputPack InputPack;
	typedef typename FirstElement::OutputPack OutputPack;
};

template<typename... Elements>
struct InspectLast;

template<typename Element, typename... Elements>
struct InspectLast<Element, Elements...> {
	typedef typename InspectLast<Elements...>::OutputPack OutputPack;
};

template<typename LastElement>
struct InspectLast<LastElement> {
	typedef typename LastElement::OutputPack OutputPack;
};

} // namespace seq_impl

template<typename... Elements>
auto seq(const Elements &... elements) {
	typedef seq_impl::Seq<traits::TypePack<Elements...>,
			typename seq_impl::InspectFirst<Elements...>::Context,
			typename seq_impl::InspectFirst<Elements...>::InputPack,
			typename seq_impl::InspectFirst<Elements...>::OutputPack,
			typename seq_impl::InspectLast<Elements...>::OutputPack> Seq;
	return Seq(elements...);
}

} } // namespace frigg::async

