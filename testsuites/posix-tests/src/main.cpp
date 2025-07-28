#include <fnmatch.h>
#include <iostream>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "testsuite.hpp"

std::vector<abstract_test_case *> &test_case_ptrs() {
	static std::vector<abstract_test_case *> singleton;
	return singleton;
}

void abstract_test_case::register_case(abstract_test_case *tcp) {
	test_case_ptrs().push_back(tcp);
}

int main(int argc, char **argv) {
	CLI::App app{"POSIX testsuite for managarm"};

	std::vector<std::string> globs;
	app.add_option("globs", globs, "tests to run");

	CLI11_PARSE(app, argc, argv);

	if (globs.empty()) {
		for(abstract_test_case *tcp : test_case_ptrs()) {
			std::cout << "posix-tests: Running " << tcp->name() << std::endl;
			tcp->run();
		}
	} else {
		for(abstract_test_case *tcp : test_case_ptrs()) {
			for (const auto &glob : globs) {
				if (fnmatch(glob.c_str(), tcp->name(), 0) == 0) {
					std::cout << "posix-tests: Running " << tcp->name() << std::endl;
					tcp->run();
				}
			}
		}
	}

	return EXIT_SUCCESS;
}
