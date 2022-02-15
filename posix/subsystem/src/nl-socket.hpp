
#include "file.hpp"

namespace nl_socket {

// Configures the given netlink protocol.
// TODO: Let this take a callback that is called on receive?
void configure(int proto_idx, int num_groups);

// Broadcasts a kernel message to the given netlink multicast group.
void broadcast(int proto_idx, int grp_idx, std::string buffer);

smarter::shared_ptr<File, FileHandle> createSocketFile(int proto_idx, bool nonBlock);
std::array<smarter::shared_ptr<File, FileHandle>, 2> createSocketPair(int proto_idx);

} // namespace nl_socket

