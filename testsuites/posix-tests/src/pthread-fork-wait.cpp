#include <assert.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

static pid_t main_pid;
static pid_t main_tid;
static pid_t child_pid;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

void *thread_waiter(void *) {
	assert(gettid() != main_tid);
	pthread_mutex_lock(&mutex);
	while(!child_pid) {
		pthread_cond_wait(&cond, &mutex);
	}
	pthread_mutex_unlock(&mutex);

	int status;
	pid_t ret = waitpid(child_pid, &status, 0);
	if (ret < 0) {
		perror("waitpid");
	}
	assert(ret == child_pid);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 69);
	return nullptr;
}

DEFINE_TEST(pthread_fork_wait, []() {
	pthread_t thread;
	main_pid = getpid();
	main_tid = gettid();
	if (pthread_create(&thread, NULL, thread_waiter, NULL)) {
		perror("pthread_create");
		exit(1);
	}

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (pid == 0) {
		assert(getppid() == main_pid);
		sleep(1);
		exit(69);
	}

	pthread_mutex_lock(&mutex);
	child_pid = pid;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);

	if (pthread_join(thread, NULL)) {
		perror("pthread_join");
		exit(1);
	}
})
