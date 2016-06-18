
namespace thor {

class RingBuffer {
public:
	RingBuffer();

	void submitBuffer(frigg::SharedPtr<AsyncRingItem> item);

	void doTransfer(frigg::SharedPtr<AsyncSendString> send,
			frigg::SharedPtr<AsyncRecvString> recv);

private:
	frigg::IntrusiveSharedLinkedList<
		AsyncRingItem,
		&AsyncRingItem::bufferItem
	> _bufferQueue;
};

} // namespace thor

