
#ifndef LIBUSB_CLIENT_HPP
#define LIBUSB_CLIENT_HPP

#include <helix/ipc.hpp>
#include "api.hpp"

namespace protocols {
namespace usb {

Device connect(helix::UniqueLane lane);

} } // namespace protocols::usb

#endif // LIBUSB_CLIENT_HPP

