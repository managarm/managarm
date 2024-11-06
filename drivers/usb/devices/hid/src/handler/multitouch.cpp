#include <core/id-allocator.hpp>
#include <format>
#include <libevbackend.hpp>
#include <linux/input.h>

#include "../spec.hpp"
#include "multitouch.hpp"

namespace {

constexpr bool debugTouches = false;

std::map<std::weak_ptr<libevbackend::EventDevice>, id_allocator<size_t>,
	std::owner_less<std::weak_ptr<libevbackend::EventDevice>>> trackingIdAllocators{};

/* maps a HID tracking ID to a evdev userspace-visible tracking ID */
std::map<std::weak_ptr<libevbackend::EventDevice>, std::map<size_t, size_t>,
	std::owner_less<std::weak_ptr<libevbackend::EventDevice>>> userTrackingIds{};

std::map<std::weak_ptr<libevbackend::EventDevice>, id_allocator<size_t>,
	std::owner_less<std::weak_ptr<libevbackend::EventDevice>>> slotIdAllocators{};

/* maps a HID tracking ID to a evdev userspace-visible slot ID */
std::map<std::weak_ptr<libevbackend::EventDevice>, std::map<size_t, size_t>,
	std::owner_less<std::weak_ptr<libevbackend::EventDevice>>> hidIdSlotMaps{};

constexpr uint32_t usageID(uint16_t page, uint16_t id) {
	return (page << 16) | id;
}

} // namespace

