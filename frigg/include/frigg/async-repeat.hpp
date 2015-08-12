
namespace frigg {
namespace async {

template<typename Condition, typename Body,
		typename InputPack, typename OutputPack>
struct RepeatWhile;

template<typename Condition, typename Body,
		typename... Inputs, typename... Outputs>
struct RepeatWhile<Condition, Body, traits::TypePack<Inputs...>, traits::TypePack<Outputs...>> {
	typedef typename Condition::Context Context;
	typedef traits::TypePack<Inputs...> InputPack;
	typedef traits::TypePack<Outputs...> OutputPack;
	typedef typename util::FuncPtrFromPack<void, OutputPack>::Type Callback;

	struct Closure {
		Closure(const RepeatWhile &element, Context &context, Callback callback)
		: conditionClosure(element.condition, context, FUNCPTR_MEMBER(this, &Closure::onCheck)),
			bodyClosure(element.body, context, FUNCPTR_MEMBER(this, &Closure::operator())),
			callback(callback) { }

		void operator() (Inputs... inputs) {
			conditionClosure(inputs...);
		}
		
		void onCheck(bool loop, Outputs... outputs) {
			if(loop) {
				bodyClosure(outputs...);
			}else{
				callback(outputs...);
			}
		}

		typename Condition::Closure conditionClosure;
		typename Body::Closure bodyClosure;
		Callback callback;
	};

	RepeatWhile(const Condition &condition, const Body &body)
	: condition(condition), body(body) { }

	Condition condition;
	Body body;
};

template<typename Condition, typename Body>
auto repeatWhile(const Condition &condition, const Body &body) {
	typedef RepeatWhile<Condition, Body,
			typename Condition::InputPack, typename Body::InputPack> RepeatWhile;
	return RepeatWhile(condition, body);
}

} } // namespace frigg::async

