
#ifndef LIBUSB_SERVER_HPP
#define LIBUSB_SERVER_HPP

#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include "api.hpp"

namespace protocols {
namespace usb {

cofiber::no_future serve(Device device, helix::UniqueLane lane);

} } // namespace protocols::usb

#endif // LIBUSB_SERVER_HPP

