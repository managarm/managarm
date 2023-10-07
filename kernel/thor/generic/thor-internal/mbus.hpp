#pragma once

#include <thor-internal/coroutine.hpp>
#include <thor-internal/universe.hpp>
#include <thor-internal/stream.hpp>
#include <frg/vector.hpp>

namespace thor {

struct Properties {
	friend struct KernelBusObject;

	void stringProperty(frg::string_view name, frg::string<KernelAlloc> &&value) {
		properties_.emplace_back(name, std::move(value));
	}

	void hexStringProperty(frg::string_view name, uint32_t value, int padding) {
		stringProperty(name, frg::to_allocated_string(*kernelAlloc, value, 16, padding));
	}

private:
	struct Property {
		Property(frg::string_view name,
			frg::string<KernelAlloc> &&value)
		: name{name}, value{std::move(value)} { }

		frg::string_view name;
		frg::string<KernelAlloc> value;
	};

	frg::vector<Property, KernelAlloc> properties_{*kernelAlloc};
};

struct KernelBusObject {
	coroutine<frg::expected<Error>> createObject(Properties &&properties);

	virtual LaneHandle initiateClient() {
		auto stream = createStream();

		async::detach_with_allocator(*kernelAlloc, [] (LaneHandle lane,
				KernelBusObject *self) -> coroutine<void> {
			while(true) {
				auto result = co_await self->handleRequest(lane);

				if (!result && result.error() == Error::endOfLane)
					break;

				// TODO(qookie): Improve error handling here.
				result.unwrap();
			}
		}(std::move(stream.get<0>()), this));

		return stream.get<1>();
	}

	virtual coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) {
		co_return Error::illegalObject;
	}

private:
	coroutine<void> handleMbusComms_(LaneHandle objectLane);
	coroutine<frg::expected<Error>> handleBind_(LaneHandle objectLane);
};

} // namespace thor
