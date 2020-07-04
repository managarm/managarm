#pragma once

namespace _debug {

enum class Condition {
	execute, write, io, readwrite
};
enum class Size {
	size1, size2, size8, size4
};

template<typename T, typename = void>
struct BreakOnWrite;

template<typename T>
struct BreakOnWrite<T, frigg::EnableIfT<sizeof(T) == 1>> {
	static void invoke(const T *p) {
		installBreak(p, Condition::write, Size::size1);
	}
};

template<typename T>
struct BreakOnWrite<T, frigg::EnableIfT<sizeof(T) == 4>> {
	static void invoke(const T *p) {
		installBreak(p, Condition::write, Size::size4);
	}
};

template<typename T>
struct BreakOnWrite<T, frigg::EnableIfT<sizeof(T) == 8>> {
	static void invoke(const T *p) {
		installBreak(p, Condition::write, Size::size8);
	}
};

inline void installBreak(const void *p, Condition condition, Size size) {
	asm volatile ("mov %0, %%db0" : : "r"(p) : "memory");

	uint64_t trigger = (uint64_t(size) << 18) | (uint64_t(condition) << 16) | (1 << 1);
	asm volatile ("mov %0, %%db7" : : "r"(trigger) : "memory");
}

} // namespace _debug

template<typename T>
void breakOnWrite(const T *p) {
	_debug::BreakOnWrite<T>::invoke(p);
}
