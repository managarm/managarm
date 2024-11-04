#pragma once

#include <frg/array.hpp>
#include <frg/tuple.hpp>
#include <frg/vector.hpp>
#include <type_traits>

#include <hel-syscalls.h>
#include <hel.h>

#include <frg/macros.hpp>

#include <bragi/helpers-all.hpp>

namespace helix {

struct UniqueDescriptor {
	friend void swap(UniqueDescriptor &a, UniqueDescriptor &b) {
		using std::swap;
		swap(a._handle, b._handle);
	}

	UniqueDescriptor() : _handle(kHelNullHandle) {}

	UniqueDescriptor(const UniqueDescriptor &other) = delete;

	UniqueDescriptor(UniqueDescriptor &&other) : UniqueDescriptor() { swap(*this, other); }

	explicit UniqueDescriptor(HelHandle handle) : _handle(handle) {}

	~UniqueDescriptor() {
		if (_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(kHelThisUniverse, _handle));
	}

	explicit operator bool() const { return _handle != kHelNullHandle; }

	UniqueDescriptor &operator=(UniqueDescriptor other) {
		swap(*this, other);
		return *this;
	}

	HelHandle getHandle() const { return _handle; }

	void release() { _handle = kHelNullHandle; }

	UniqueDescriptor dup() const {
		if (!_handle)
			return {};
		HelHandle newHandle;
		HEL_CHECK(helTransferDescriptor(getHandle(), kHelThisUniverse, &newHandle));
		return UniqueDescriptor(newHandle);
	}

  private:
	HelHandle _handle;
};

struct BorrowedDescriptor {
	BorrowedDescriptor() : _handle(kHelNullHandle) {}

	BorrowedDescriptor(const BorrowedDescriptor &other) = default;
	BorrowedDescriptor(BorrowedDescriptor &&other) = default;

	explicit BorrowedDescriptor(HelHandle handle) : _handle(handle) {}

	BorrowedDescriptor(const UniqueDescriptor &other) : BorrowedDescriptor(other.getHandle()) {}

	~BorrowedDescriptor() = default;

	BorrowedDescriptor &operator=(const BorrowedDescriptor &) = default;

	HelHandle getHandle() const { return _handle; }

	UniqueDescriptor dup() const {
		HelHandle new_handle;
		HEL_CHECK(helTransferDescriptor(getHandle(), kHelThisUniverse, &new_handle));
		return UniqueDescriptor(new_handle);
	}

  private:
	HelHandle _handle;
};

} // namespace helix

