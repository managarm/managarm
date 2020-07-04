#pragma once

namespace thor {

void initializeSvrctl();
void runMbus();
LaneHandle runServer(frg::string_view name);

} // namespace thor
