
namespace frigg {
namespace async {

namespace run_impl {

template<typename AsyncElement, typename CompleteFunctor, typename OutputPack>
struct Block;

template<typename AsyncElement, typename CompleteFunctor, typename... Outputs>
struct Block<AsyncElement, CompleteFunctor, traits::TypePack<Outputs...>> {
	Block(AsyncElement element, typename AsyncElement::Context &&context_constructor,
			CompleteFunctor complete_functor)
	: context(traits::move(context_constructor)), closure(element, context,
			FUNCPTR_MEMBER(this, &Block::onComplete)),
		completeFunctor(complete_functor) { }
		
	void onComplete(Outputs... outputs) {
		completeFunctor(outputs...);
	}
		
	typename AsyncElement::Context context;
	typename AsyncElement::Closure closure;
	CompleteFunctor completeFunctor;
};

} // namespace run_impl

template<typename Allocator, typename AsyncElement, typename CompleteFunctor, typename... Inputs>
void run(Allocator &allocator, AsyncElement element, typename AsyncElement::Context &&context,
		CompleteFunctor complete_functor, Inputs... inputs) {
	typedef run_impl::Block<AsyncElement, CompleteFunctor,
			typename AsyncElement::OutputPack> Block;
	auto block = memory::construct<Block>(allocator,
			element, traits::move(context), complete_functor);
	block->closure(inputs...);
}

} } // namespace frigg::async

