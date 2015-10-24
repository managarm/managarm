
#ifndef BRAGI_MBUS_HPP
#define BRAGI_MBUS_HPP

#include <string>
#include <vector>

#include <frigg/callback.hpp>

#include <helx.hpp>

namespace bragi_mbus {

typedef int64_t ObjectId;

struct ObjectHandler {
	virtual void requireIf(ObjectId object_id, frigg::CallbackPtr<void(HelHandle)> callback) = 0;
};

struct Connection {
	Connection(helx::EventHub &event_hub);

	void setObjectHandler(ObjectHandler *handler);

	void connect(frigg::CallbackPtr<void()> callback);
	
	void registerObject(std::string capability,
			frigg::CallbackPtr<void(ObjectId)> callback);

	void enumerate(std::string capability,
			frigg::CallbackPtr<void(std::vector<ObjectId>)> callback);
	
	void queryIf(ObjectId object_id,
			frigg::CallbackPtr<void(HelHandle)> callback);

private:
	struct ConnectClosure {
		ConnectClosure(Connection &connection, frigg::CallbackPtr<void()> on_connect);
	
		void operator() ();

	private:
		void connected(HelError error, HelHandle handle);
		void processRequest();
		void recvdRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

		Connection &connection;
		frigg::CallbackPtr<void()> onConnect;

		uint8_t buffer[128];
	};
	
	struct RegisterClosure {
		RegisterClosure(Connection &connection, std::string capability,
				frigg::CallbackPtr<void(ObjectId)> callback);
	
		void operator() ();

	private:
		void recvdResponse(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

		Connection &connection;
		std::string capability;
		frigg::CallbackPtr<void(ObjectId)> callback;

		uint8_t buffer[128];
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
	
	struct RequireIfClosure {
		RequireIfClosure(Connection &connection, int64_t request_id, ObjectId object_id);
	
		void operator() ();

	private:
		void requiredIf(HelHandle handle);

		Connection &connection;
		int64_t requestId;
		ObjectId objectId;
	};

	helx::EventHub &eventHub;
	helx::Pipe mbusPipe;
	ObjectHandler *objectHandler;
};

} // namespace bragi_mbus

#endif // BRAGI_MBUS_HPP

