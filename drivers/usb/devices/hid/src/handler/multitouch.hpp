#pragma once

#include "../hid.hpp"

namespace handler::multitouch {

struct MultitouchHandler final : public Handler {
	void handleReport(std::shared_ptr<libevbackend::EventDevice> eventDev,
		std::vector<Element> &elements,
		std::vector<std::pair<bool, int32_t>> &values) override;
	void setupElement(std::shared_ptr<libevbackend::EventDevice> eventDev, Element *) override;
};

} // namespace handler::multitouch
