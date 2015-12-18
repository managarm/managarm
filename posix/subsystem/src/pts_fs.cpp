
#include "common.hpp"
#include "pts_fs.hpp"

namespace pts_fs {

// --------------------------------------------------------
// Endpoint
// --------------------------------------------------------

Endpoint::Endpoint()
: chunkQueue(*allocator), readQueue(*allocator) { }

void Endpoint::writeToQueue(const void *buffer, size_t length) {
	// process read requests until either the buffer was compeletly transfered
	// or all requests have been satisfied
	size_t transfered = 0;
	while(!readQueue.empty()) {
		ReadRequest request = readQueue.removeFront();

		size_t read_length = frigg::min(length - transfered, request.maxLength);
		memcpy(request.buffer, (uint8_t *)buffer + transfered, read_length);
		request.callback(read_length);
		
		transfered += read_length;
		if(transfered == length)
			return;
	}

	// enqueue the remaining chunk
	Endpoint::Chunk chunk;
	chunk.buffer = frigg::UniqueMemory<Allocator>(*allocator, length - transfered);
	memcpy(chunk.buffer.data(), (uint8_t *)buffer + transfered, length - transfered);

	chunkQueue.addBack(frigg::move(chunk));
}

void Endpoint::readFromQueue(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(size_t)> callback) {
	if(chunkQueue.empty()) {
		readQueue.addBack(ReadRequest(buffer, max_length, callback));
	}else{
		Endpoint::Chunk &chunk = chunkQueue.front();

		assert(chunk.consumed < chunk.buffer.size());
		size_t length = frigg::min(chunk.buffer.size() - chunk.consumed, max_length);
		memcpy(buffer, (uint8_t *)chunk.buffer.data() + chunk.consumed, length);
		chunk.consumed += length;

		if(chunk.consumed == chunk.buffer.size())
			chunkQueue.removeFront();

		callback(length);
	}
}

// --------------------------------------------------------
// Endpoint::Chunk
// --------------------------------------------------------

Endpoint::Chunk::Chunk()
: consumed(0) { }

// --------------------------------------------------------
// Endpoint::ReadRequest
// --------------------------------------------------------

Endpoint::ReadRequest::ReadRequest(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(size_t)> callback)
: buffer(buffer), maxLength(max_length), callback(callback) { }

// --------------------------------------------------------
// Master
// --------------------------------------------------------

Master::Master(frigg::SharedPtr<Terminal> terminal)
: terminal(frigg::move(terminal)) { }

void Master::write(const void *buffer, size_t length, frigg::CallbackPtr<void()> callback) {
//	terminal->master.writeToQueue(buffer, length); // implement the ECHO flag
	terminal->slave.writeToQueue(buffer, length);
	callback();
}

void Master::read(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(size_t)> callback) {
	terminal->master.readFromQueue(buffer, max_length, callback);
}

// --------------------------------------------------------
// Slave
// --------------------------------------------------------

Slave::Slave(frigg::SharedPtr<Terminal> terminal)
: terminal(frigg::move(terminal)) { }

void Slave::write(const void *buffer, size_t length, frigg::CallbackPtr<void()> callback) {
	terminal->master.writeToQueue(buffer, length);
	callback();
}

void Slave::read(void *buffer, size_t max_length,
		frigg::CallbackPtr<void(size_t)> callback) {
	terminal->slave.readFromQueue(buffer, max_length, callback);
}

// --------------------------------------------------------
// MountPoint
// --------------------------------------------------------

MountPoint::MountPoint()
: openTerminals(frigg::DefaultHasher<int>(), *allocator), nextTerminalNumber(1) { }

void MountPoint::openMounted(StdUnsafePtr<Process> process,
		frigg::StringView path, uint32_t flags, uint32_t mode,
		frigg::CallbackPtr<void(StdSharedPtr<VfsOpenFile>)> callback) {
	if(path == "ptmx") {
		int number = nextTerminalNumber++;
		auto terminal = frigg::makeShared<Terminal>(*allocator);
		openTerminals.insert(number, frigg::WeakPtr<Terminal>(terminal));

		auto master = frigg::makeShared<Master>(*allocator, frigg::move(terminal));
		callback(frigg::staticPtrCast<VfsOpenFile>(master));
	}else{
		frigg::Optional<int> number = path.toNumber<int>();
		assert(number);

		frigg::WeakPtr<Terminal> *entry = openTerminals.get(*number);
		assert(entry);
		frigg::SharedPtr<Terminal> terminal(*entry);
		assert(terminal);

		auto slave = frigg::makeShared<Slave>(*allocator, frigg::move(terminal));
		callback(frigg::staticPtrCast<VfsOpenFile>(slave));
	}
}

} // namespace pts_fs

