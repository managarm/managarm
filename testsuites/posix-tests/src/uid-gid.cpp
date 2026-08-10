#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <cstdlib>
#include <cstdint>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.hpp"

namespace {

struct UidState {
	uid_t real;
	uid_t effective;
	uid_t saved;
};

struct GidState {
	gid_t real;
	gid_t effective;
	gid_t saved;
};

UidState get_uid_state() {
	UidState state;
	assert(getresuid(&state.real, &state.effective, &state.saved) == 0);
	return state;
}

GidState get_gid_state() {
	GidState state;
	assert(getresgid(&state.real, &state.effective, &state.saved) == 0);
	return state;
}

void assert_uid_state(const UidState &expected) {
	const auto actual = get_uid_state();
	assert(actual.real == expected.real);
	assert(actual.effective == expected.effective);
	assert(actual.saved == expected.saved);
}

void assert_gid_state(const GidState &expected) {
	const auto actual = get_gid_state();
	assert(actual.real == expected.real);
	assert(actual.effective == expected.effective);
	assert(actual.saved == expected.saved);
}

bool id_is_mapped(const char *path, uint64_t id) {
	FILE *file = fopen(path, "r");
	if(!file)
		return true;

	bool haveMapping = false;
	bool mapped = false;
	unsigned long long first;
	unsigned long long second;
	unsigned long long count;
	while(fscanf(file, "%llu %llu %llu", &first, &second, &count) == 3) {
		haveMapping = true;
		if(id >= first && id - first < count)
			mapped = true;
	}
	fclose(file);

	// Systems without user namespaces need not provide these files. An empty
	// file is treated the same way, since it does not describe a restriction.
	return !haveMapping || mapped;
}

template<typename Id>
bool choose_other(const char *mapPath, Id first, Id second, Id third, Id &result) {
	const Id candidates[] = {
		static_cast<Id>(65534), static_cast<Id>(65533),
		static_cast<Id>(1000), static_cast<Id>(1001),
		static_cast<Id>(1), static_cast<Id>(2)
	};
	for(auto candidate : candidates) {
		if(candidate != first && candidate != second && candidate != third
				&& id_is_mapped(mapPath, static_cast<uint64_t>(candidate))) {
			result = candidate;
			return true;
		}
	}

	// Do not scan the whole ID space: a rootless user namespace may map only
	// one ID, while a normal namespace accepts the candidates above.
	return false;
}

template<typename Id>
constexpr Id no_change_id() {
	return static_cast<Id>(-1);
}

void assert_permission_failure(int result) {
	assert(result == -1);
	assert(errno == EPERM);
}

template<typename F>
void run_in_child(F &&function) {
	pid_t child = fork();
	assert(child >= 0);
	if(!child) {
		function();
		_exit(EXIT_SUCCESS);
	}

	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == EXIT_SUCCESS);
}

} // namespace

DEFINE_TEST(getresuid, ([] {
	const auto state = get_uid_state();
	assert(state.real == getuid());
	assert(state.effective == geteuid());
}))

DEFINE_TEST(getresgid, ([] {
	const auto state = get_gid_state();
	assert(state.real == getgid());
	assert(state.effective == getegid());
}))

DEFINE_TEST(setreuid, ([] {
	run_in_child([] {
		const auto before = get_uid_state();
		assert(setreuid(no_change_id<uid_t>(), no_change_id<uid_t>()) == 0);
		assert_uid_state(before);

		const bool privileged = before.real == 0 || before.effective == 0;
		uid_t other;
		if(!choose_other("/proc/self/uid_map", before.real, before.effective,
				before.saved, other))
			return;
		if(privileged) {
			assert(setreuid(other, other) == 0);
			const auto after = get_uid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			errno = 0;
			assert_permission_failure(setreuid(other, no_change_id<uid_t>()));
			assert_uid_state(before);
		}
	});
}))

DEFINE_TEST(setregid, ([] {
	run_in_child([] {
		const auto before = get_gid_state();
		assert(setregid(no_change_id<gid_t>(), no_change_id<gid_t>()) == 0);
		assert_gid_state(before);

		const bool privileged = before.real == 0 || before.effective == 0;
		gid_t other;
		if(!choose_other("/proc/self/gid_map", before.real, before.effective,
				before.saved, other))
			return;
		if(privileged) {
			assert(setregid(other, other) == 0);
			const auto after = get_gid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			errno = 0;
			assert_permission_failure(setregid(other, no_change_id<gid_t>()));
			assert_gid_state(before);
		}
	});
}))

DEFINE_TEST(setresuid, ([] {
	run_in_child([] {
		const auto before = get_uid_state();
		assert(setresuid(no_change_id<uid_t>(), no_change_id<uid_t>(),
				no_change_id<uid_t>()) == 0);
		assert_uid_state(before);

		const bool privileged = before.real == 0 || before.effective == 0;
		uid_t other;
		if(!choose_other("/proc/self/uid_map", before.real, before.effective,
				before.saved, other))
			return;
		if(privileged) {
			assert(setresuid(other, other, other) == 0);
			const auto after = get_uid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			assert(setresuid(before.real, before.effective, before.saved) == 0);
			assert_uid_state(before);

			errno = 0;
			assert_permission_failure(setresuid(other, no_change_id<uid_t>(),
					no_change_id<uid_t>()));
			assert_uid_state(before);
		}
	});
}))

DEFINE_TEST(setresgid, ([] {
	run_in_child([] {
		const auto before = get_gid_state();
		assert(setresgid(no_change_id<gid_t>(), no_change_id<gid_t>(),
				no_change_id<gid_t>()) == 0);
		assert_gid_state(before);

		const bool privileged = before.real == 0 || before.effective == 0;
		gid_t other;
		if(!choose_other("/proc/self/gid_map", before.real, before.effective,
				before.saved, other))
			return;
		if(privileged) {
			assert(setresgid(other, other, other) == 0);
			const auto after = get_gid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			assert(setresgid(before.real, before.effective, before.saved) == 0);
			assert_gid_state(before);

			errno = 0;
			assert_permission_failure(setresgid(other, no_change_id<gid_t>(),
					no_change_id<gid_t>()));
			assert_gid_state(before);
		}
	});
}))
