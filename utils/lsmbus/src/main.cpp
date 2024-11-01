#include <iostream>
#include <type_traits>
#include <typeinfo>

#include <protocols/mbus/client.hpp>

namespace {
struct PrintVisitor {
	void operator()(mbus_ng::StringItem &item) { std::cout << item.value; }

	void operator()(mbus_ng::ArrayItem &item) {
		std::cout << "[ ";
		for (auto array_item : item.items) {
			std::visit(PrintVisitor{}, array_item);
			std::cout << " ";
		}
		std::cout << "]";
	}

	template <typename T> void operator()(T &&) {
		std::cout << "WARNING: Unimplemented type: " << typeid(std::decay_t<T>).name();
	}
};

async::result<void> enumerateBus() {
	auto filter = mbus_ng::Conjunction({});
	auto enumerator = mbus_ng::Instance::global().enumerate(filter);

	while (true) {
		auto [paginated, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			std::cout << "Entity \"" << event.name << "\" (ID " << event.id << "):\n";
			for (auto &[name, value] : event.properties) {
				std::cout << "\t" << name << ": ";
				std::visit(PrintVisitor{}, value);
				std::cout << std::endl;
			}
			std::cout << "\n";
		}

		if (!paginated)
			break;
	}
}
} // namespace

int main() { async::run(enumerateBus(), helix::currentDispatcher); }