namespace handler::multitouch {

void MultitouchHandler::setupElement(std::shared_ptr<libevbackend::EventDevice> _eventDev, Element *e) {
	if(e->parent->type() != CollectionType::Collection)
		return;

	auto c = static_cast<Collection *>(e->parent);

	if(c->usageId() != usageID(pages::digitizers, usage::digitizers::finger))
		return;

	if(e->usagePage == pages::digitizers) {
		if(e->usageId == usage::digitizers::tipSwitch) {
			e->inputType = EV_KEY;
			e->inputCode = BTN_TOUCH;
		} else if(e->usageId == usage::digitizers::contactIdentifier) {
			_eventDev->setAbsoluteDetails(ABS_MT_SLOT, 0, libevbackend::maxMultitouchSlots - 1);
			_eventDev->enableEvent(EV_ABS, ABS_MT_SLOT);

			e->inputType = EV_ABS;
			e->inputCode = ABS_MT_TRACKING_ID;
			e->logicalMin = 0;
			e->logicalMax = UINT16_MAX;
		}
	} else if(e->usagePage == pages::genericDesktop) {
		if(e->usageId == usage::genericDesktop::x) {
			_eventDev->setAbsoluteDetails(ABS_MT_POSITION_X, e->logicalMin, e->logicalMax);
			_eventDev->enableEvent(EV_ABS, ABS_MT_POSITION_X);
		} else if(e->usageId == usage::genericDesktop::y) {
			_eventDev->setAbsoluteDetails(ABS_MT_POSITION_Y, e->logicalMin, e->logicalMax);
			_eventDev->enableEvent(EV_ABS, ABS_MT_POSITION_Y);
		}
	}
};

void MultitouchHandler::handleReport(std::shared_ptr<libevbackend::EventDevice> eventDev,
		std::vector<Element> &elements,
		std::vector<std::pair<bool, int32_t>> &values) {
	struct touchInfo {
		Collection *c;
		size_t slot;
		std::optional<size_t> hidTrackingId = std::nullopt;
		size_t userTrackingId;
		size_t x;
		size_t y;
		bool touching;
		bool valid;
		size_t xElementId;
		size_t yElementId;
	};

	std::vector<std::shared_ptr<touchInfo>> touches;

	auto is_touch = [&touches](Collection *c) -> bool {
		return std::find_if(touches.cbegin(), touches.cend(), [&](auto e) {
			return e->c == c;
		}) != touches.cend();
	};

	auto touch = [&](Collection *c) -> std::shared_ptr<touchInfo> {
		if(!is_touch(c)) {
			touches.push_back(std::make_shared<touchInfo>(c));
		}

		return *std::find_if(touches.cbegin(), touches.cend(), [&](auto e) {
			return e->c == c;
		});
	};

	for(size_t i = 0; i < elements.size(); i++) {
		auto &e = elements[i];
		auto &v = values[i];

		if(e.parent->type() != CollectionType::Collection)
			continue;

		auto c = static_cast<Collection *>(e.parent);

		if(c->usageId() != usageID(pages::digitizers, usage::digitizers::finger))
			continue;

		if(e.usagePage == pages::digitizers) {
			if(e.usageId == usage::digitizers::contactIdentifier) {
				v.first = false;
				touch(c)->hidTrackingId = v.second;

				if(v.second == 0) {
					touch(c)->valid = false;
					continue;
				}

				if(!hidIdSlotMaps.contains(eventDev))
					hidIdSlotMaps.insert({eventDev, {}});
				if(!slotIdAllocators.contains(eventDev))
					slotIdAllocators.insert({eventDev, {0, libevbackend::maxMultitouchSlots - 1}});

				if(!hidIdSlotMaps.at(eventDev).contains(v.second)) {
					auto slot_id = slotIdAllocators.at(eventDev).allocate();
					hidIdSlotMaps.at(eventDev).insert({v.second, slot_id});
					touch(c)->slot = slot_id;
				} else {
					assert(hidIdSlotMaps.at(eventDev).contains(v.second));
					touch(c)->slot = hidIdSlotMaps.at(eventDev).at(v.second);
				}
			} else if(e.usageId == usage::digitizers::touchValid) {
				touch(c)->valid = v.second;
			}
		}
	}

	for(size_t i = 0; i < elements.size(); i++) {
		auto &e = elements[i];
		auto &v = values[i];

		if(e.parent->type() != CollectionType::Collection)
			continue;

		auto c = static_cast<Collection *>(e.parent);

		if(!is_touch(c))
			continue;

		if(e.usagePage == pages::genericDesktop) {
			if(e.usageId == usage::genericDesktop::x) {
				touch(c)->xElementId = i;
				touch(c)->x = v.second;
				v.first = false;
			} else if(e.usageId == usage::genericDesktop::y) {
				touch(c)->yElementId = i;
				touch(c)->y = v.second;
				v.first = false;
			}
		} else if(e.usagePage == pages::digitizers) {
			if(e.usageId == usage::digitizers::tipSwitch) {
				auto t = touch(c);

				t->touching = v.second;
				v.first = false;

				if(!userTrackingIds.contains(eventDev)) {
					userTrackingIds.insert({eventDev, {}});
				}

				if(t->touching && t->hidTrackingId && !userTrackingIds.at(eventDev).contains(*t->hidTrackingId)) {
					if(!trackingIdAllocators.contains(eventDev))
						trackingIdAllocators.insert({eventDev, {1}});

					auto allocated_id = trackingIdAllocators.at(eventDev).allocate();

					userTrackingIds
						.at(eventDev)
						.insert({*t->hidTrackingId, allocated_id});
				}

				if(t->touching) {
					assert(t->hidTrackingId);
					assert(userTrackingIds.at(eventDev).contains(*t->hidTrackingId));

					t->userTrackingId = userTrackingIds
						.at(eventDev)
						.at(*t->hidTrackingId);
				}
			}
		}
	}

	auto current_touches = std::find_if(touches.cbegin(), touches.cend(), [](auto t) {
		return t->valid;
	});

	bool abs_x_y_enabled = false;

	for(auto t : touches) {
		if(!t->valid || !t->hidTrackingId) {
			continue;
		}

		int trackingId = t->touching ? t->userTrackingId : -1;

		if(debugTouches)
			std::cout << std::format("hid: touch slot={} tracking={} x={} y={}\n", t->slot, trackingId, t->x, t->y);

		// first, check whether any of the values even changed; if not, we need to also suppress
		// emitting ABS_MT_SLOT.
		auto &mtState = eventDev->currentMultitouchState();

		if(mtState.contains(t->slot)) {
			auto &slot = mtState.at(t->slot);
			if(static_cast<size_t>(slot.abs[ABS_MT_POSITION_X - libevbackend::ABS_MT_FIRST]) == t->x
			&& static_cast<size_t>(slot.abs[ABS_MT_POSITION_Y - libevbackend::ABS_MT_FIRST]) == t->y
			&& slot.abs[ABS_MT_TRACKING_ID - libevbackend::ABS_MT_FIRST] == trackingId)
				continue;
		}

		eventDev->emitEvent(EV_ABS, ABS_MT_SLOT, t->slot);
		eventDev->emitEvent(EV_ABS, ABS_MT_TRACKING_ID, trackingId);

		if(t->touching) {
			eventDev->emitEvent(EV_ABS, ABS_MT_POSITION_X, t->x);
			eventDev->emitEvent(EV_ABS, ABS_MT_POSITION_Y, t->y);

			if(!abs_x_y_enabled) {
				values[t->xElementId].first = true;
				values[t->yElementId].first = true;
				abs_x_y_enabled = true;
			}
		}
	}

	if(current_touches != touches.cend()) {
		eventDev->emitEvent(EV_KEY, BTN_TOUCH, 1);
	} else {
		eventDev->emitEvent(EV_KEY, BTN_TOUCH, 0);
	}

	/* free IDs and erase touch events where needed */
	for(auto t : touches) {
		if(t->valid && !t->touching) {
			assert(userTrackingIds.contains(eventDev));
			userTrackingIds
				.at(eventDev)
				.erase(*t->hidTrackingId);

			assert(slotIdAllocators.contains(eventDev));
			slotIdAllocators.at(eventDev).free(t->slot);
			assert(hidIdSlotMaps.contains(eventDev));
			hidIdSlotMaps.at(eventDev).erase(*t->hidTrackingId);
		}
	}
}

} // namespace handler::multitouch
