
namespace frigg {

// --------------------------------------------------------
// wrap()
// --------------------------------------------------------

template<typename Functor>
class WrapFunctor {
public:
	constexpr WrapFunctor(Functor functor)
	: p_functor(functor) { }

	template<typename Callback, typename Context>
	class Bound {
	public:
		template<typename... CbArgs>
		Bound(const WrapFunctor &wrap, Context *context, CbArgs &&... cb_args)
		: p_functor(wrap.p_functor), p_callback(traits::forward<CbArgs>(cb_args)...),
				p_context(context) { }
		
		template<typename... Args>
		void operator() (Args &&... args) {
			p_functor(p_context, p_callback, traits::forward<Args>(args)...);
		}

	private:
		Functor p_functor;
		Callback p_callback;
		Context *p_context;
	};

private:
	Functor p_functor;
};

template<typename Functor>
constexpr auto wrapFunctor(Functor functor) {
	return WrapFunctor<Functor>(functor);
};

// --------------------------------------------------------
// wrapFuncPtr()
// --------------------------------------------------------

template<typename FuncPointer, typename Functor>
class WrapFuncPtr;

template<typename... PArgs, typename Functor>
class WrapFuncPtr<void (*) (void *, PArgs...), Functor> {
public:
	constexpr WrapFuncPtr(Functor functor)
	: p_functor(functor) { }

	template<typename Callback, typename Context>
	class Bound {
	public:
		template<typename... CbArgs>
		Bound(const WrapFuncPtr &wrap, Context *context, CbArgs &&... cb_args)
		: p_functor(wrap.p_functor), p_callback(traits::forward<CbArgs>(cb_args)...),
				p_context(context) { }
		
		template<typename... Args>
		void operator() (Args &&... args) {
			p_functor(p_context, this, &invokeCallback, traits::forward<Args>(args)...);
		}

	private:
		static void invokeCallback(void *object, PArgs... pargs) {
			auto bound = static_cast<Bound *>(object);
			bound->p_callback(pargs...);
		}

		Functor p_functor;
		Callback p_callback;
		Context *p_context;
	};

private:
	Functor p_functor;
};

template<typename FuncPointer, typename Functor>
constexpr auto wrapFuncPtr(Functor functor) {
	return WrapFuncPtr<FuncPointer, Functor>(functor);
};

// --------------------------------------------------------
// subContext()
// --------------------------------------------------------

template<typename MemberPtr, typename Async>
class SubContext;

template<typename Outer, typename Inner, typename Async>
class SubContext<Inner Outer::*, Async> {
public:
	constexpr SubContext(Inner Outer::*member_ptr, const Async &async)
	: p_async(async), p_memberPtr(member_ptr) { }

	template<typename Callback, typename Context>
	class Bound {
	public:
		template<typename... CbArgs>
		Bound(const SubContext &sub_context, Outer *outer, CbArgs &&... cb_args)
		: p_async(sub_context.p_async, &(outer->*(sub_context.p_memberPtr)),
				traits::forward<CbArgs>(cb_args)...) { }
		
		template<typename... Args>
		void operator() (Args &&... args) {
			p_async(traits::forward<Args>(args)...);
		}

	private:
		typename Async::template Bound<Callback, Inner> p_async;
	};

private:
	Async p_async;
	Inner Outer::*p_memberPtr;
};

template<typename MemberPtr, typename Async>
constexpr auto subContext(MemberPtr member_ptr, const Async &async) {
	return SubContext<MemberPtr, Async>(member_ptr, async);
}

// --------------------------------------------------------
// asyncSeq()
// --------------------------------------------------------

namespace details {

template<typename... Sequence>
class Seq;

template<typename First, typename... Follow>
class Seq<First, Follow...> {
public:
	constexpr Seq(const First &first, const Follow &... follow)
	: p_first(first), p_follow(follow...) { }

	template<typename Callback, typename Context>
	class Bound {
	public:
		template<typename... CbArgs>
		Bound(const Seq &seq, Context *context, CbArgs &&... cb_args)
		: p_first(seq.p_first, context, seq.p_follow, context,
				traits::forward<CbArgs>(cb_args)...) { }
		
		template<typename... Args>
		void operator() (Args &&... args) {
			p_first(traits::forward<Args>(args)...);
		}

	private:
		typedef typename Seq<Follow...>::template Bound<Callback, Context> FollowBound;
		typename First::template Bound<FollowBound, Context> p_first;
	};

private:
	First p_first;
	Seq<Follow...> p_follow;
};

template<typename Finally>
class Seq<Finally> {
public:
	constexpr Seq(const Finally &finally)
	: p_finally(finally) { }

	template<typename Callback, typename Context>
	class Bound {
	public:
		template<typename... CbArgs>
		Bound(const Seq &seq, Context *context, CbArgs &&... cb_args)
		: p_finally(seq.p_finally, context, traits::forward<CbArgs>(cb_args)...) { }
		
