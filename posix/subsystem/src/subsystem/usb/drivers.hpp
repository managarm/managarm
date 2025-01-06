#pragma once

#include "src/drvcore.hpp"

struct CdcMbimDriver final : drvcore::BusDriver {
	CdcMbimDriver(drvcore::BusSubsystem *parent, std::string name)
	: drvcore::BusDriver(parent, name) {}
};

struct CdcNcmDriver final : drvcore::BusDriver {
	CdcNcmDriver(drvcore::BusSubsystem *parent, std::string name)
	: drvcore::BusDriver(parent, name) {}
};

struct CdcEtherDriver final : drvcore::BusDriver {
	CdcEtherDriver(drvcore::BusSubsystem *parent, std::string name)
	: drvcore::BusDriver(parent, name) {}
};
