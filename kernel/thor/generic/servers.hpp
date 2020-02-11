#ifndef THOR_GENERIC_SERVERS_HPP
#define THOR_GENERIC_SERVERS_HPP

namespace thor {

void initializeSvrctl();
void runMbus();
LaneHandle runServer(frg::string_view name);

} // namespace thor

#endif // THOR_GENERIC_SERVERS_HPP
