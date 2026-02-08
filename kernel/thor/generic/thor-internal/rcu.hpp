#pragma once

#include <optional>
#include <thor-internal/cpu-data.hpp>

namespace thor {

struct RcuCallable {
	friend struct RcuDispatcher;

private:
	void (*call_)(RcuCallable *);
	frg::default_list_hook<RcuCallable> hook_;
};

void setRcuOnline(CpuData *cpu);

void submitRcu(RcuCallable *callable, void (*call)(RcuCallable *));

// Policy class for frigg::rcu_radixtree.
struct RcuPolicy {
	template<typename T, typename D>
	struct obj_base : private RcuCallable {
		void retire(D d = D()) {
			d_ = std::move(d);
			submitRcu(this, [] (RcuCallable *base) {
				auto self = static_cast<obj_base *>(base);
				(*self->d_)(static_cast<T *>(self));
			});
		}

	private:
		std::optional<D> d_;
	};
};

} // namespace
