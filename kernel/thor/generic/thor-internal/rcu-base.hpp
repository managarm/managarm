#pragma once

#include <frg/list.hpp>

namespace thor {

struct RcuCallable {
	friend struct RcuDispatcher;

private:
	void (*call_)(RcuCallable *);
	frg::default_list_hook<RcuCallable> hook_;
};

} // namespace thor
