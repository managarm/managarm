#ifndef THOR_GENERIC_WORK_QUEUE_HPP
#define THOR_GENERIC_WORK_QUEUE_HPP

#include <atomic>

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
