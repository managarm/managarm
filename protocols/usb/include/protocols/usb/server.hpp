
#ifndef LIBUSB_SERVER_HPP
#define LIBUSB_SERVER_HPP

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include "api.hpp"

namespace protocols {
namespace usb {

async::detached serve(Device device, helix::UniqueLane lane);

} } // namespace protocols::usb

#endif // LIBUSB_SERVER_HPP

