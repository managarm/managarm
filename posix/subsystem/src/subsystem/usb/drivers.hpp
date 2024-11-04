#pragma once

#include "src/drvcore.hpp"

struct CdcMbimDriver final : drvcore::BusDriver {
	CdcMbimDriver(std::shared_ptr<drvcore::BusSubsystem> parent, std::string name)
	    : drvcore::BusDriver(parent, name) {}
};

struct CdcNcmDriver final : drvcore::BusDriver {
	CdcNcmDriver(std::shared_ptr<drvcore::BusSubsystem> parent, std::string name)
	    : drvcore::BusDriver(parent, name) {}
};

struct CdcEtherDriver final : drvcore::BusDriver {
	CdcEtherDriver(std::shared_ptr<drvcore::BusSubsystem> parent, std::string name)
	    : drvcore::BusDriver(parent, name) {}
};
