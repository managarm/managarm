#pragma once

#include <async/basic.hpp>
#include "stream.hpp"

namespace thor {

void fiberCopyToBundle(MemoryView *bundle, ptrdiff_t offset, const void *pointer, size_t size);
void fiberCopyFromBundle(MemoryView *bundle, ptrdiff_t offset, void *pointer, size_t size);

void fiberSleep(uint64_t nanos);

LaneHandle fiberOffer(LaneHandle lane);
LaneHandle fiberAccept(LaneHandle lane);

void fiberSend(LaneHandle lane, const void *buffer, size_t length);
frigg::UniqueMemory<KernelAlloc> fiberRecv(LaneHandle lane);

void fiberPushDescriptor(LaneHandle lane, AnyDescriptor descriptor);
AnyDescriptor fiberPullDescriptor(LaneHandle lane);

//---------------------------------------------------------------------------------------

struct OfferSender {
	LaneHandle lane;
};

template<typename R>
struct OfferOperation {
	void start() {
		auto cb = [this] (Error error, LaneHandle handle) {
			async::execution::set_value(receiver_,
					frg::tuple<Error, LaneHandle>{error, std::move(handle)});
		};
		submitOffer(s_.lane, cb);
	}

	OfferOperation(OfferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	OfferOperation(const OfferOperation &) = delete;

	OfferOperation &operator= (const OfferOperation &) = delete;

private:
	OfferSender s_;
	R receiver_;
};

template<typename R>
inline OfferOperation<R> connect(OfferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<OfferSender, frg::tuple<Error, LaneHandle>>
operator co_await(OfferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------
struct AcceptSender {
	LaneHandle lane;
};

template<typename R>
struct AcceptOperation {
	void start() {
		auto cb = [this] (Error error, LaneHandle handle) {
			async::execution::set_value(receiver_,
					frg::tuple<Error, LaneHandle>{error, std::move(handle)});
		};
		submitAccept(s_.lane, cb);
	}

	AcceptOperation(AcceptSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

	AcceptOperation(const AcceptOperation &) = delete;

	AcceptOperation &operator= (const AcceptOperation &) = delete;

private:
	AcceptSender s_;
	R receiver_;
};

template<typename R>
inline AcceptOperation<R> connect(AcceptSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<AcceptSender, frg::tuple<Error, LaneHandle>>
operator co_await(AcceptSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct SendBufferSender {
	LaneHandle lane;
	frigg::UniqueMemory<KernelAlloc> buffer;
};

template<typename R>
struct SendBufferOperation {
	void start() {
		auto cb = [this] (Error error) {
			async::execution::set_value(receiver_, error);
		};
		submitSendBuffer(s_.lane, std::move(s_.buffer), cb);
	}

	SendBufferOperation(SendBufferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	SendBufferSender s_;
	R receiver_;
};

template<typename R>
inline SendBufferOperation<R> connect(SendBufferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<SendBufferSender, Error>
operator co_await(SendBufferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct RecvBufferSender {
	LaneHandle lane;
};

template<typename R>
struct RecvBufferOperation {
	void start() {
		auto cb = [this] (Error error, frigg::UniqueMemory<KernelAlloc> buffer) {
			async::execution::set_value(receiver_,
					frg::tuple<Error, frigg::UniqueMemory<KernelAlloc>>{error, std::move(buffer)});
		};
		submitRecvInline(s_.lane, cb);
	}

	RecvBufferOperation(RecvBufferSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	RecvBufferSender s_;
	R receiver_;
};

template<typename R>
inline RecvBufferOperation<R> connect(RecvBufferSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<RecvBufferSender, frg::tuple<Error, frigg::UniqueMemory<KernelAlloc>>>
operator co_await(RecvBufferSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct PushDescriptorSender {
	LaneHandle lane;
	AnyDescriptor descriptor;
};

template<typename R>
struct PushDescriptorOperation {
	void start() {
		auto cb = [this] (Error error) {
			async::execution::set_value(receiver_, error);
		};
		submitPushDescriptor(s_.lane, std::move(s_.descriptor), cb);
	}

	PushDescriptorOperation(PushDescriptorSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	PushDescriptorSender s_;
	R receiver_;
};

template<typename R>
inline PushDescriptorOperation<R> connect(PushDescriptorSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<PushDescriptorSender, Error>
operator co_await(PushDescriptorSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

struct PullDescriptorSender {
	LaneHandle lane;
};

template<typename R>
struct PullDescriptorOperation {
	void start() {
		auto cb = [this] (Error error, AnyDescriptor desc) {
			async::execution::set_value(receiver_,
					frg::tuple<Error, AnyDescriptor>{error, std::move(desc)});
		};
		submitPullDescriptor(s_.lane, cb);
	}

	PullDescriptorOperation(PullDescriptorSender s, R receiver)
	: s_{std::move(s)}, receiver_{std::move(receiver)} { }

private:
	PullDescriptorSender s_;
	R receiver_;
};

template<typename R>
inline PullDescriptorOperation<R> connect(PullDescriptorSender s, R receiver) {
	return {std::move(s), std::move(receiver)};
}

inline async::sender_awaiter<PullDescriptorSender, frg::tuple<Error, AnyDescriptor>>
operator co_await(PullDescriptorSender s) {
	return {std::move(s)};
}

//---------------------------------------------------------------------------------------

// Returns true if an IPC error is caused by the remote side not following the protocol.
inline bool isRemoteIpcError(Error e) {
	switch(e) {
		case Error::bufferTooSmall:
		case Error::transmissionMismatch:
			return true;
		default:
			return false;
	}
}

} // namespace thor
