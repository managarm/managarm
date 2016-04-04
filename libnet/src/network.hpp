
#ifndef NETWORK_HPP
#define NETWORK_HPP

#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <helx.hpp>
#include <bragi/mbus.hpp>
#include <vector>
#include <unordered_map>
#include <string>

namespace libnet {

struct Network {
	Network(NetDevice &device);
	NetDevice &device;
};

struct OpenFile {
	
};

struct Client {
	Client(helx::EventHub &event_hub, Network &net);

	void init(frigg::CallbackPtr<void()> callback);

private:
	struct ObjectHandler : public bragi_mbus::ObjectHandler {
		ObjectHandler(Client &client);
		
		// inherited from bragi_mbus::ObjectHandler
		void requireIf(bragi_mbus::ObjectId object_id,
				frigg::CallbackPtr<void(HelHandle)> callback) override;

		Client &client;
	};

	struct InitClosure {
		InitClosure(Client &client, frigg::CallbackPtr<void()> callback);

		void operator() ();

	private:
		void connected();
		void registered(bragi_mbus::ObjectId object_id);

		Client &client;
		frigg::CallbackPtr<void()> callback;
	};
	
	helx::EventHub &eventHub;
	Network &net;
	ObjectHandler objectHandler;
	bragi_mbus::Connection mbusConnection;
};

struct Connection {
	Connection(helx::EventHub &event_hub, Network &net, helx::Pipe pipe);

	void operator() ();

	Network &getNet();
	helx::Pipe &getPipe();

	int attachOpenFile(OpenFile *handle);
	OpenFile *getOpenFile(int handle);

private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	helx::EventHub &eventHub;
	Network &net;
	helx::Pipe pipe;

	std::unordered_map<int, OpenFile *> fileHandles;
	int nextHandle;
	uint8_t buffer[128];
};

struct OpenClosure {
	OpenClosure(Connection &connection, int64_t response_id,
			managarm::fs::CntRequest request);

	void operator() ();

	Connection &connection;
	int64_t responseId;
	managarm::fs::CntRequest request;
};

} // namespace libnet

#endif // NETWORK_HPP

