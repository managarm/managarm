#pragma once

#include <frg/list.hpp>
#include <frigg/atomic.hpp>
#include "../../arch/x86/ints.hpp"

struct abstract_cancellation_callback {
	friend struct cancellation_event;

	frg::default_list_hook<abstract_cancellation_callback> hook;

private:
	virtual void call() = 0;
};

struct cancellation_event {
	friend struct cancellation_token;

	template<typename F>
	friend struct scoped_cancellation_callback;

	template<typename F>
	friend struct transient_cancellation_callback;

	cancellation_event()
	: _was_requested{false} { };

	cancellation_event(const cancellation_event &) = delete;
	cancellation_event(cancellation_event &&) = delete;

	~cancellation_event() {
		assert(_cbs.empty() && "all callbacks must be destructed before"
				" cancellation_event is destructed");
	}

	cancellation_event &operator= (const cancellation_event &) = delete;
	cancellation_event &operator= (cancellation_event &&) = delete;

	void cancel();

private:
	frigg::TicketLock _mutex;

	bool _was_requested;

	frg::intrusive_list<
		abstract_cancellation_callback,
		frg::locate_member<
			abstract_cancellation_callback,
			frg::default_list_hook<abstract_cancellation_callback>,
			&abstract_cancellation_callback::hook
		>
	> _cbs;
};

struct cancellation_token {
	template<typename F>
	friend struct scoped_cancellation_callback;

	template<typename F>
	friend struct transient_cancellation_callback;

	cancellation_token()
	: _event{nullptr} { }

	cancellation_token(cancellation_event &event_ref)
	: _event{&event_ref} { }

	bool is_cancellation_requested() const {
		if(!_event)
			return false;
		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&_event->_mutex);
		return _event->_was_requested;
	}

private:
	cancellation_event *_event;
};

template<typename F>
struct transient_cancellation_callback : abstract_cancellation_callback {
	transient_cancellation_callback(F functor = F{})
	: _event{nullptr}, _functor{std::move(functor)} {
	}

	transient_cancellation_callback(const transient_cancellation_callback &) = delete;
	transient_cancellation_callback(transient_cancellation_callback &&) = delete;

	// TODO: we could do some sanity checking of the state in the destructor.
	~transient_cancellation_callback() = default;

	transient_cancellation_callback &operator= (const transient_cancellation_callback &) = delete;
	transient_cancellation_callback &operator= (transient_cancellation_callback &&) = delete;

	bool try_set(cancellation_token token) {
		assert(!_event);
		if(!token._event)
			return true;
		_event = token._event;

		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&_event->_mutex);
		if(!_event->_was_requested) {
			_event->_cbs.push_back(this);
			return true;
		}
		return false;
	}

	// TODO: provide a set() method that calls the handler inline if cancellation is requested.

	bool try_reset() {
		if(!_event)
			return true;

		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&_event->_mutex);
		if(!_event->_was_requested) {
			auto it = _event->_cbs.iterator_to(this);
			_event->_cbs.erase(it);
			return true;
		}
		return false;
	}

	// TODO: provide a reset() method that waits until the cancellation handler completed.
	//       This can be done by spinning on an atomic variable in cancellation_event
	//       that determines whether handlers are currently running.

private:
	void call() override {
		_functor();
	}

	cancellation_event *_event;
	F _functor;
};

inline void cancellation_event::cancel() {
	frg::intrusive_list<
		abstract_cancellation_callback,
		frg::locate_member<
			abstract_cancellation_callback,
			frg::default_list_hook<abstract_cancellation_callback>,
			&abstract_cancellation_callback::hook
		>
	> pending;

	{
		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&_mutex);

		_was_requested = true;
		pending.splice(pending.begin(), _cbs);
	}

	while(!pending.empty()) {
		auto cb = pending.front();
		pending.pop_front();
		cb->call();
	}
}
