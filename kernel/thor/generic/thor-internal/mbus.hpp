#pragma once

#include <thor-internal/coroutine.hpp>
#include <thor-internal/universe.hpp>
#include <thor-internal/stream.hpp>
#include <frg/vector.hpp>

namespace thor {

struct Properties {
	friend struct KernelBusObject;

	void stringProperty(frg::string_view name, frg::string<KernelAlloc> &&value) {
		auto allocName = frg::string<KernelAlloc>(*kernelAlloc, name);
		properties_.emplace_back(std::move(allocName), std::move(value));
	}

	void hexStringProperty(frg::string_view name, uint32_t value, int padding) {
		auto allocName = frg::string<KernelAlloc>(*kernelAlloc, name);
		stringProperty(std::move(allocName), frg::to_allocated_string(*kernelAlloc, value, 16, padding));
	}

	void decStringProperty(frg::string_view name, uint32_t value, int padding) {
		auto allocName = frg::string<KernelAlloc>(*kernelAlloc, name);
		stringProperty(std::move(allocName), frg::to_allocated_string(*kernelAlloc, value, 10, padding));
	}

private:
	struct Property {
		Property(frg::string<KernelAlloc> &&name,
			frg::string<KernelAlloc> &&value)
		: name{std::move(name)}, value{std::move(value)} { }

		frg::string<KernelAlloc> name;
		frg::string<KernelAlloc> value;
	};

	frg::vector<Property, KernelAlloc> properties_{*kernelAlloc};
};

struct KernelBusObject {
	coroutine<frg::expected<Error, size_t>> createObject(frg::string_view name, Properties &&properties);
	coroutine<Error> updateProperties(Properties &properties);

	virtual LaneHandle initiateClient() {
		auto stream = createStream();

		async::detach_with_allocator(*kernelAlloc, [] (LaneHandle lane,
				KernelBusObject *self) -> coroutine<void> {
			while(true) {
				auto result = co_await self->handleRequest(lane);

				if (!result && result.error() == Error::endOfLane)
					break;

				// TODO(qookie): Improve error handling here.
				if(!result)
					infoLogger() << "thor: failed to handle KernelBusObject mbus request with error " << static_cast<int>(result.error()) << frg::endlog;
			}
		}(std::move(stream.get<0>()), this));

		return stream.get<1>();
	}

	virtual coroutine<frg::expected<Error>> handleRequest(LaneHandle lane)  {
		co_return Error::illegalObject;
	}

private:
	coroutine<void> handleMbusComms_(LaneHandle mgmtLane);
	coroutine<frg::expected<Error>> handleServeRemoteLane_(LaneHandle mgmtLane);

	int64_t mbusId_;
};

} // namespace thor
