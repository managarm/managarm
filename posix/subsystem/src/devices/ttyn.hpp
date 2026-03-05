#pragma once

#include "../device.hpp"

std::shared_ptr<UnixDevice> createTTYNDevice(int n);
