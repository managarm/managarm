#pragma once

#include <concepts>

#include <frg/list.hpp>

namespace thor {

struct RcuCallable {
	friend struct RcuDispatcher;

private:
	void (*call_)(RcuCallable *);
	frg::default_list_hook<RcuCallable> hook_;
};

// Marker that promises that all instances of a type are RCU protected.
// If shared_ptrs are used, this includes their refcoutns (which allocate_rcu_shared() guarantees).
// Deriving classes can enforce this by providing appropriate create() factories instead of exposing public constructors.
struct RcuProtected {};

template<typename T>
concept IsRcuProtected = std::derived_from<T, RcuProtected>;

} // namespace thor