namespace helix_ng {

using namespace helix;

struct DismissResult {
	DismissResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
};

struct OfferResult {
	OfferResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	UniqueDescriptor descriptor() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelHandleResult *>(ptr);
		_error = result->error;
		_descriptor = UniqueDescriptor{result->handle};
		ptr = (char *)ptr + sizeof(HelHandleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
	UniqueDescriptor _descriptor;
};

struct AcceptResult {
	AcceptResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	UniqueDescriptor descriptor() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelHandleResult *>(ptr);
		_error = result->error;
		_descriptor = UniqueDescriptor{result->handle};
		ptr = (char *)ptr + sizeof(HelHandleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
	UniqueDescriptor _descriptor;
};

struct ImbueCredentialsResult {
	ImbueCredentialsResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
};

struct ExtractCredentialsResult {
	ExtractCredentialsResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	char *credentials() {
		FRG_ASSERT(_valid);
		return _credentials;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelCredentialsResult *>(ptr);
		_error = result->error;
		memcpy(_credentials, result->credentials, 16);
		ptr = (char *)ptr + sizeof(HelCredentialsResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
	char _credentials[16];
};

struct SendBufferResult {
	SendBufferResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
};

struct SendBufferSgResult {
	SendBufferSgResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
};

struct RecvBufferResult {
	RecvBufferResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	size_t actualLength() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return _length;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelLengthResult *>(ptr);
		_error = result->error;
		_length = result->length;
		ptr = (char *)ptr + sizeof(HelLengthResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
	size_t _length;
};

struct RecvInlineResult {
	RecvInlineResult() : _valid{false} {}

	HelError error() const {
		FRG_ASSERT(_valid);
		return _error;
	}

	void *data() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return _data;
	}

	const void *data() const {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return _data;
	}

	size_t length() const {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return _length;
	}

	size_t size() const { return length(); }

	void parse(void *&ptr, ElementHandle element) {
		auto result = reinterpret_cast<HelInlineResult *>(ptr);
		_error = result->error;
		_length = result->length;
		_data = result->data;

		_element = element;

		ptr = (char *)ptr + sizeof(HelInlineResult) + ((_length + 7) & ~size_t(7));
		_valid = true;
	}

	void reset() { _element = {}; }

  private:
	bool _valid;
	HelError _error;
	ElementHandle _element;
	void *_data;
	size_t _length;
};

struct PushDescriptorResult {
	PushDescriptorResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
};

struct PullDescriptorResult {
	PullDescriptorResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	UniqueDescriptor descriptor() {
		FRG_ASSERT(_valid);
		HEL_CHECK(error());
		return std::move(_descriptor);
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelHandleResult *>(ptr);
		_error = result->error;
		_descriptor = UniqueDescriptor{result->handle};
		ptr = (char *)ptr + sizeof(HelHandleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
	UniqueDescriptor _descriptor;
};

// --------------------------------------------------------------------
// Items
// --------------------------------------------------------------------

struct Dismiss {};

template <typename... T> struct Offer {
	frg::tuple<T...> nested_actions;
	bool wants_lane;
};

template <typename... T> struct Accept {
	frg::tuple<T...> nested_actions;
};

struct ImbueCredentials {
	HelHandle handle;
};

struct ExtractCredentials {};

struct SendBuffer {
	const void *buf;
	size_t size;
};

struct SendBufferSg {
	const void *buf;
	size_t size;
};

struct RecvBuffer {
	void *buf;
	size_t size;
};

struct RecvInline {};

struct PushDescriptor {
	HelHandle handle;
};

struct PullDescriptor {};

template <typename Allocator> struct SendBragiHeadTail {
	SendBragiHeadTail(Allocator allocator) : head{allocator}, tail{allocator} {}

	SendBragiHeadTail(const SendBragiHeadTail &) = delete;
	SendBragiHeadTail &operator=(const SendBragiHeadTail &) = delete;

	SendBragiHeadTail(SendBragiHeadTail &&other) = default;
	SendBragiHeadTail &operator=(SendBragiHeadTail &&other) = default;

	frg::vector<uint8_t, Allocator> head;
	frg::vector<uint8_t, Allocator> tail;
};

template <typename Allocator> struct SendBragiHeadOnly {
	SendBragiHeadOnly(Allocator allocator) : head{allocator} {}

	SendBragiHeadOnly(const SendBragiHeadOnly &) = delete;
	SendBragiHeadOnly &operator=(const SendBragiHeadOnly &) = delete;

	SendBragiHeadOnly(SendBragiHeadOnly &&other) = default;
	SendBragiHeadOnly &operator=(SendBragiHeadOnly &&other) = default;

	frg::vector<uint8_t, Allocator> head;
};

// --------------------------------------------------------------------
// Construction functions
// --------------------------------------------------------------------

inline auto dismiss() { return Dismiss{}; }

template <typename... T> inline auto offer(T &&...args) {
	return Offer<T...>{frg::make_tuple(std::forward<T>(args)...), false};
}

struct want_lane_t {};
constexpr inline want_lane_t want_lane;

template <typename... T> inline auto offer(want_lane_t, T &&...args) {
	return Offer<T...>{frg::make_tuple(std::forward<T>(args)...), true};
}

template <typename... T> inline auto accept(T &&...args) {
	return Accept<T...>{frg::make_tuple(std::forward<T>(args)...)};
}

inline auto imbueCredentials() { return ImbueCredentials{kHelThisThread}; }

inline auto imbueCredentials(BorrowedDescriptor desc) { return ImbueCredentials{desc.getHandle()}; }

inline auto imbueCredentials(HelHandle handle) { return ImbueCredentials{handle}; }

inline auto extractCredentials() { return ExtractCredentials{}; }

inline auto sendBuffer(const void *data, size_t length) { return SendBuffer{data, length}; }

inline auto sendBufferSg(const HelSgItem *data, size_t length) {
	return SendBufferSg{data, length};
}

inline auto recvBuffer(void *data, size_t length) { return RecvBuffer{data, length}; }

inline auto recvInline() { return RecvInline{}; }

inline auto pushDescriptor(BorrowedDescriptor desc) { return PushDescriptor{desc.getHandle()}; }

inline auto pullDescriptor() { return PullDescriptor{}; }

template <typename Message, typename Allocator>
inline auto sendBragiHeadTail(Message &msg, Allocator allocator = Allocator()) {
	SendBragiHeadTail<Allocator> item{allocator};
	item.head.resize(Message::head_size);
	item.tail.resize(msg.size_of_tail());

	bragi::write_head_tail(msg, item.head, item.tail);

	return item;
}

template <typename Message, typename Allocator>
inline auto sendBragiHeadOnly(Message &msg, Allocator allocator = Allocator()) {
	SendBragiHeadOnly<Allocator> item{allocator};
	item.head.resize(Message::head_size);
	FRG_ASSERT(!msg.size_of_tail());

	bragi::write_head_only(msg, item.head);

	return item;
}

// --------------------------------------------------------------------
// Item -> HelAction transformation
// --------------------------------------------------------------------

struct {
	template <typename T, typename... Ts> auto operator()(const T &arg, const Ts &...args) {
		return frg::array_concat<HelAction>(createActionsArrayFor(true, arg), (*this)(args...));
	}

	template <typename T> auto operator()(const T &arg) {
		return createActionsArrayFor(false, arg);
	}

	auto operator()() { return frg::array<HelAction, 0>{}; }
} chainActionArrays;

inline auto createActionsArrayFor(bool chain, const Dismiss &) {
	HelAction action{};
	action.type = kHelActionDismiss;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

template <typename... T> inline auto createActionsArrayFor(bool chain, const Offer<T...> &o) {
	HelAction action{};
	action.type = kHelActionOffer;
	action.flags = (chain ? kHelItemChain : 0) |
	               (std::tuple_size_v<decltype(o.nested_actions)> > 0 ? kHelItemAncillary : 0) |
	               (o.wants_lane ? kHelItemWantLane : 0);

	return frg::array_concat<HelAction>(
	    frg::array<HelAction, 1>{action}, frg::apply(chainActionArrays, o.nested_actions)
	);
}

template <typename... T> inline auto createActionsArrayFor(bool chain, const Accept<T...> &o) {
	HelAction action{};
	action.type = kHelActionAccept;
	action.flags = (chain ? kHelItemChain : 0) |
	               (std::tuple_size_v<decltype(o.nested_actions)> > 0 ? kHelItemAncillary : 0);

	return frg::array_concat<HelAction>(
	    frg::array<HelAction, 1>{action}, frg::apply(chainActionArrays, o.nested_actions)
	);
}

inline auto createActionsArrayFor(bool chain, const ImbueCredentials &item) {
	HelAction action{};
	action.type = kHelActionImbueCredentials;
	action.flags = chain ? kHelItemChain : 0;
	action.handle = item.handle;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const ExtractCredentials &) {
	HelAction action{};
	action.type = kHelActionExtractCredentials;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const SendBuffer &item) {
	HelAction action{};
	action.type = kHelActionSendFromBuffer;
	action.flags = chain ? kHelItemChain : 0;
	action.buffer = const_cast<void *>(item.buf);
	action.length = item.size;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const SendBufferSg &item) {
	HelAction action{};
	action.type = kHelActionSendFromBufferSg;
	action.flags = chain ? kHelItemChain : 0;
	action.buffer = const_cast<void *>(item.buf);
	action.length = item.size;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const RecvBuffer &item) {
	HelAction action{};
	action.type = kHelActionRecvToBuffer;
	action.flags = chain ? kHelItemChain : 0;
	action.buffer = item.buf;
	action.length = item.size;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const RecvInline &) {
	HelAction action{};
	action.type = kHelActionRecvInline;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const PushDescriptor &item) {
	HelAction action{};
	action.type = kHelActionPushDescriptor;
	action.flags = chain ? kHelItemChain : 0;
	action.handle = item.handle;

	return frg::array<HelAction, 1>{action};
}

inline auto createActionsArrayFor(bool chain, const PullDescriptor &) {
	HelAction action{};
	action.type = kHelActionPullDescriptor;
	action.flags = chain ? kHelItemChain : 0;

	return frg::array<HelAction, 1>{action};
}

template <typename Allocator>
inline auto createActionsArrayFor(bool chain, const SendBragiHeadTail<Allocator> &item) {
	HelAction headAction{}, tailAction{};
	headAction.type = kHelActionSendFromBuffer;
	headAction.flags = kHelItemChain;
	headAction.buffer = const_cast<uint8_t *>(item.head.data());
	headAction.length = item.head.size();

	tailAction.type = kHelActionSendFromBuffer;
	tailAction.flags = chain ? kHelItemChain : 0;
	tailAction.buffer = const_cast<uint8_t *>(item.tail.data());
	tailAction.length = item.tail.size();

	return frg::array<HelAction, 2>{headAction, tailAction};
}

template <typename Allocator>
inline auto createActionsArrayFor(bool chain, const SendBragiHeadOnly<Allocator> &item) {
	HelAction action{};

	action.type = kHelActionSendFromBuffer;
	action.flags = chain ? kHelItemChain : 0;
	action.buffer = const_cast<uint8_t *>(item.head.data());
	action.length = item.head.size();

	return frg::array<HelAction, 1>{action};
}

// --------------------------------------------------------------------
// Item -> Result type transformation
// --------------------------------------------------------------------

// Offer/Accept helper
template <typename Type, typename... T>
using HelperResultTypeTuple =
    decltype(frg::tuple_cat(frg::tuple<Type>{}, resultTypeTuple(std::declval<T>())...));

inline auto resultTypeTuple(const Dismiss &) { return HelperResultTypeTuple<DismissResult>{}; }

template <typename... T> inline auto resultTypeTuple(const Offer<T...> &) {
	return HelperResultTypeTuple<OfferResult, T...>{};
}

template <typename... T> inline auto resultTypeTuple(const Accept<T...> &) {
	return HelperResultTypeTuple<AcceptResult, T...>{};
}

inline auto resultTypeTuple(const ImbueCredentials &) {
	return frg::tuple<ImbueCredentialsResult>{};
}

inline auto resultTypeTuple(const ExtractCredentials &) {
	return frg::tuple<ExtractCredentialsResult>{};
}

inline auto resultTypeTuple(const SendBuffer &) { return frg::tuple<SendBufferResult>{}; }

inline auto resultTypeTuple(const SendBufferSg &) { return frg::tuple<SendBufferSgResult>{}; }

inline auto resultTypeTuple(const RecvBuffer &) { return frg::tuple<RecvBufferResult>{}; }

inline auto resultTypeTuple(const RecvInline &) { return frg::tuple<RecvInlineResult>{}; }

inline auto resultTypeTuple(const PushDescriptor &) { return frg::tuple<PushDescriptorResult>{}; }

inline auto resultTypeTuple(const PullDescriptor &) { return frg::tuple<PullDescriptorResult>{}; }

template <typename Allocator> inline auto resultTypeTuple(const SendBragiHeadTail<Allocator> &) {
	return frg::tuple<SendBufferResult, SendBufferResult>{};
}

template <typename Allocator> inline auto resultTypeTuple(const SendBragiHeadOnly<Allocator> &) {
	return frg::tuple<SendBufferResult>{};
}

template <typename... T> inline auto createResultsTuple(T &&...args) {
	return frg::tuple_cat(resultTypeTuple(std::forward<T>(args))...);
}

// --------------------------------------------------------------------
// Results for other operations
// --------------------------------------------------------------------

struct AsyncNopResult {
	AsyncNopResult() : _valid{false} {}

	HelError error() {
		FRG_ASSERT(_valid);
		return _error;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelSimpleResult *>(ptr);
		_error = result->error;
		ptr = (char *)ptr + sizeof(HelSimpleResult);
		_valid = true;
	}

  private:
	bool _valid;
	HelError _error;
};

struct AwaitEventResult {
	AwaitEventResult() : valid_{false} {}

	HelError error() {
		FRG_ASSERT(valid_);
		return error_;
	}

	uint32_t bitset() {
		FRG_ASSERT(valid_);
		return bitset_;
	}

	uint32_t sequence() {
		FRG_ASSERT(valid_);
		return sequence_;
	}

	void parse(void *&ptr, ElementHandle) {
		auto result = reinterpret_cast<HelEventResult *>(ptr);
		error_ = result->error;
		bitset_ = result->bitset;
		sequence_ = result->sequence;
		ptr = (char *)ptr + sizeof(HelEventResult);
		valid_ = true;
	}

  private:
	bool valid_;
	HelError error_;
	uint32_t bitset_;
	uint32_t sequence_;
};

} // namespace helix_ng