		template<typename... Args>
		void operator() (Args &&... args) {
			p_finally(traits::forward<Args>(args)...);
		}

	private:
		typename Finally::template Bound<Callback, Context> p_finally;
	};

private:
	Finally p_finally;
};

} // namespace details

template<typename... Sequence>
constexpr auto asyncSeq(const Sequence &... sequence) {
	return details::Seq<Sequence...>(sequence...);
}

// --------------------------------------------------------
// asyncRepeatWhile()
// --------------------------------------------------------

template<typename Condition, typename Body>
class RepeatWhile {
public:
	constexpr RepeatWhile(Condition condition, Body body)
	: p_condition(condition), p_body(body) { }

	template<typename Callback, typename Context>
	class Bound {
	public:
		template<typename... CbArgs>
		Bound(const RepeatWhile &repeat_until, Context *context, CbArgs &&... cb_args) { }
		//: p_body(repeat_until.p_body, context, this, traits::forward<CbArgs>(cb_args)...) { }
		
		// disallow copying as it would break the Check::p_super pointer
		Bound(const Bound &other) = delete;
		Bound &operator= (const Bound &other) = delete;

		template<typename... Args>
		void operator() (Args &&... args) {
			//p_body(traits::forward<Args>(args)...);
		}

/*	private:
		class Check {
		public:
			template<typename... CbArgs>
			Check(Bound *super, CbArgs &&... cb_args)
			: p_super(super), p_callback(traits::forward<CbArgs>(cb_args)...) { }

			template<typename... Args>
			void operator() (bool another_loop, Args &&... args) {
				if(another_loop) {
					(*p_super)(traits::forward<Args>(args)...);
				}else{
					p_callback(traits::forward<Args>(args)...);
				}
			}

		private:
			Bound *p_super;
			Callback p_callback;
		};
		
		typename Body::template Bound<Check, Context> p_body;*/
	};

private:
	Condition p_condition;
	Body p_body;
};

template<typename Condition, typename Body>
constexpr auto asyncRepeatWhile(const Condition &condition, const Body &body) {
	return RepeatWhile<Condition, Body>(condition, body);
}


// --------------------------------------------------------
// asyncRepeatUntil()
// --------------------------------------------------------

template<typename Body>
class RepeatUntil {
public:
	constexpr RepeatUntil(Body body)
	: p_body(body) { }

	template<typename Callback, typename Context>
	class Bound {
	public:
		template<typename... CbArgs>
		Bound(const RepeatUntil &repeat_until, Context *context, CbArgs &&... cb_args)
		: p_body(repeat_until.p_body, context, this, traits::forward<CbArgs>(cb_args)...) { }
		
		// disallow copying as it would break the Check::p_super pointer
		Bound(const Bound &other) = delete;
		Bound &operator= (const Bound &other) = delete;

		template<typename... Args>
		void operator() (Args &&... args) {
			p_body(traits::forward<Args>(args)...);
		}

	private:
		class Check {
		public:
			template<typename... CbArgs>
			Check(Bound *super, CbArgs &&... cb_args)
			: p_super(super), p_callback(traits::forward<CbArgs>(cb_args)...) { }

			template<typename... Args>
			void operator() (bool another_loop, Args &&... args) {
				if(another_loop) {
					(*p_super)(traits::forward<Args>(args)...);
				}else{
					p_callback(traits::forward<Args>(args)...);
				}
			}

		private:
			Bound *p_super;
			Callback p_callback;
		};
		
		typename Body::template Bound<Check, Context> p_body;
	};

private:
	Body p_body;
};

template<typename Body>
constexpr auto asyncRepeatUntil(const Body &body) {
	return RepeatUntil<Body>(body);
}

// --------------------------------------------------------
// runAsync()
// --------------------------------------------------------

namespace details {

template<typename Async, typename Context>
struct Closure {
	struct DeleteMe {
		DeleteMe(Closure *closure_ptr)
		: closurePtr(closure_ptr) { }

		void operator() () {
			// TODO: delete this object
		}

		Closure *closurePtr;
	};
	
	template<typename... CtxArgs>
	Closure(const Async &async, CtxArgs &&... ctx_args)
	: async(async, &context, this), context(traits::forward<CtxArgs>(ctx_args)...) { }

	void run() {
		async();
	}
	
	typename Async::template Bound<DeleteMe, Context> async;
	Context context;
};

} // namespace details

template<typename Context, typename Allocator, typename Async, typename... CtxArgs>
void runAsync(Allocator &allocator, const Async &async, CtxArgs &&... ctx_args) {
	typedef details::Closure<Async, Context> Closure;
	auto closure_ptr = memory::construct<Closure>(allocator,
			async, traits::forward<CtxArgs>(ctx_args)...);
	closure_ptr->run();
}

} // namespace frigg

