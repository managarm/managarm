#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#define DEFINE_TEST(s, f) \
	static test_case test_ ## s{#s, f};

struct abstract_test_case {
private:
	static void register_case(abstract_test_case *tcp);

public:
	abstract_test_case(const char *name)
	: name_{name} {
		register_case(this);
	}

	abstract_test_case(const abstract_test_case &) = delete;

	virtual ~abstract_test_case() = default;

	abstract_test_case &operator= (const abstract_test_case &) = delete;

	const char *name() {
		return name_;
	}

	virtual void run() = 0;

private:
	const char *name_;
};

template<typename F>
struct test_case : abstract_test_case {
	test_case(const char *name, F functor)
	: abstract_test_case{name}, functor_{std::move(functor)} { }

	void run() override {
		functor_();
	}

private:
	F functor_;
};

#define assert_errno(fail_func, expr) ((void)(((expr) ? 1 : 0) || (assert_errno_fail(fail_func, #expr, __FILE__, __PRETTY_FUNCTION__, __LINE__), 0)))

inline void assert_errno_fail(const char *fail_func, const char *expr,
		const char *file, const char *func, int line) {
	int err = errno;
	fprintf(stderr, "In function %s, file %s:%d: Function %s failed with error '%s'; failing assertion: '%s'\n",
			func, file, line, fail_func, strerror(err), expr);
	abort();
	__builtin_unreachable();
}
