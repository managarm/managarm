#include <iostream>
#include <vector>

#include "testsuite.hpp"

std::vector<abstract_test_case *> &test_case_ptrs() {
	static std::vector<abstract_test_case *> singleton;
	return singleton;
}

void abstract_test_case::register_case(abstract_test_case *tcp) { test_case_ptrs().push_back(tcp); }

int main() {
	for (abstract_test_case *tcp : test_case_ptrs()) {
		std::cout << "posix-tests: Running " << tcp->name() << std::endl;
		tcp->run();
	}
}
