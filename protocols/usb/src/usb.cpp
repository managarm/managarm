
#include <protocols/usb/api.hpp>

// arg0 = wValue in the USB spec
// arg1 = wIndex in the USB spec

ControlTransfer::ControlTransfer(XferFlags flags, ControlRecipient recipient,
		ControlType type, uint8_t request, uint16_t arg0, uint16_t arg1,
		void *buffer, size_t length)
	: flags(flags), recipient(recipient), type(type), request(request), arg0(arg0),
			arg1(arg1), buffer(buffer), length(length) { }

InterruptTransfer::InterruptTransfer(XferFlags flags, void *buffer, size_t length)
	: flags(flags), buffer(buffer), length(length) { }

