
#ifndef HELIX_HPP
#define HELIX_HPP

#include <atomic>
#include <stdexcept>

#include <hel.h>
#include <hel-syscalls.h>

namespace helix {

struct UniqueDescriptor {
	UniqueDescriptor()
	: _handle(kHelNullHandle) { }

	explicit UniqueDescriptor(HelHandle handle)
	: _handle(handle) { }

	HelHandle getHandle() {
		return _handle;
	}

private:
	HelHandle _handle;
};

struct UniqueHub : public UniqueDescriptor {
	UniqueHub(UniqueDescriptor descriptor)
	: UniqueDescriptor(std::move(descriptor)) { }
};

inline UniqueHub createHub() {
	HelHandle handle;
	HEL_CHECK(helCreateEventHub(&handle));
	return UniqueHub(UniqueDescriptor(handle));
}

struct UniquePipe : public UniqueDescriptor {
	UniquePipe() = default;

	UniquePipe(UniqueDescriptor descriptor)
	: UniqueDescriptor(std::move(descriptor)) { }
};

inline std::pair<UniquePipe, UniquePipe> createFullPipe() {
	HelHandle first_handle, second_handle;
	HEL_CHECK(helCreateFullPipe(&first_handle, &second_handle));
	return { UniquePipe(UniqueDescriptor(first_handle)),
			UniquePipe(UniqueDescriptor(second_handle)) };
}

template<typename M>
struct Dispatcher;

namespace ops {
	struct OperationBase {
		OperationBase(HelHandle hub)
		: _hub(hub) { }

	protected:
		// TODO: the hub should be type-safe.
		HelHandle _hub;
		int64_t _asyncId;
	};

	template<typename M>
	struct CompletableBase {
		typename M::Future future() {
			return _completer.future();
		}

	protected:
		typename M::Completer _completer;
	};

	struct ResultBase {
		// TODO: replace by std::error_code
		HelError error() {
			return _error;
		}

	protected:
		HelError _error;
	};

	struct RecvStringResult : public ResultBase {
		size_t actualLength() {
			HEL_CHECK(_error);
			return _actualLength;
		}

		int64_t requestId() {
			HEL_CHECK(_error);
			return _requestId;
		}
		int64_t sequenceId() {
			HEL_CHECK(_error);
			return _sequenceId;
		}

	protected:
		size_t _actualLength;
		int64_t _requestId;
		int64_t _sequenceId;
	};

	struct SendStringResult : public ResultBase {

	};

	template<typename M>
	struct RecvString : public OperationBase, public RecvStringResult, public CompletableBase<M> {
		friend class Dispatcher<M>;

		RecvString(Dispatcher<M> &dispatcher, UniquePipe &pipe, void *buffer, size_t max_length,
				int64_t msg_request, int64_t msg_seq, uint32_t flags)
		: OperationBase(dispatcher._hub.getHandle()) {
			HEL_CHECK(helSubmitRecvString(pipe.getHandle(), _hub,
					(uint8_t *)buffer, max_length, msg_request, msg_seq,
					0, (uintptr_t)this, flags, &_asyncId));
		}
	};

	template<typename M>
	struct SendString : public OperationBase, public SendStringResult, public CompletableBase<M> {
		friend class Dispatcher<M>;

		SendString(Dispatcher<M> &dispatcher, UniquePipe &pipe, const void *buffer, size_t length,
				int64_t msg_request, int64_t msg_seq, uint32_t flags)
		: OperationBase(dispatcher._hub.getHandle()) {
			HEL_CHECK(helSubmitSendString(pipe.getHandle(), _hub,
					(const uint8_t *)buffer, length, msg_request, msg_seq,
					0, (uintptr_t)this, flags, &_asyncId));
		}
	};
} // namespace ops

template<typename M>
struct Dispatcher {
	friend class ops::RecvString<M>;
	friend class ops::SendString<M>;

	using RecvString = ops::RecvString<M>;
	using SendString = ops::SendString<M>;

	Dispatcher(UniqueHub hub)
	: _hub(std::move(hub)) { }

	void operator() () {
		static constexpr int kEventsPerCall = 16;

		HelEvent list[kEventsPerCall];
		size_t num_items;
		HEL_CHECK(helWaitForEvents(_hub.getHandle(), list, kEventsPerCall,
				kHelWaitInfinite, &num_items));

		for(int i = 0; i < num_items; i++) {
			HelEvent &e = list[i];
			switch(e.type) {
			case kHelEventRecvString: {
				auto ptr = static_cast<RecvString *>((void *)e.submitObject);
				ptr->_error = e.error;
				ptr->_actualLength = e.length;
				ptr->_requestId = e.msgRequest;
				ptr->_sequenceId = e.msgSequence;
				ptr->_completer();
			} break;
			case kHelEventSendString: {
				auto ptr = static_cast<SendString *>((void *)e.submitObject);
				ptr->_error = e.error;
				ptr->_completer();
			} break;
			default:
				throw std::runtime_error("Unknown event type");
			}
		}
	}

private:
	UniqueHub _hub;
};

} // namespace helix

#endif // HELIX_HPP

