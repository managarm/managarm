
#ifndef HELIX_HPP
#define HELIX_HPP

#include <atomic>
#include <stdexcept>

#include <hel.h>
#include <hel-syscalls.h>

namespace helix {

struct UniqueDescriptor {
	friend void swap(UniqueDescriptor &a, UniqueDescriptor &b) {
		using std::swap;
		swap(a._handle, b._handle);
	}

	UniqueDescriptor()
	: _handle(kHelNullHandle) { }
	
	UniqueDescriptor(const UniqueDescriptor &other) = delete;

	UniqueDescriptor(UniqueDescriptor &&other)
	: UniqueDescriptor() {
		swap(*this, other);
	}

	explicit UniqueDescriptor(HelHandle handle)
	: _handle(handle) { }

	~UniqueDescriptor() {
		if(_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(_handle));
	}

	UniqueDescriptor &operator= (UniqueDescriptor other) {
		swap(*this, other);
		return *this;
	}

	HelHandle getHandle() const {
		return _handle;
	}

	void release() {
		_handle = kHelNullHandle;
	}

private:
	HelHandle _handle;
};

struct BorrowedDescriptor {
	BorrowedDescriptor()
	: _handle(kHelNullHandle) { }
	
	BorrowedDescriptor(const BorrowedDescriptor &other) = default;
	BorrowedDescriptor(BorrowedDescriptor &&other) = default;

	explicit BorrowedDescriptor(HelHandle handle)
	: _handle(handle) { }
	
	BorrowedDescriptor(const UniqueDescriptor &other)
	: BorrowedDescriptor(other.getHandle()) { }

	~BorrowedDescriptor() = default;

	BorrowedDescriptor &operator= (const BorrowedDescriptor &) = default;

	HelHandle getHandle() const {
		return _handle;
	}

private:
	HelHandle _handle;
};

template<typename Tag>
struct UniqueResource : UniqueDescriptor {
	UniqueResource() = default;

	explicit UniqueResource(HelHandle handle)
	: UniqueDescriptor(handle) { }

	UniqueResource(UniqueDescriptor descriptor)
	: UniqueDescriptor(std::move(descriptor)) { }
};

template<typename Tag>
struct BorrowedResource : BorrowedDescriptor {
	BorrowedResource() = default;

	explicit BorrowedResource(HelHandle handle)
	: BorrowedDescriptor(handle) { }

	BorrowedResource(BorrowedDescriptor descriptor)
	: BorrowedDescriptor(descriptor) { }

	BorrowedResource(const UniqueResource<Tag> &other)
	: BorrowedDescriptor(other) { }
};

struct Hub { };
using UniqueHub = UniqueResource<Hub>;
using BorrowedHub = BorrowedResource<Hub>;

inline UniqueHub createHub() {
	HelHandle handle;
	HEL_CHECK(helCreateEventHub(&handle));
	return UniqueHub(handle);
}

struct Pipe { };
using UniquePipe = UniqueResource<Pipe>;
using BorrowedPipe = BorrowedResource<Pipe>;

inline std::pair<UniquePipe, UniquePipe> createFullPipe() {
	HelHandle first_handle, second_handle;
	HEL_CHECK(helCreateFullPipe(&first_handle, &second_handle));
	return { UniquePipe(first_handle), UniquePipe(second_handle) };
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
	
	struct RecvDescriptorResult : public ResultBase {
		UniqueDescriptor descriptor() {
			UniqueDescriptor descriptor(_handle);
			_handle = kHelNullHandle;
			return descriptor;
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
		HelHandle _handle;
		int64_t _requestId;
		int64_t _sequenceId;
	};

	struct SendStringResult : public ResultBase {

	};

	template<typename M>
	struct RecvString : public OperationBase, public RecvStringResult, public CompletableBase<M> {
		friend class Dispatcher<M>;

		RecvString(Dispatcher<M> &dispatcher, BorrowedPipe pipe, void *buffer, size_t max_length,
				int64_t msg_request, int64_t msg_seq, uint32_t flags)
		: OperationBase(dispatcher.getHub().getHandle()) {
			auto error = helSubmitRecvString(pipe.getHandle(), _hub,
					(uint8_t *)buffer, max_length, msg_request, msg_seq,
					0, (uintptr_t)this, flags, &_asyncId);
			if(error) {
				_error = error;
				CompletableBase<M>::_completer();
			}
		}
	};

	template<typename M>
	struct RecvDescriptor : public OperationBase, public RecvDescriptorResult, public CompletableBase<M> {
		friend class Dispatcher<M>;

		RecvDescriptor(Dispatcher<M> &dispatcher, BorrowedPipe pipe,
				int64_t msg_request, int64_t msg_seq, uint32_t flags)
		: OperationBase(dispatcher.getHub().getHandle()) {
			auto error = helSubmitRecvDescriptor(pipe.getHandle(), _hub,
					msg_request, msg_seq, 0, (uintptr_t)this, flags, &_asyncId);
			if(error) {
				_error = error;
				CompletableBase<M>::_completer();
			}
		}
	};

	template<typename M>
	struct SendString : public OperationBase, public SendStringResult, public CompletableBase<M> {
		friend class Dispatcher<M>;

		SendString(Dispatcher<M> &dispatcher, BorrowedPipe pipe, const void *buffer, size_t length,
				int64_t msg_request, int64_t msg_seq, uint32_t flags)
		: OperationBase(dispatcher.getHub().getHandle()) {
			auto error = helSubmitSendString(pipe.getHandle(), _hub,
					(const uint8_t *)buffer, length, msg_request, msg_seq,
					0, (uintptr_t)this, flags, &_asyncId);
			if(error) {
				_error = error;
				CompletableBase<M>::_completer();
			}
		}
	};
} // namespace ops

template<typename M>
struct Dispatcher {
	using RecvString = ops::RecvString<M>;
	using RecvDescriptor = ops::RecvDescriptor<M>;
	using SendString = ops::SendString<M>;

	Dispatcher(UniqueHub hub)
	: _hub(std::move(hub)) { }

	BorrowedHub getHub() const {
		return _hub;
	}

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
			case kHelEventRecvDescriptor: {
				auto ptr = static_cast<RecvDescriptor *>((void *)e.submitObject);
				ptr->_error = e.error;
				ptr->_handle = e.handle;
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

