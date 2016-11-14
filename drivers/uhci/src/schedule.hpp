
struct QueuedTransaction {
	QueuedTransaction();
	
	cofiber::future<void> future();

	void setupTransfers(TransferDescriptor *transfers, size_t num_transfers);
	QueueHead::LinkPointer head();
	void dumpTransfer();
	bool progress();

	boost::intrusive::list_member_hook<> transactionHook;

private:
	cofiber::promise<void> _promise;
	size_t _numTransfers;
	TransferDescriptor *_transfers;
	size_t _completeCounter;
};


struct ControlTransaction : QueuedTransaction {
	ControlTransaction(SetupPacket setup, void *buffer, int address,
			int endpoint, size_t packet_size, XferFlags flags);

private:
	SetupPacket _setup;
};


struct NormalTransaction : QueuedTransaction {
	NormalTransaction(void *buffer, size_t length, int address,
			int endpoint, size_t packet_size, XferFlags flags);
};


struct ScheduleEntity {
	virtual	QueueHead::LinkPointer head() = 0;
	virtual void linkNext(QueueHead::LinkPointer link) = 0;
	virtual void progress() = 0;

	boost::intrusive::list_member_hook<> scheduleHook;
};


struct DummyEntity : ScheduleEntity {
	DummyEntity();

	QueueHead::LinkPointer head() override;
	void linkNext(QueueHead::LinkPointer link) override;
	void progress() override;

	TransferDescriptor *_transfer;
	
	boost::intrusive::list<
		QueuedTransaction,
		boost::intrusive::member_hook<
			QueuedTransaction,
			boost::intrusive::list_member_hook<>,
			&QueuedTransaction::transactionHook
		>
	> transactionList;
};

struct QueueEntity : ScheduleEntity {
	QueueEntity();

	QueueHead::LinkPointer head() override;
	void linkNext(QueueHead::LinkPointer link) override;
	void progress() override;

	QueueHead *_queue;
	
	boost::intrusive::list<
		QueuedTransaction,
		boost::intrusive::member_hook<
			QueuedTransaction,
			boost::intrusive::list_member_hook<>,
			&QueuedTransaction::transactionHook
		>
	> transactionList;
};

