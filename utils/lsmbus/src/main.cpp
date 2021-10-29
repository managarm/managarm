#include <typeinfo>
#include <type_traits>
#include <iostream>

#include <protocols/mbus/client.hpp>

namespace {
struct PrintVisitor {
	void operator()(mbus::StringItem &item) {
		std::cout << item.value;
	}

	template<typename T>
	void operator()(T&&) {
		std::cout << "WARNING: Unimplemented type: " << typeid(std::decay_t<T>).name();
	}
};

async::detached enumerateBus() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({});

	auto handler = mbus::ObserverHandler {}
	.withAttach([] (mbus::Entity, mbus::Properties props) {
		std::cout << "found mbus entry:\n";
		for (auto &[name, value] : props) {
			std::cout << "\tproperty: \"" << name << "\": ";
			std::visit(PrintVisitor {}, value);
			std::cout << std::endl;
		}
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}
}

int main() {
	enumerateBus();
	async::run_forever(helix::currentDispatcher);
}
