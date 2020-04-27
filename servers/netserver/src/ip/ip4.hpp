#pragma once

#include <helix/ipc.hpp>
#include <map>
#include <smarter.hpp>
#include <netserver/nic.hpp>

#include "fs.pb.h"

struct Ip4Socket;

struct Ip4 {
	managarm::fs::Errors serveSocket(helix::UniqueLane lane, int type, int proto);
	// frame is a view into the owner buffer, stripping away eth bits
	void feedPacket(nic::MacAddress dest, nic::MacAddress src,
		arch::dma_buffer owner, arch::dma_buffer_view frame);

private:
	std::multimap<int, smarter::shared_ptr<Ip4Socket>> sockets;
};

Ip4 &ip4();
