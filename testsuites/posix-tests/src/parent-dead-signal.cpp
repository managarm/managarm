#include <assert.h>
#include <atomic>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

namespace {

int child_end = -1;

void handle_signal(int sig) {
	if(sig == SIGUSR1) {
		printf("Received signal %d. Success!\n", sig);
		assert(getppid() == 1);
		int data = 42;
		write(child_end, &data, sizeof(data));
		exit(0);
	}
}

}

DEFINE_TEST(parent_death_signal, ([] {
	int fds[2] = {};
	int pipe_res = pipe(fds);
	if(pipe_res) {
		perror("pipe");
		exit(EXIT_FAILURE);
	}
	child_end = fds[1];

	pid_t test_parent = fork();

	if (test_parent == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if(test_parent == 0) {
		pid_t parent_pid = getpid(); // Save the parent PID
		int pid = fork();

		if (pid == -1) {
			perror("fork");
			exit(EXIT_FAILURE);
		}

		if (pid == 0) { // Child process
			// Set the PR_SET_PDEATHSIG option
			if (prctl(PR_SET_PDEATHSIG, SIGUSR1) == -1) {
				perror("prctl");
				exit(EXIT_FAILURE);
			}

			// Check if the parent is still the same process
			if (getppid() != parent_pid) {
				printf("Parent has already terminated before setting PR_SET_PDEATHSIG.\n");
				exit(EXIT_FAILURE);
			}

			// Set up a signal handler for SIGUSR1
			struct sigaction sa;
			sa.sa_handler = handle_signal;
			sa.sa_flags = 0;
			sigemptyset(&sa.sa_mask);
			if (sigaction(SIGUSR1, &sa, NULL) == -1) {
				perror("sigaction");
				exit(EXIT_FAILURE);
			}

			printf("Child process %d running. Waiting for parent termination.\n", getpid());

			// Loop indefinitely to simulate work
			while (1) {
				pause(); // Wait for signals
			}
		} else { // Parent process
			printf("Parent process (PID: %d) sleeping for 5 seconds.\n", parent_pid);
			sleep(5);

			printf("Parent process exiting.\n");
			exit(0);
		}
	} else {
		int status = 0;
		auto ret = waitpid(test_parent, &status, 0);
		assert(ret == test_parent);
		assert(WIFEXITED(status));
		assert(WEXITSTATUS(status) == 0);

		int test_data = 0;
		ret = read(fds[0], &test_data, sizeof(test_data));
		assert(ret == sizeof(test_data));
		assert(test_data == 42);
	}
}))
