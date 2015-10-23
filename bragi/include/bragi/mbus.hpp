
#ifndef BRAGI_MBUS_HPP
#define BRAGI_MBUS_HPP

#include <string>
#include <vector>

#include <frigg/callback.hpp>

#include <helx.hpp>

namespace bragi_mbus {

typedef int64_t ObjectId;

struct Connection {
	Connection(helx::EventHub &event_hub);

	void connect(frigg::CallbackPtr<void()> callback);

	void enumerate(std::string capability,
			frigg::CallbackPtr<void(std::vector<ObjectId>)> callback);
	
	void queryIf(ObjectId object_id,
			frigg::CallbackPtr<void(HelHandle)> callback);

private:
	struct ConnectClosure {
		ConnectClosure(Connection &connection, frigg::CallbackPtr<void()> callback);
	
		void operator() ();

	private:
		void connected(HelError error, HelHandle handle);

		Connection &connection;
		frigg::CallbackPtr<void()> callback;
	};
	
	struct EnumerateClosure {
		EnumerateClosure(Connection &connection, std::string capability,
				frigg::CallbackPtr<void(std::vector<ObjectId>)> callback);
	
		void operator() ();

	private:
		void recvdResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

		Connection &connection;
		std::string capability;
		frigg::CallbackPtr<void(std::vector<ObjectId>)> callback;

		uint8_t buffer[128];
	};
	
	struct QueryIfClosure {
		QueryIfClosure(Connection &connection, ObjectId object_id,
				frigg::CallbackPtr<void(HelHandle)> callback);
	
		void operator() ();

	private:
		void recvdDescriptor(HelError error,
				int64_t msg_request, int64_t msg_seq, HelHandle handle);

		Connection &connection;
		ObjectId objectId;
		frigg::CallbackPtr<void(HelHandle)> callback;

		uint8_t buffer[128];
	};

	helx::EventHub &eventHub;
	helx::Pipe mbusPipe;
};

} // namespace bragi_mbus

#endif // BRAGI_MBUS_HPP

