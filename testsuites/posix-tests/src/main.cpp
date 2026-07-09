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

static void run_case(abstract_test_case *tcp) {
	std::cout << "posix-tests: Running " << tcp->name() << std::endl;
	try {
		tcp->run();
	} catch (const test_skipped &skip) {
		std::cout << "posix-tests: Skipping " << tcp->name() << ": " << skip.reason << std::endl;
	}
}

int main(int argc, char **argv) {
	CLI::App app{"POSIX testsuite for managarm"};

	std::vector<std::string> globs;
	app.add_option("globs", globs, "tests to run");

	CLI11_PARSE(app, argc, argv);

	if (globs.empty()) {
		for(abstract_test_case *tcp : test_case_ptrs()) {
			run_case(tcp);
		}
	} else {
		for(abstract_test_case *tcp : test_case_ptrs()) {
			for (const auto &glob : globs) {
				if (fnmatch(glob.c_str(), tcp->name(), 0) == 0) {
					run_case(tcp);
				}
			}
		}
	}

	return EXIT_SUCCESS;
}
