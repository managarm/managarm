#pragma once

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
