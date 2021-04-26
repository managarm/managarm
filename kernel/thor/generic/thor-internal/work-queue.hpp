#pragma once

#include <atomic>

#include <async/basic.hpp>
#include <frg/container_of.hpp>
#include <frg/list.hpp>
#include <frg/spinlock.hpp>
#include <smarter.hpp>

#include <thor-internal/executor-context.hpp>

namespace thor {

struct WorkQueue;

struct Worklet {
	friend struct WorkQueue;

	void setup(void (*run)(Worklet *), WorkQueue *wq);

private:
	smarter::shared_ptr<WorkQueue> _workQueue;
	void (*_run)(Worklet *);
	frg::default_list_hook<Worklet> _hook;
};

struct WorkQueue {
	static WorkQueue *generalQueue();

	static void post(Worklet *worklet);
	static bool enter(Worklet *worklet);

	WorkQueue(ExecutorContext *executorContext = illegalExecutorContext())
	: _executorContext{executorContext}, _localPosted{false}, _lockedPosted{false} { }

	bool check();

	void run();

	auto take() {
		return selfPtr.lock();
	}

	// ----------------------------------------------------------------------------------
	// schedule() and its boilerplate.
	// ----------------------------------------------------------------------------------

	template<typename Receiver>
	struct ScheduleOperation {
		ScheduleOperation(WorkQueue *wq, Receiver r)
		: wq_{wq}, r_{std::move(r)} { }

		void start() {
			worklet_.setup([] (Worklet *base) {
				auto self = frg::container_of(base, &ScheduleOperation::worklet_);
				async::execution::set_value(self->r_);
			}, wq_);
			post(&worklet_);
		}

	private:
		WorkQueue *wq_;
		Receiver r_;
		Worklet worklet_;
	};

	struct ScheduleSender {
		using value_type = void;

		template<typename Receiver>
		friend ScheduleOperation<Receiver> connect(ScheduleSender s, Receiver r) {
			return {s.wq, std::move(r)};
		}

		friend async::sender_awaiter<ScheduleSender> operator co_await (ScheduleSender s) {
			return {s};
		}

		WorkQueue *wq;
	};

	ScheduleSender schedule() {
		return {this};
	}

	// ----------------------------------------------------------------------------------
	// enter() and its boilerplate.
	// ----------------------------------------------------------------------------------

	template<typename Receiver>
	struct EnterOperation {
		EnterOperation(WorkQueue *wq, Receiver r)
		: wq_{wq}, r_{std::move(r)} { }

		bool start_inline() {
			worklet_.setup([] (Worklet *base) {
				auto self = frg::container_of(base, &EnterOperation::worklet_);
				async::execution::set_value_noinline(self->r_);
			}, wq_);
			if(enter(&worklet_)) {
				async::execution::set_value_inline(r_);
				return true;
			}
			return false;
		}

	private:
		WorkQueue *wq_;
		Receiver r_;
		Worklet worklet_;
	};

	struct EnterSender {
		using value_type = void;

		template<typename Receiver>
		friend EnterOperation<Receiver> connect(EnterSender s, Receiver r) {
			return {s.wq, std::move(r)};
		}

		friend async::sender_awaiter<EnterSender> operator co_await (EnterSender s) {
			return {s};
		}

		WorkQueue *wq;
	};

	EnterSender enter() {
		return {this};
	}

	// ----------------------------------------------------------------------------------

	smarter::weak_ptr<WorkQueue> selfPtr;

protected:
	virtual void wakeup() = 0;

private:
	ExecutorContext *_executorContext;

	frg::intrusive_list<
		Worklet,
		frg::locate_member<
			Worklet,
			frg::default_list_hook<Worklet>,
			&Worklet::_hook
		>
	> _localQueue;

	std::atomic<bool> _localPosted;

	std::atomic<bool> _inRun{false};

	frg::ticket_spinlock _mutex;

	// Writes to this flag are totally ordered since they only happen within _mutex.
	// Each 0-1 transition of this flag causes wakeup() to be called.
	// wakeup() is responsible to ensure that (i) check() (and eventually run()) will be called,
	// and (ii) that the call to check() synchronizes with the 0-1 transition of _lockedPosted.
	// (In the case of threads, this is guaranteed by the blocking mechanics.)
	std::atomic<bool> _lockedPosted;

	frg::intrusive_list<
		Worklet,
		frg::locate_member<
			Worklet,
			frg::default_list_hook<Worklet>,
			&Worklet::_hook
		>
	> _lockedQueue;
};

inline void Worklet::setup(void (*run)(Worklet *), WorkQueue *wq) {
	auto swq = wq->selfPtr.lock();
	assert(swq);
	_run = run;
	_workQueue = std::move(swq);
}

} // namespace thor
