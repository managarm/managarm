#pragma once

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

} // namespace
