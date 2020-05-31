#ifndef THOR_GENERIC_WORK_QUEUE_HPP
#define THOR_GENERIC_WORK_QUEUE_HPP

#include <atomic>

#include <async/basic.hpp>
#include <frg/container_of.hpp>
#include <frg/list.hpp>
#include <frigg/atomic.hpp>

namespace thor {

struct WorkQueue;

struct Worklet {
	friend struct WorkQueue;

	void setup(void (*run)(Worklet *), WorkQueue *wq) {
		_run = run;
		_workQueue = wq;
	}

	void setup(void (*run)(Worklet *));

private:
	WorkQueue *_workQueue;
	void (*_run)(Worklet *);
	frg::default_list_hook<Worklet> _hook;
};

struct WorkScope {
	WorkScope(WorkQueue *queue);

	WorkScope() = delete;
	
	WorkScope(const WorkScope &) = delete;

	~WorkScope();

	WorkScope &operator= (const WorkScope &) = delete;

private:
	WorkQueue *_scopedQueue;
	WorkQueue *_outerQueue;
};

struct WorkQueue {
	static WorkQueue *localQueue();

	static void post(Worklet *worklet);

	WorkQueue()
	: _anyPosted{false} { }

	bool check();

	void run();

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
				self->r_.set_value();
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

protected:
	virtual void wakeup() = 0;

private:
	frg::intrusive_list<
		Worklet,
		frg::locate_member<
			Worklet,
			frg::default_list_hook<Worklet>,
			&Worklet::_hook
		>
	> _pending;
	
	frigg::TicketLock _mutex;

	std::atomic<bool> _anyPosted;

	frg::intrusive_list<
		Worklet,
		frg::locate_member<
			Worklet,
			frg::default_list_hook<Worklet>,
			&Worklet::_hook
		>
	> _posted;
};

inline void Worklet::setup(void (*run)(Worklet *)) {
	setup(run, WorkQueue::localQueue());
}

} // namespace thor

#endif // THOR_GENERIC_WORK_QUEUE_HPP
