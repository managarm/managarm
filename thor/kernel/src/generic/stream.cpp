
#include "kernel.hpp"

namespace thor {

void LaneDescriptor::submit(frigg::SharedPtr<StreamControl> control) {
	_handle.getStream()->submit(_handle.getLane(), frigg::move(control));
}

static void transfer(SendFromBuffer &from, RecvToBuffer &to) {
	to.accessor.copyTo(from.buffer.data(), from.buffer.size());
	to.complete();
	from.complete();
/*	
	send->error = kErrSuccess;

	recv->error = kErrSuccess;
	recv->msgRequest = send->msgRequest;
	recv->msgSequence = send->msgSequence;
	recv->length = send->kernelBuffer.size();

	AsyncOperation::complete(frigg::move(send));
	AsyncOperation::complete(frigg::move(recv));*/
}

Stream::Stream()
: peerCounter{1, 1} { }

void Stream::submit(int local, frigg::SharedPtr<StreamControl> control) {
	assert(local == 0 || local == 1);
	int remote = 1 - local;

	_processQueue[local].addBack(frigg::move(control));

	while(!_processQueue[local].empty()
			&& !_processQueue[remote].empty()) {
		auto local_ctrl = _processQueue[local].removeFront();
		auto remote_ctrl = _processQueue[remote].removeFront();

		if(SendFromBuffer::classOf(*local_ctrl)
				&& RecvToBuffer::classOf(*remote_ctrl)) {
			transfer(static_cast<SendFromBuffer &>(*local_ctrl),
					static_cast<RecvToBuffer &>(*remote_ctrl));
		}else{
			assert(!"Operations do not match");
		}
	}
}

} // namespace thor

