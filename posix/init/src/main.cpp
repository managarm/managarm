
#include <stdlib.h>
#include <stdio.h>

#include <spawn.h>

#include <vector>

int main() {
	printf("Starting posix-init\n");

	std::vector<char *> argv;
	argv.push_back(const_cast<char *>("acpi"));
	argv.push_back(nullptr);

	char *envp[] = { nullptr };

	pid_t pid;
	if(posix_spawn(&pid, "zisa", nullptr, nullptr, argv.data(), envp)) {
		printf("posix_spawn() failed\n");
		abort();
	}
}

