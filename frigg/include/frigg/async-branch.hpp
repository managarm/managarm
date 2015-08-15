
namespace frigg {
namespace async {

namespace branch_impl {

template<typename Condition, typename IfCase, typename ElseCase,
		typename InputPack, typename CaseInputPack>
struct Branch;

template<typename Condition, typename IfCase, typename ElseCase,
		typename... Inputs, typename... CaseInputs>
struct Branch<Condition, IfCase, ElseCase,
		traits::TypePack<Inputs...>, traits::TypePack<CaseInputs...>> {
	typedef typename Condition::Context Context;
	typedef typename Condition::InputPack InputPack;
	typedef typename IfCase::OutputPack OutputPack;
	typedef typename util::CallbackFromPack<void, OutputPack>::Type Callback;

	struct Closure {
		Closure(const Branch &element, Context &context, Callback callback)
		: conditionClosure(element.condition, context, CALLBACK_MEMBER(this, &Closure::onBranch)),
				ifClosure(element.ifCase, context, callback),
				elseClosure(element.elseCase, context, callback) { }

		void operator() (Inputs... inputs) {
			conditionClosure(inputs...);
		}

		void onBranch(bool which, CaseInputs... case_inputs) {
			if(which) {
				ifClosure(case_inputs...);
			}else{
				elseClosure(case_inputs...);
			}
		}

		typename Condition::Closure conditionClosure;
		typename IfCase::Closure ifClosure;
		typename ElseCase::Closure elseClosure;
	};

	Branch(const Condition &condition, const IfCase &if_case, const ElseCase &else_case)
	: condition(condition), ifCase(if_case), elseCase(else_case) { }
	
	Condition condition;
	IfCase ifCase;
	ElseCase elseCase;
};

} // namespace branch_impl

template<typename Condition, typename IfCase, typename ElseCase>
auto branch(const Condition &condition, const IfCase &if_case, const ElseCase &else_case) {
	typedef branch_impl::Branch<Condition, IfCase, ElseCase,
			typename Condition::InputPack, typename IfCase::InputPack> Branch;
	return Branch(condition, if_case, else_case);
}

} } // namespace frigg::async

