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
	const auto getResuidResult = getresuid(&state.real, &state.effective, &state.saved);
	assert(getResuidResult == 0);
	return state;
}

GidState get_gid_state() {
	GidState state;
	const auto getResgidResult = getresgid(&state.real, &state.effective, &state.saved);
	assert(getResgidResult == 0);
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
	const auto waitResult = waitpid(child, &status, 0);
	assert(waitResult == child);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == EXIT_SUCCESS);
}

} // namespace

DEFINE_TEST(getresuid, ([] {
	const auto state = get_uid_state();
	const auto real = getuid();
	const auto effective = geteuid();
	assert(state.real == real);
	assert(state.effective == effective);
}))

DEFINE_TEST(getresgid, ([] {
	const auto state = get_gid_state();
	const auto real = getgid();
	const auto effective = getegid();
	assert(state.real == real);
	assert(state.effective == effective);
}))

DEFINE_TEST(setuid_drops_saved_uid, ([] {
	const auto before = get_uid_state();
	if(before.effective != 0)
		skip_test("requires effective UID 0");

	uid_t other;
	if(!choose_other("/proc/self/uid_map", before.real, before.effective,
			before.saved, other))
		skip_test("no alternative mapped UID");

	run_in_child([=] {
		const auto setUidResult = setuid(other);
		assert(setUidResult == 0);
		assert_uid_state({other, other, other});
		errno = 0;
		const auto setReuidResult = setreuid(no_change_id<uid_t>(), 0);
		assert_permission_failure(setReuidResult);
		assert_uid_state({other, other, other});
	});
}))

DEFINE_TEST(setgid_drops_saved_gid, ([] {
	const auto before = get_gid_state();
	const auto uidState = get_uid_state();
	if(uidState.effective != 0)
		skip_test("requires effective UID 0");

	gid_t other;
	if(!choose_other("/proc/self/gid_map", before.real, before.effective,
			before.saved, other))
		skip_test("no alternative mapped GID");
	uid_t dropUid;
	if(!choose_other("/proc/self/uid_map", uidState.real, uidState.effective,
			uidState.saved, dropUid))
		skip_test("no alternative mapped UID");

	run_in_child([=] {
		const auto setGidResult = setgid(other);
		assert(setGidResult == 0);
		assert_gid_state({other, other, other});
		const auto setUidResult = setuid(dropUid);
		assert(setUidResult == 0);
		errno = 0;
		const auto setRegidResult = setregid(no_change_id<gid_t>(), 0);
		assert_permission_failure(setRegidResult);
		assert_gid_state({other, other, other});
	});
}))

DEFINE_TEST(setreuid, ([] {
	const auto before = get_uid_state();
	uid_t other;
	if(!choose_other("/proc/self/uid_map", before.real, before.effective,
			before.saved, other))
		skip_test("no alternative mapped UID");

	const bool privileged = before.real == 0 || before.effective == 0;
	run_in_child([=] {
		const auto noChangeResult = setreuid(no_change_id<uid_t>(), no_change_id<uid_t>());
		assert(noChangeResult == 0);
		assert_uid_state(before);

		if(privileged) {
			const auto setReuidResult = setreuid(other, other);
			assert(setReuidResult == 0);
			const auto after = get_uid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			errno = 0;
			const auto setReuidResult = setreuid(other, no_change_id<uid_t>());
			assert_permission_failure(setReuidResult);
			assert_uid_state(before);
		}
	});
}))

DEFINE_TEST(setregid, ([] {
	const auto before = get_gid_state();
	const auto uidState = get_uid_state();
	gid_t other;
	if(!choose_other("/proc/self/gid_map", before.real, before.effective,
			before.saved, other))
		skip_test("no alternative mapped GID");

	const bool privileged = uidState.real == 0 || uidState.effective == 0;
	run_in_child([=] {
		const auto noChangeResult = setregid(no_change_id<gid_t>(), no_change_id<gid_t>());
		assert(noChangeResult == 0);
		assert_gid_state(before);

		if(privileged) {
			const auto setRegidResult = setregid(other, other);
			assert(setRegidResult == 0);
			const auto after = get_gid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			errno = 0;
			const auto setRegidResult = setregid(other, no_change_id<gid_t>());
			assert_permission_failure(setRegidResult);
			assert_gid_state(before);
		}
	});
}))

DEFINE_TEST(setresuid, ([] {
	const auto before = get_uid_state();
	uid_t other;
	if(!choose_other("/proc/self/uid_map", before.real, before.effective,
			before.saved, other))
		skip_test("no alternative mapped UID");

	const bool privileged = before.real == 0 || before.effective == 0;
	run_in_child([=] {
		const auto noChangeResult = setresuid(no_change_id<uid_t>(), no_change_id<uid_t>(),
				no_change_id<uid_t>());
		assert(noChangeResult == 0);
		assert_uid_state(before);

		if(privileged) {
			if(before.effective == 0) {
				const auto dropEuidResult = setresuid(no_change_id<uid_t>(), other,
						no_change_id<uid_t>());
				assert(dropEuidResult == 0);
				assert_uid_state({before.real, other, before.saved});
				const auto restoreEuidResult = setresuid(no_change_id<uid_t>(), 0,
						no_change_id<uid_t>());
				assert(restoreEuidResult == 0);
				assert_uid_state(before);
			}

			const auto setResuidResult = setresuid(other, other, other);
			assert(setResuidResult == 0);
			const auto after = get_uid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			const auto noOpResult = setresuid(before.real, before.effective, before.saved);
			assert(noOpResult == 0);
			assert_uid_state(before);

			errno = 0;
			const auto setResuidResult = setresuid(other, no_change_id<uid_t>(),
					no_change_id<uid_t>());
			assert_permission_failure(setResuidResult);
			assert_uid_state(before);
		}
	});
}))

DEFINE_TEST(setresgid, ([] {
	const auto before = get_gid_state();
	const auto uidState = get_uid_state();
	gid_t other;
	if(!choose_other("/proc/self/gid_map", before.real, before.effective,
			before.saved, other))
		skip_test("no alternative mapped GID");

	const bool privileged = uidState.real == 0 || uidState.effective == 0;
	run_in_child([=] {
		const auto noChangeResult = setresgid(no_change_id<gid_t>(), no_change_id<gid_t>(),
				no_change_id<gid_t>());
		assert(noChangeResult == 0);
		assert_gid_state(before);

		if(privileged) {
			const auto setResgidResult = setresgid(other, other, other);
			assert(setResgidResult == 0);
			const auto after = get_gid_state();
			assert(after.real == other);
			assert(after.effective == other);
			assert(after.saved == other);
		} else {
			const auto noOpResult = setresgid(before.real, before.effective, before.saved);
			assert(noOpResult == 0);
			assert_gid_state(before);

			errno = 0;
			const auto setResgidResult = setresgid(other, no_change_id<gid_t>(),
					no_change_id<gid_t>());
			assert_permission_failure(setResgidResult);
			assert_gid_state(before);
		}
	});
}))
